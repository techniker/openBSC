
/* BSC Multiplexer/NAT Utilities */

/*
 * (C) 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2010 by On-Waves
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <openbsc/bsc_nat.h>
#include <openbsc/bsc_nat_sccp.h>
#include <openbsc/bsc_msc.h>
#include <openbsc/gsm_data.h>
#include <openbsc/debug.h>
#include <openbsc/ipaccess.h>

#include <osmocore/linuxlist.h>
#include <osmocore/talloc.h>
#include <osmocore/gsm0808.h>

#include <osmocore/protocol/gsm_08_08.h>

#include <osmocom/sccp/sccp.h>

#include <netinet/in.h>
#include <arpa/inet.h>


static const struct rate_ctr_desc bsc_cfg_ctr_description[] = {
	[BCFG_CTR_SCCP_CONN]     = { "sccp.conn",      "SCCP Connections         "},
	[BCFG_CTR_SCCP_CALLS]    = { "sccp.calls",     "SCCP Assignment Commands "},
	[BCFG_CTR_NET_RECONN]    = { "net.reconnects", "Network reconnects       "},
	[BCFG_CTR_DROPPED_SCCP]  = { "dropped.sccp",   "Dropped SCCP connections."},
	[BCFG_CTR_DROPPED_CALLS] = { "dropped.calls",  "Dropped active calls.    "},
};

static const struct rate_ctr_group_desc bsc_cfg_ctrg_desc = {
	.group_name_prefix = "nat.bsc",
	.group_description = "NAT BSC Statistics",
	.num_ctr = ARRAY_SIZE(bsc_cfg_ctr_description),
	.ctr_desc = bsc_cfg_ctr_description,
};

struct bsc_nat *bsc_nat_alloc(void)
{
	struct bsc_nat *nat = talloc_zero(tall_bsc_ctx, struct bsc_nat);
	if (!nat)
		return NULL;

	INIT_LLIST_HEAD(&nat->sccp_connections);
	INIT_LLIST_HEAD(&nat->bsc_connections);
	INIT_LLIST_HEAD(&nat->bsc_configs);
	INIT_LLIST_HEAD(&nat->access_lists);

	nat->stats.sccp.conn = counter_alloc("nat.sccp.conn");
	nat->stats.sccp.calls = counter_alloc("nat.sccp.calls");
	nat->stats.bsc.reconn = counter_alloc("nat.bsc.conn");
	nat->stats.bsc.auth_fail = counter_alloc("nat.bsc.auth_fail");
	nat->stats.msc.reconn = counter_alloc("nat.msc.conn");
	nat->msc_ip = talloc_strdup(nat, "127.0.0.1");
	nat->msc_port = 5000;
	nat->auth_timeout = 2;
	nat->ping_timeout = 20;
	nat->pong_timeout = 5;
	return nat;
}

void bsc_nat_set_msc_ip(struct bsc_nat *nat, const char *ip)
{
	if (nat->msc_ip)
		talloc_free(nat->msc_ip);
	nat->msc_ip = talloc_strdup(nat, ip);
}

struct bsc_connection *bsc_connection_alloc(struct bsc_nat *nat)
{
	struct bsc_connection *con = talloc_zero(nat, struct bsc_connection);
	if (!con)
		return NULL;

	con->nat = nat;
	write_queue_init(&con->write_queue, 100);
	return con;
}

struct bsc_config *bsc_config_alloc(struct bsc_nat *nat, const char *token, unsigned int lac)
{
	struct bsc_config *conf = talloc_zero(nat, struct bsc_config);
	if (!conf)
		return NULL;

	conf->token = talloc_strdup(conf, token);
	conf->lac = lac;
	conf->nr = nat->num_bsc;
	conf->nat = nat;

	llist_add_tail(&conf->entry, &nat->bsc_configs);
	++nat->num_bsc;

	conf->stats.ctrg = rate_ctr_group_alloc(conf, &bsc_cfg_ctrg_desc, conf->lac);
	if (!conf->stats.ctrg) {
		talloc_free(conf);
		return NULL;
	}

	return conf;
}

void sccp_connection_destroy(struct sccp_connections *conn)
{
	LOGP(DNAT, LOGL_DEBUG, "Destroy 0x%x <-> 0x%x mapping for con %p\n",
	     sccp_src_ref_to_int(&conn->real_ref),
	     sccp_src_ref_to_int(&conn->patched_ref), conn->bsc);
	bsc_mgcp_dlcx(conn);
	llist_del(&conn->list_entry);
	talloc_free(conn);
}

struct bsc_connection *bsc_nat_find_bsc(struct bsc_nat *nat, struct msgb *msg, int *lac_out)
{
	struct bsc_connection *bsc;
	int data_length;
	const uint8_t *data;
	struct tlv_parsed tp;
	int i = 0;

	*lac_out = -1;

	if (!msg->l3h || msgb_l3len(msg) < 3) {
		LOGP(DNAT, LOGL_ERROR, "Paging message is too short.\n");
		return NULL;
	}

	tlv_parse(&tp, gsm0808_att_tlvdef(), msg->l3h + 3, msgb_l3len(msg) - 3, 0, 0);
	if (!TLVP_PRESENT(&tp, GSM0808_IE_CELL_IDENTIFIER_LIST)) {
		LOGP(DNAT, LOGL_ERROR, "No CellIdentifier List inside paging msg.\n");
		return NULL;
	}

	data_length = TLVP_LEN(&tp, GSM0808_IE_CELL_IDENTIFIER_LIST);
	data = TLVP_VAL(&tp, GSM0808_IE_CELL_IDENTIFIER_LIST);

	/* No need to try a different BSS */
	if (data[0] == CELL_IDENT_BSS) {
		return NULL;
	} else if (data[0] != CELL_IDENT_LAC) {
		LOGP(DNAT, LOGL_ERROR, "Unhandled cell ident discrminator: %d\n", data[0]);
		return NULL;
	}

	/* Currently we only handle one BSC */
	for (i = 1; i < data_length - 1; i += 2) {
		unsigned int _lac = ntohs(*(unsigned int *) &data[i]);
		*lac_out = _lac;
		llist_for_each_entry(bsc, &nat->bsc_connections, list_entry) {
			if (!bsc->cfg)
				continue;
			if (!bsc->authenticated || _lac != bsc->cfg->lac)
				continue;

			return bsc;
		}
	}

	return NULL;
}

int bsc_write_mgcp(struct bsc_connection *bsc, const uint8_t *data, unsigned int length)
{
	struct msgb *msg;

	if (length > 4096 - 128) {
		LOGP(DINP, LOGL_ERROR, "Can not send message of that size.\n");
		return -1;
	}

	msg = msgb_alloc_headroom(4096, 128, "to-bsc");
	if (!msg) {
		LOGP(DINP, LOGL_ERROR, "Failed to allocate memory for BSC msg.\n");
		return -1;
	}

	/* copy the data */
	msg->l3h = msgb_put(msg, length);
	memcpy(msg->l3h, data, length);

        return bsc_write(bsc, msg, NAT_IPAC_PROTO_MGCP);
}

int bsc_write(struct bsc_connection *bsc, struct msgb *msg, int proto)
{
	/* prepend the header */
	ipaccess_prepend_header(msg, proto);

	if (write_queue_enqueue(&bsc->write_queue, msg) != 0) {
		LOGP(DINP, LOGL_ERROR, "Failed to enqueue the write.\n");
		msgb_free(msg);
		return -1;
	}

	return 0;
}

static int lst_check_allow(struct bsc_nat_acc_lst *lst, const char *mi_string)
{
	struct bsc_nat_acc_lst_entry *entry;

	llist_for_each_entry(entry, &lst->fltr_list, list) {
		if (!entry->imsi_allow)
			continue;
		if (regexec(&entry->imsi_allow_re, mi_string, 0, NULL, 0) == 0)
			return 0;
	}

	return 1;
}

static int lst_check_deny(struct bsc_nat_acc_lst *lst, const char *mi_string)
{
	struct bsc_nat_acc_lst_entry *entry;

	llist_for_each_entry(entry, &lst->fltr_list, list) {
		if (!entry->imsi_deny)
			continue;
		if (regexec(&entry->imsi_deny_re, mi_string, 0, NULL, 0) == 0)
			return 0;
	}

	return 1;
}

/* apply white/black list */
static int auth_imsi(struct bsc_connection *bsc, const char *mi_string)
{
	/*
	 * Now apply blacklist/whitelist of the BSC and the NAT.
	 * 1.) Reject if the IMSI is not allowed at the BSC
	 * 2.) Allow directly if the IMSI is allowed at the BSC
	 * 3.) Reject if the IMSI not allowed at the global level.
	 * 4.) Allow directly if the IMSI is allowed at the global level
	 */
	struct bsc_nat_acc_lst *nat_lst = NULL;
	struct bsc_nat_acc_lst *bsc_lst = NULL;

	bsc_lst = bsc_nat_acc_lst_find(bsc->nat, bsc->cfg->acc_lst_name);
	nat_lst = bsc_nat_acc_lst_find(bsc->nat, bsc->nat->acc_lst_name);


	if (bsc_lst) {
		/* 1. BSC deny */
		if (lst_check_deny(bsc_lst, mi_string) == 0) {
			LOGP(DNAT, LOGL_ERROR,
			     "Filtering %s by imsi_deny on bsc nr: %d.\n", mi_string, bsc->cfg->nr);
			return -2;
		}

		/* 2. BSC allow */
		if (lst_check_allow(bsc_lst, mi_string) == 0)
			return 1;
	}

	/* 3. NAT deny */
	if (nat_lst) {
		if (lst_check_deny(nat_lst, mi_string) == 0) {
			LOGP(DNAT, LOGL_ERROR,
			     "Filtering %s by nat imsi_deny on bsc nr: %d.\n", mi_string, bsc->cfg->nr);
			return -3;
		}
	}

	return 1;
}

static int _cr_check_loc_upd(struct bsc_connection *bsc, uint8_t *data, unsigned int length)
{
	uint8_t mi_type;
	struct gsm48_loc_upd_req *lu;
	char mi_string[GSM48_MI_SIZE];

	if (length < sizeof(*lu)) {
		LOGP(DNAT, LOGL_ERROR,
		     "LU does not fit. Length is %d \n", length);
		return -1;
	}

	lu = (struct gsm48_loc_upd_req *) data;
	mi_type = lu->mi[0] & GSM_MI_TYPE_MASK;

	/*
	 * We can only deal with the IMSI. This will fail for a phone that
	 * will send the TMSI of a previous network to us.
	 */
	if (mi_type != GSM_MI_TYPE_IMSI)
		return 0;

	gsm48_mi_to_string(mi_string, sizeof(mi_string), lu->mi, lu->mi_len);
	return auth_imsi(bsc, mi_string);
}

static int _cr_check_cm_serv_req(struct bsc_connection *bsc, uint8_t *data, unsigned int length)
{
	static const uint32_t classmark_offset =
				offsetof(struct gsm48_service_request, classmark);

	char mi_string[GSM48_MI_SIZE];
	uint8_t mi_type;
	int rc;
	struct gsm48_service_request *req;

	/* unfortunately in Phase1 the classmark2 length is variable */

	if (length < sizeof(*req)) {
		LOGP(DNAT, LOGL_ERROR,
		     "CM Serv Req does not fit. Length is %d\n", length);
		return -1;
	}

	req = (struct gsm48_service_request *) data;
	rc = gsm48_extract_mi((uint8_t *) &req->classmark,
			      length - classmark_offset, mi_string, &mi_type);
	if (rc < 0) {
		LOGP(DNAT, LOGL_ERROR, "Failed to parse the classmark2/mi. error: %d\n", rc);
		return -1;
	}

	/* we have to let the TMSI or such pass */
	if (mi_type != GSM_MI_TYPE_IMSI)
		return 0;

	return auth_imsi(bsc, mi_string);
}

static int _cr_check_pag_resp(struct bsc_connection *bsc, uint8_t *data, unsigned int length)
{
	struct gsm48_pag_resp *resp;
	char mi_string[GSM48_MI_SIZE];
	uint8_t mi_type;

	if (length < sizeof(*resp)) {
		LOGP(DNAT, LOGL_ERROR, "PAG RESP does not fit. Length was %d.\n", length);
		return -1;
	}

	resp = (struct gsm48_pag_resp *) data;
	if (gsm48_paging_extract_mi(resp, length, mi_string, &mi_type) < 0) {
		LOGP(DNAT, LOGL_ERROR, "Failed to extract the MI.\n");
		return -1;
	}

	/* we need to let it pass for now */
	if (mi_type != GSM_MI_TYPE_IMSI)
		return 0;

	return auth_imsi(bsc, mi_string);
}

static int _dt_check_id_resp(struct bsc_connection *bsc,
			     uint8_t *data, unsigned int length,
			     struct sccp_connections *con)
{
	char mi_string[GSM48_MI_SIZE];
	uint8_t mi_type;
	int ret;

	if (length < 2) {
		LOGP(DNAT, LOGL_ERROR, "mi does not fit.\n");
		return -1;
	}

	if (data[0] < length - 1) {
		LOGP(DNAT, LOGL_ERROR, "mi length too big.\n");
		return -2;
	}

	mi_type = data[1] & GSM_MI_TYPE_MASK;
	gsm48_mi_to_string(mi_string, sizeof(mi_string), &data[1], data[0]);

	if (mi_type != GSM_MI_TYPE_IMSI)
		return 0;

	ret = auth_imsi(bsc, mi_string);
	con->imsi_checked = 1;
	return ret;
}

/* Filter out CR data... */
int bsc_nat_filter_sccp_cr(struct bsc_connection *bsc, struct msgb *msg, struct bsc_nat_parsed *parsed, int *con_type)
{
	struct tlv_parsed tp;
	struct gsm48_hdr *hdr48;
	int hdr48_len;
	int len;
	uint8_t msg_type;

	*con_type = NAT_CON_TYPE_NONE;

	if (parsed->gsm_type != BSS_MAP_MSG_COMPLETE_LAYER_3) {
		LOGP(DNAT, LOGL_ERROR,
		     "Rejecting CR message due wrong GSM Type %d\n", parsed->gsm_type);
		return -1;
	}

	/* the parsed has had some basic l3 length check */
	len = msg->l3h[1];
	if (msgb_l3len(msg) - 3 < len) {
		LOGP(DNAT, LOGL_ERROR,
		     "The CR Data has not enough space...\n");
		return -1;
	}

	msg->l4h = &msg->l3h[3];
	len -= 1;

	tlv_parse(&tp, gsm0808_att_tlvdef(), msg->l4h, len, 0, 0);

	if (!TLVP_PRESENT(&tp, GSM0808_IE_LAYER_3_INFORMATION)) {
		LOGP(DNAT, LOGL_ERROR, "CR Data does not contain layer3 information.\n");
		return -1;
	}

	hdr48_len = TLVP_LEN(&tp, GSM0808_IE_LAYER_3_INFORMATION);

	if (hdr48_len < sizeof(*hdr48)) {
		LOGP(DNAT, LOGL_ERROR, "GSM48 header does not fit.\n");
		return -1;
	}

	hdr48 = (struct gsm48_hdr *) TLVP_VAL(&tp, GSM0808_IE_LAYER_3_INFORMATION);

	msg_type = hdr48->msg_type & 0xbf;
	if (hdr48->proto_discr == GSM48_PDISC_MM &&
	    msg_type == GSM48_MT_MM_LOC_UPD_REQUEST) {
		*con_type = NAT_CON_TYPE_LU;
		return _cr_check_loc_upd(bsc, &hdr48->data[0], hdr48_len - sizeof(*hdr48));
	} else if (hdr48->proto_discr == GSM48_PDISC_MM &&
		  msg_type == GSM48_MT_MM_CM_SERV_REQ) {
		*con_type = NAT_CON_TYPE_CM_SERV_REQ;
		return _cr_check_cm_serv_req(bsc, &hdr48->data[0], hdr48_len - sizeof(*hdr48));
	} else if (hdr48->proto_discr == GSM48_PDISC_RR &&
		   msg_type == GSM48_MT_RR_PAG_RESP) {
		*con_type = NAT_CON_TYPE_PAG_RESP;
		return _cr_check_pag_resp(bsc, &hdr48->data[0], hdr48_len - sizeof(*hdr48));
	} else {
		/* We only want to filter the above, let other things pass */
		*con_type = NAT_CON_TYPE_OTHER;
		return 0;
	}
}

int bsc_nat_filter_dt(struct bsc_connection *bsc, struct msgb *msg,
		      struct sccp_connections *con, struct bsc_nat_parsed *parsed)
{
	uint32_t len;
	uint8_t msg_type;
	struct gsm48_hdr *hdr48;

	if (con->imsi_checked)
		return 0;

	/* only care about DTAP messages */
	if (parsed->bssap != BSSAP_MSG_DTAP)
		return 0;

	/* gsm_type is actually the size of the dtap */
	len = parsed->gsm_type;
	if (len < msgb_l3len(msg) - 3) {
		LOGP(DNAT, LOGL_ERROR, "Not enough space for DTAP.\n");
		return -1;
	}

	if (len < sizeof(*hdr48)) {
		LOGP(DNAT, LOGL_ERROR, "GSM48 header does not fit.\n");
		return -1;
	}

	msg->l4h = &msg->l3h[3];
	hdr48 = (struct gsm48_hdr *) msg->l4h;

	msg_type = hdr48->msg_type & 0xbf;
	if (hdr48->proto_discr == GSM48_PDISC_MM &&
	    msg_type == GSM48_MT_MM_ID_RESP) {
		return _dt_check_id_resp(bsc, &hdr48->data[0], len - sizeof(*hdr48), con);
	} else {
		return 0;
	}
}

void bsc_parse_reg(void *ctx, regex_t *reg, char **imsi, int argc, const char **argv)
{
	if (*imsi) {
		talloc_free(*imsi);
		*imsi = NULL;
	}
	regfree(reg);

	if (argc > 0) {
		*imsi = talloc_strdup(ctx, argv[0]);
		regcomp(reg, argv[0], 0);
	}
}

static const char *con_types [] = {
	[NAT_CON_TYPE_NONE] = "n/a",
	[NAT_CON_TYPE_LU] = "Location Update",
	[NAT_CON_TYPE_CM_SERV_REQ] = "CM Serv Req",
	[NAT_CON_TYPE_PAG_RESP] = "Paging Response",
	[NAT_CON_TYPE_LOCAL_REJECT] = "Local Reject",
	[NAT_CON_TYPE_OTHER] = "Other",
};

const char *bsc_con_type_to_string(int type)
{
	return con_types[type];
}

struct bsc_nat_acc_lst *bsc_nat_acc_lst_find(struct bsc_nat *nat, const char *name)
{
	struct bsc_nat_acc_lst *lst;

	if (!name)
		return NULL;

	llist_for_each_entry(lst, &nat->access_lists, list)
		if (strcmp(lst->name, name) == 0)
			return lst;

	return NULL;
}

struct bsc_nat_acc_lst *bsc_nat_acc_lst_get(struct bsc_nat *nat, const char *name)
{
	struct bsc_nat_acc_lst *lst;

	lst = bsc_nat_acc_lst_find(nat, name);
	if (lst)
		return lst;

	lst = talloc_zero(nat, struct bsc_nat_acc_lst);
	if (!lst) {
		LOGP(DNAT, LOGL_ERROR, "Failed to allocate access list");
		return NULL;
	}

	INIT_LLIST_HEAD(&lst->fltr_list);
	lst->name = talloc_strdup(lst, name);
	llist_add_tail(&lst->list, &nat->access_lists);
	return lst;
}

void bsc_nat_acc_lst_delete(struct bsc_nat_acc_lst *lst)
{
	llist_del(&lst->list);
	talloc_free(lst);
}

struct bsc_nat_acc_lst_entry *bsc_nat_acc_lst_entry_create(struct bsc_nat_acc_lst *lst)
{
	struct bsc_nat_acc_lst_entry *entry;

	entry = talloc_zero(lst, struct bsc_nat_acc_lst_entry);
	if (!entry)
		return NULL;

	llist_add_tail(&entry->list, &lst->fltr_list);
	return entry;
}

int bsc_nat_msc_is_connected(struct bsc_nat *nat)
{
	return nat->msc_con->is_connected;
}
