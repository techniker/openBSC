/* BSC Multiplexer/NAT */

/*
 * (C) 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2010 by On-Waves
 * (C) 2009 by Harald Welte <laforge@gnumonks.org>
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define _GNU_SOURCE
#include <getopt.h>

#include <openbsc/debug.h>
#include <openbsc/bsc_msc.h>
#include <openbsc/bsc_nat.h>
#include <openbsc/bsc_nat_sccp.h>
#include <openbsc/ipaccess.h>
#include <openbsc/abis_nm.h>
#include <openbsc/vty.h>

#include <osmocore/gsm0808.h>
#include <osmocore/talloc.h>
#include <osmocore/process.h>

#include <osmocore/protocol/gsm_08_08.h>

#include <osmocom/vty/telnet_interface.h>
#include <osmocom/vty/vty.h>

#include <osmocom/sccp/sccp.h>

#include "../../bscconfig.h"

#define SCCP_CLOSE_TIME 20
#define SCCP_CLOSE_TIME_TIMEOUT 19

struct log_target *stderr_target;
static const char *config_file = "bsc-nat.cfg";
static struct in_addr local_addr;
static struct bsc_fd bsc_listen;
static const char *msc_ip = NULL;
static struct timer_list sccp_close;
static int daemonize = 0;

const char *openbsc_copyright =
	"Copyright (C) 2010 Holger Hans Peter Freyther and On-Waves\r\n"
	"License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl.html>\r\n"
	"This is free software: you are free to change and redistribute it.\r\n"
	"There is NO WARRANTY, to the extent permitted by law.\r\n";

static struct bsc_nat *nat;
static void bsc_send_data(struct bsc_connection *bsc, const uint8_t *data, unsigned int length, int);
static void msc_send_reset(struct bsc_msc_connection *con);

struct bsc_config *bsc_config_num(struct bsc_nat *nat, int num)
{
	struct bsc_config *conf;

	llist_for_each_entry(conf, &nat->bsc_configs, entry)
		if (conf->nr == num)
			return conf;

	return NULL;
}

/*
 * below are stubs we need to link
 */
int nm_state_event(enum nm_evt evt, uint8_t obj_class, void *obj,
		   struct gsm_nm_state *old_state, struct gsm_nm_state *new_state,
		   struct abis_om_obj_inst *obj_ins)
{
	return -1;
}

void input_event(int event, enum e1inp_sign_type type, struct gsm_bts_trx *trx)
{}

static void queue_for_msc(struct bsc_msc_connection *con, struct msgb *msg) __attribute__((nonnull (1, 2)));
static void queue_for_msc(struct bsc_msc_connection *con, struct msgb *msg)
{
	if (write_queue_enqueue(&con->write_queue, msg) != 0) {
		LOGP(DINP, LOGL_ERROR, "Failed to enqueue the write.\n");
		msgb_free(msg);
	}
}

static void send_reset_ack(struct bsc_connection *bsc)
{
	static const uint8_t gsm_reset_ack[] = {
		0x09, 0x00, 0x03, 0x07, 0x0b, 0x04, 0x43, 0x01,
		0x00, 0xfe, 0x04, 0x43, 0x5c, 0x00, 0xfe, 0x03,
		0x00, 0x01, 0x31,
	};

	bsc_send_data(bsc, gsm_reset_ack, sizeof(gsm_reset_ack), IPAC_PROTO_SCCP);
}

static void send_ping(struct bsc_connection *bsc)
{
	static const uint8_t id_ping[] = {
		IPAC_MSGT_PING,
	};

	bsc_send_data(bsc, id_ping, sizeof(id_ping), IPAC_PROTO_IPACCESS);
}

static void send_pong(struct bsc_connection *bsc)
{
	static const uint8_t id_pong[] = {
		IPAC_MSGT_PONG,
	};

	bsc_send_data(bsc, id_pong, sizeof(id_pong), IPAC_PROTO_IPACCESS);
}

static void bsc_pong_timeout(void *_bsc)
{
	struct bsc_connection *bsc = _bsc;

	LOGP(DNAT, LOGL_ERROR, "BSC Nr: %d PONG timeout.\n", bsc->cfg->nr);
	bsc_close_connection(bsc);
}

static void bsc_ping_timeout(void *_bsc)
{
	struct bsc_connection *bsc = _bsc;

	if (bsc->nat->ping_timeout < 0)
		return;

	send_ping(bsc);

	/* send another ping in 20 seconds */
	bsc_schedule_timer(&bsc->ping_timeout, bsc->nat->ping_timeout, 0);

	/* also start a pong timer */
	bsc_schedule_timer(&bsc->pong_timeout, bsc->nat->pong_timeout, 0);
}

static void start_ping_pong(struct bsc_connection *bsc)
{
	bsc->pong_timeout.data = bsc;
	bsc->pong_timeout.cb = bsc_pong_timeout;
	bsc->ping_timeout.data = bsc;
	bsc->ping_timeout.cb = bsc_ping_timeout;

	bsc_ping_timeout(bsc);
}

static void send_id_ack(struct bsc_connection *bsc)
{
	static const uint8_t id_ack[] = {
		IPAC_MSGT_ID_ACK
	};

	bsc_send_data(bsc, id_ack, sizeof(id_ack), IPAC_PROTO_IPACCESS);
}

static void send_id_req(struct bsc_connection *bsc)
{
	static const uint8_t id_req[] = {
		IPAC_MSGT_ID_GET,
		0x01, IPAC_IDTAG_UNIT,
		0x01, IPAC_IDTAG_MACADDR,
		0x01, IPAC_IDTAG_LOCATION1,
		0x01, IPAC_IDTAG_LOCATION2,
		0x01, IPAC_IDTAG_EQUIPVERS,
		0x01, IPAC_IDTAG_SWVERSION,
		0x01, IPAC_IDTAG_UNITNAME,
		0x01, IPAC_IDTAG_SERNR,
	};

	bsc_send_data(bsc, id_req, sizeof(id_req), IPAC_PROTO_IPACCESS);
}

static void nat_send_rlsd(struct sccp_connections *conn)
{
	struct sccp_connection_released *rel;
	struct msgb *msg;

	msg = msgb_alloc_headroom(4096, 128, "rlsd");
	if (!msg) {
		LOGP(DNAT, LOGL_ERROR, "Failed to allocate clear command.\n");
		return;
	}

	msg->l2h = msgb_put(msg, sizeof(*rel));
	rel = (struct sccp_connection_released *) msg->l2h;
	rel->type = SCCP_MSG_TYPE_RLSD;
	rel->release_cause = SCCP_RELEASE_CAUSE_SCCP_FAILURE;
	rel->destination_local_reference = conn->remote_ref;
	rel->source_local_reference = conn->patched_ref;

	ipaccess_prepend_header(msg, IPAC_PROTO_SCCP);

	queue_for_msc(conn->msc_con, msg);
}

static void nat_send_rlc(struct bsc_msc_connection *msc_con,
			 struct sccp_source_reference *src,
			 struct sccp_source_reference *dst)
{
	struct sccp_connection_release_complete *rlc;
	struct msgb *msg;

	msg = msgb_alloc_headroom(4096, 128, "rlc");
	if (!msg) {
		LOGP(DNAT, LOGL_ERROR, "Failed to allocate clear command.\n");
		return;
	}

	msg->l2h = msgb_put(msg, sizeof(*rlc));
	rlc = (struct sccp_connection_release_complete *) msg->l2h;
	rlc->type = SCCP_MSG_TYPE_RLC;
	rlc->destination_local_reference = *dst;
	rlc->source_local_reference = *src;

	ipaccess_prepend_header(msg, IPAC_PROTO_SCCP);

	queue_for_msc(msc_con, msg);
}

static void send_mgcp_reset(struct bsc_connection *bsc)
{
	static const uint8_t mgcp_reset[] = {
	    "RSIP 1 13@mgw MGCP 1.0\r\n"
	};

	bsc_write_mgcp(bsc, mgcp_reset, sizeof mgcp_reset - 1);
}

/*
 * Below is the handling of messages coming
 * from the MSC and need to be forwarded to
 * a real BSC.
 */
static void initialize_msc_if_needed(struct bsc_msc_connection *msc_con)
{
	if (msc_con->first_contact)
		return;

	msc_con->first_contact = 1;
	msc_send_reset(msc_con);
}

static void send_id_get_response(struct bsc_msc_connection *msc_con)
{
	struct msgb *msg = bsc_msc_id_get_resp(nat->token);
	if (!msg)
		return;

	ipaccess_prepend_header(msg, IPAC_PROTO_IPACCESS);
	queue_for_msc(msc_con, msg);
}

/*
 * Currently we are lacking refcounting so we need to copy each message.
 */
static void bsc_send_data(struct bsc_connection *bsc, const uint8_t *data, unsigned int length, int proto)
{
	struct msgb *msg;

	if (length > 4096 - 128) {
		LOGP(DINP, LOGL_ERROR, "Can not send message of that size.\n");
		return;
	}

	msg = msgb_alloc_headroom(4096, 128, "to-bsc");
	if (!msg) {
		LOGP(DINP, LOGL_ERROR, "Failed to allocate memory for BSC msg.\n");
		return;
	}

	msg->l2h = msgb_put(msg, length);
	memcpy(msg->data, data, length);

	bsc_write(bsc, msg, proto);
}

/*
 * Release an established connection. We will have to release it to the BSC
 * and to the network and we do it the following way.
 * 1.) Give up on the MSC side
 *  1.1) Send a RLSD message, it is a bit non standard but should work, we
 *       ignore the RLC... we might complain about it. Other options would
 *       be to send a Release Request, handle the Release Complete..
 *  1.2) Mark the data structure to be con_local and wait for 2nd
 *
 * 2.) Give up on the BSC side
 *  2.1) Depending on the con type reject the service, or just close it
 */
static void bsc_send_con_release(struct bsc_connection *bsc, struct sccp_connections *con)
{
	struct msgb *rlsd;
	/* 1. release the network */
	rlsd = sccp_create_rlsd(&con->patched_ref, &con->remote_ref,
				SCCP_RELEASE_CAUSE_END_USER_ORIGINATED);
	if (!rlsd)
		LOGP(DNAT, LOGL_ERROR, "Failed to create RLSD message.\n");
	else {
		ipaccess_prepend_header(rlsd, IPAC_PROTO_SCCP);
		queue_for_msc(con->msc_con, rlsd);
	}
	con->con_local = 1;
	con->msc_con = NULL;

	/* 2. release the BSC side */
	if (con->con_type == NAT_CON_TYPE_LU) {
		struct msgb *payload, *udt;
		payload = gsm48_create_loc_upd_rej(GSM48_REJECT_PLMN_NOT_ALLOWED);

		if (payload) {
			gsm0808_prepend_dtap_header(payload, 0);
			udt = sccp_create_dt1(&con->real_ref, payload->data, payload->len);
			if (udt)
				bsc_write(bsc, udt, IPAC_PROTO_SCCP);
			else
				LOGP(DNAT, LOGL_ERROR, "Failed to create DT1\n");

			msgb_free(payload);
		} else {
			LOGP(DNAT, LOGL_ERROR, "Failed to allocate LU Reject.\n");
		}
	}

	rlsd = sccp_create_rlsd(&con->remote_ref, &con->real_ref,
				SCCP_RELEASE_CAUSE_END_USER_ORIGINATED);
	if (!rlsd) {
		LOGP(DNAT, LOGL_ERROR, "Failed to allocate RLSD for the BSC.\n");
		sccp_connection_destroy(con);
		return;
	}

	con->con_type = NAT_CON_TYPE_LOCAL_REJECT;
	bsc_write(bsc, rlsd, IPAC_PROTO_SCCP);
}

static void bsc_send_con_refuse(struct bsc_connection *bsc,
				struct bsc_nat_parsed *parsed, int con_type)
{
	struct msgb *payload;
	struct msgb *refuse;

	if (con_type == NAT_CON_TYPE_LU)
		payload = gsm48_create_loc_upd_rej(GSM48_REJECT_PLMN_NOT_ALLOWED);
	else if (con_type == NAT_CON_TYPE_CM_SERV_REQ)
		payload = gsm48_create_mm_serv_rej(GSM48_REJECT_PLMN_NOT_ALLOWED);
	else {
		LOGP(DNAT, LOGL_ERROR, "Unknown connection type: %d\n", con_type);
		payload = NULL;
	}

	/*
	 * Some BSCs do not handle the payload inside a SCCP CREF msg
	 * so we will need to:
	 * 1.) Allocate a local connection and mark it as local..
	 * 2.) queue data for downstream.. and the RLC should delete everything
	 */
	if (payload) {
		struct msgb *cc, *udt, *rlsd;
		struct sccp_connections *con;
		con = create_sccp_src_ref(bsc, parsed);
		if (!con)
			goto send_refuse;

		/* declare it local and assign a unique remote_ref */
		con->con_type = NAT_CON_TYPE_LOCAL_REJECT;
		con->con_local = 1;
		con->has_remote_ref = 1;
		con->remote_ref = con->patched_ref;

		/* 1. create a confirmation */
		cc = sccp_create_cc(&con->remote_ref, &con->real_ref);
		if (!cc)
			goto send_refuse;

		/* 2. create the DT1 */
		gsm0808_prepend_dtap_header(payload, 0);
		udt = sccp_create_dt1(&con->real_ref, payload->data, payload->len);
		if (!udt) {
			msgb_free(cc);
			goto send_refuse;
		}

		/* 3. send a RLSD */
		rlsd = sccp_create_rlsd(&con->remote_ref, &con->real_ref,
					SCCP_RELEASE_CAUSE_END_USER_ORIGINATED);
		if (!rlsd) {
			msgb_free(cc);
			msgb_free(udt);
			goto send_refuse;
		}

		bsc_write(bsc, cc, IPAC_PROTO_SCCP);
		bsc_write(bsc, udt, IPAC_PROTO_SCCP);
		bsc_write(bsc, rlsd, IPAC_PROTO_SCCP);
		msgb_free(payload);
		return;
	}


send_refuse:
	if (payload)
		msgb_free(payload);

	refuse = sccp_create_refuse(parsed->src_local_ref,
				    SCCP_REFUSAL_SCCP_FAILURE, NULL, 0);
	if (!refuse) {
		LOGP(DNAT, LOGL_ERROR,
		     "Creating refuse msg failed for SCCP 0x%x on BSC Nr: %d.\n",
		      sccp_src_ref_to_int(parsed->src_local_ref), bsc->cfg->nr);
		return;
	}

	bsc_write(bsc, refuse, IPAC_PROTO_SCCP);
}


static int forward_sccp_to_bts(struct bsc_msc_connection *msc_con, struct msgb *msg)
{
	struct sccp_connections *con = NULL;
	struct bsc_connection *bsc;
	struct bsc_nat_parsed *parsed;
	int proto;

	/* filter, drop, patch the message? */
	parsed = bsc_nat_parse(msg);
	if (!parsed) {
		LOGP(DNAT, LOGL_ERROR, "Can not parse msg from BSC.\n");
		return -1;
	}

	if (bsc_nat_filter_ipa(DIR_BSC, msg, parsed))
		goto exit;

	proto = parsed->ipa_proto;

	/* Route and modify the SCCP packet */
	if (proto == IPAC_PROTO_SCCP) {
		switch (parsed->sccp_type) {
		case SCCP_MSG_TYPE_UDT:
			/* forward UDT messages to every BSC */
			goto send_to_all;
			break;
		case SCCP_MSG_TYPE_RLSD:
		case SCCP_MSG_TYPE_CREF:
		case SCCP_MSG_TYPE_DT1:
		case SCCP_MSG_TYPE_IT:
			con = patch_sccp_src_ref_to_bsc(msg, parsed, nat);
			if (parsed->gsm_type == BSS_MAP_MSG_ASSIGMENT_RQST) {
				counter_inc(nat->stats.sccp.calls);

				if (con) {
					struct rate_ctr_group *ctrg;
					ctrg = con->bsc->cfg->stats.ctrg;
					rate_ctr_inc(&ctrg->ctr[BCFG_CTR_SCCP_CALLS]);
					if (bsc_mgcp_assign_patch(con, msg) != 0)
						LOGP(DNAT, LOGL_ERROR, "Failed to assign...\n");
				} else
					LOGP(DNAT, LOGL_ERROR, "Assignment command but no BSC.\n");
			}
			break;
		case SCCP_MSG_TYPE_CC:
			con = patch_sccp_src_ref_to_bsc(msg, parsed, nat);
			if (!con || update_sccp_src_ref(con, parsed) != 0)
				goto exit;
			break;
		case SCCP_MSG_TYPE_RLC:
			LOGP(DNAT, LOGL_ERROR, "Unexpected release complete from MSC.\n");
			goto exit;
			break;
		case SCCP_MSG_TYPE_CR:
			/* MSC never opens a SCCP connection, fall through */
		default:
			goto exit;
		}

		if (!con && parsed->sccp_type == SCCP_MSG_TYPE_RLSD) {
			LOGP(DNAT, LOGL_NOTICE, "Sending fake RLC on RLSD message to network.\n");
			/* Exchange src/dest for the reply */
			nat_send_rlc(msc_con, parsed->dest_local_ref, parsed->src_local_ref);
		} else if (!con)
			LOGP(DNAT, LOGL_ERROR, "Unknown connection for msg type: 0x%x from the MSC.\n", parsed->sccp_type);
	}

	talloc_free(parsed);
	if (!con)
		return -1;
	if (!con->bsc->authenticated) {
		LOGP(DNAT, LOGL_ERROR, "Selected BSC not authenticated.\n");
		return -1;
	}

	bsc_send_data(con->bsc, msg->l2h, msgb_l2len(msg), proto);
	return 0;

send_to_all:
	/*
	 * Filter Paging from the network. We do not want to send a PAGING
	 * Command to every BSC in our network. We will analys the PAGING
	 * message and then send it to the authenticated messages...
	 */
	if (parsed->ipa_proto == IPAC_PROTO_SCCP && parsed->gsm_type == BSS_MAP_MSG_PAGING) {
		int lac;
		bsc = bsc_nat_find_bsc(nat, msg, &lac);
		if (bsc && bsc->cfg->forbid_paging)
			LOGP(DNAT, LOGL_DEBUG, "Paging forbidden for BTS: %d\n", bsc->cfg->nr);
		else if (bsc)
			bsc_send_data(bsc, msg->l2h, msgb_l2len(msg), parsed->ipa_proto);
		else if (lac != -1)
			LOGP(DNAT, LOGL_ERROR, "Could not determine BSC for paging on lac: %d/0x%x\n",
			     lac, lac);

		goto exit;
	}
	/* currently send this to every BSC connected */
	llist_for_each_entry(bsc, &nat->bsc_connections, list_entry) {
		if (!bsc->authenticated)
			continue;

		bsc_send_data(bsc, msg->l2h, msgb_l2len(msg), parsed->ipa_proto);
	}

exit:
	talloc_free(parsed);
	return 0;
}

static void msc_connection_was_lost(struct bsc_msc_connection *con)
{
	struct bsc_connection *bsc, *tmp;

	LOGP(DMSC, LOGL_ERROR, "Closing all connections downstream.\n");
	llist_for_each_entry_safe(bsc, tmp, &nat->bsc_connections, list_entry)
		bsc_close_connection(bsc);

	bsc_mgcp_free_endpoints(nat);
	bsc_msc_schedule_connect(con);
}

static void msc_connection_connected(struct bsc_msc_connection *con)
{
	counter_inc(nat->stats.msc.reconn);
}

static void msc_send_reset(struct bsc_msc_connection *msc_con)
{
	static const uint8_t reset[] = {
		0x00, 0x12, 0xfd,
		0x09, 0x00, 0x03, 0x05, 0x07, 0x02, 0x42, 0xfe,
		0x02, 0x42, 0xfe, 0x06, 0x00, 0x04, 0x30, 0x04,
		0x01, 0x20
	};

	struct msgb *msg;

	msg = msgb_alloc_headroom(4096, 128, "08.08 reset");
	if (!msg) {
		LOGP(DMSC, LOGL_ERROR, "Failed to allocate reset msg.\n");
		return;
	}

	msg->l2h = msgb_put(msg, sizeof(reset));
	memcpy(msg->l2h, reset, msgb_l2len(msg));

	queue_for_msc(msc_con, msg);

	LOGP(DMSC, LOGL_NOTICE, "Scheduled GSM0808 reset msg for the MSC.\n");
}

static int ipaccess_msc_read_cb(struct bsc_fd *bfd)
{
	int error;
	struct bsc_msc_connection *msc_con;
	struct msgb *msg = ipaccess_read_msg(bfd, &error);
	struct ipaccess_head *hh;

	msc_con = (struct bsc_msc_connection *) bfd->data;

	if (!msg) {
		if (error == 0)
			LOGP(DNAT, LOGL_FATAL, "The connection the MSC was lost, exiting\n");
		else
			LOGP(DNAT, LOGL_ERROR, "Failed to parse ip access message: %d\n", error);

		bsc_msc_lost(msc_con);
		return -1;
	}

	LOGP(DNAT, LOGL_DEBUG, "MSG from MSC: %s proto: %d\n", hexdump(msg->data, msg->len), msg->l2h[0]);

	/* handle base message handling */
	hh = (struct ipaccess_head *) msg->data;
	ipaccess_rcvmsg_base(msg, bfd);

	/* initialize the networking. This includes sending a GSM08.08 message */
	if (hh->proto == IPAC_PROTO_IPACCESS) {
		if (msg->l2h[0] == IPAC_MSGT_ID_ACK)
			initialize_msc_if_needed(msc_con);
		else if (msg->l2h[0] == IPAC_MSGT_ID_GET)
			send_id_get_response(msc_con);
	} else if (hh->proto == IPAC_PROTO_SCCP)
		forward_sccp_to_bts(msc_con, msg);

	msgb_free(msg);
	return 0;
}

static int ipaccess_msc_write_cb(struct bsc_fd *bfd, struct msgb *msg)
{
	int rc;
	rc = write(bfd->fd, msg->data, msg->len);

	if (rc != msg->len) {
		LOGP(DNAT, LOGL_ERROR, "Failed to write MSG to MSC.\n");
		return -1;
	}

	return rc;
}

/*
 * Below is the handling of messages coming
 * from the BSC and need to be forwarded to
 * a real BSC.
 */

/*
 * Remove the connection from the connections list,
 * remove it from the patching of SCCP header lists
 * as well. Maybe in the future even close connection..
 */
void bsc_close_connection(struct bsc_connection *connection)
{
	struct sccp_connections *sccp_patch, *tmp;
	struct rate_ctr *ctr = NULL;

	/* stop the timeout timer */
	bsc_del_timer(&connection->id_timeout);
	bsc_del_timer(&connection->ping_timeout);
	bsc_del_timer(&connection->pong_timeout);

	if (connection->cfg)
		ctr = &connection->cfg->stats.ctrg->ctr[BCFG_CTR_DROPPED_SCCP];

	/* remove all SCCP connections */
	llist_for_each_entry_safe(sccp_patch, tmp, &nat->sccp_connections, list_entry) {
		if (sccp_patch->bsc != connection)
			continue;

		if (ctr)
			rate_ctr_inc(ctr);
		if (sccp_patch->has_remote_ref && !sccp_patch->con_local)
			nat_send_rlsd(sccp_patch);
		sccp_connection_destroy(sccp_patch);
	}

	/* close endpoints allocated by this BSC */
	bsc_mgcp_clear_endpoints_for(connection);

	bsc_unregister_fd(&connection->write_queue.bfd);
	close(connection->write_queue.bfd.fd);
	write_queue_clear(&connection->write_queue);
	llist_del(&connection->list_entry);

	talloc_free(connection);
}

static void ipaccess_close_bsc(void *data)
{
	struct sockaddr_in sock;
	socklen_t len = sizeof(sock);
	struct bsc_connection *conn = data;


	getpeername(conn->write_queue.bfd.fd, (struct sockaddr *) &sock, &len);
	LOGP(DNAT, LOGL_ERROR, "BSC on %s didn't respond to identity request. Closing.\n",
	     inet_ntoa(sock.sin_addr));
	bsc_close_connection(conn);
}

static void ipaccess_auth_bsc(struct tlv_parsed *tvp, struct bsc_connection *bsc)
{
	struct bsc_config *conf;
	const char *token = (const char *) TLVP_VAL(tvp, IPAC_IDTAG_UNITNAME);

	if (bsc->cfg) {
		LOGP(DNAT, LOGL_ERROR, "Reauth on fd %d bsc nr %d\n",
		     bsc->write_queue.bfd.fd, bsc->cfg->nr);
		return;
	}

	llist_for_each_entry(conf, &bsc->nat->bsc_configs, entry) {
		if (strcmp(conf->token, token) == 0) {
			rate_ctr_inc(&conf->stats.ctrg->ctr[BCFG_CTR_NET_RECONN]);
			bsc->authenticated = 1;
			bsc->cfg = conf;
			bsc_del_timer(&bsc->id_timeout);
			LOGP(DNAT, LOGL_NOTICE, "Authenticated bsc nr: %d lac: %d on fd %d\n",
			     conf->nr, conf->lac, bsc->write_queue.bfd.fd);
			start_ping_pong(bsc);
			return;
		}
	}

	LOGP(DNAT, LOGL_ERROR, "No bsc found for token %s on fd: %d.\n", token,
	     bsc->write_queue.bfd.fd);
}

static int forward_sccp_to_msc(struct bsc_connection *bsc, struct msgb *msg)
{
	int con_filter = 0;
	struct bsc_msc_connection *con_msc = NULL;
	struct bsc_connection *con_bsc = NULL;
	int con_type;
	struct bsc_nat_parsed *parsed;

	/* Parse and filter messages */
	parsed = bsc_nat_parse(msg);
	if (!parsed) {
		LOGP(DNAT, LOGL_ERROR, "Can not parse msg from BSC.\n");
		msgb_free(msg);
		return -1;
	}

	if (bsc_nat_filter_ipa(DIR_MSC, msg, parsed))
		goto exit;

	/*
	 * check authentication after filtering to not reject auth
	 * responses coming from the BSC. We have to make sure that
	 * nothing from the exit path will forward things to the MSC
	 */
	if (!bsc->authenticated) {
		LOGP(DNAT, LOGL_ERROR, "BSC is not authenticated.\n");
		msgb_free(msg);
		return -1;
	}


	/* modify the SCCP entries */
	if (parsed->ipa_proto == IPAC_PROTO_SCCP) {
		int filter;
		struct sccp_connections *con;
		switch (parsed->sccp_type) {
		case SCCP_MSG_TYPE_CR:
			filter = bsc_nat_filter_sccp_cr(bsc, msg, parsed, &con_type);
			if (filter < 0)
				goto exit3;
			if (!create_sccp_src_ref(bsc, parsed))
				goto exit2;
			con = patch_sccp_src_ref_to_msc(msg, parsed, bsc);
			con->msc_con = bsc->nat->msc_con;
			con_msc = con->msc_con;
			con->con_type = con_type;
			con->imsi_checked = filter;
			con_bsc = con->bsc;
			break;
		case SCCP_MSG_TYPE_RLSD:
		case SCCP_MSG_TYPE_CREF:
		case SCCP_MSG_TYPE_DT1:
		case SCCP_MSG_TYPE_CC:
		case SCCP_MSG_TYPE_IT:
			con = patch_sccp_src_ref_to_msc(msg, parsed, bsc);
			if (con) {
				filter = bsc_nat_filter_dt(bsc, msg, con, parsed);
				if (filter < 0) {
					bsc_send_con_release(bsc, con);
					con = NULL;
					goto exit2;
				} else {
					con_bsc = con->bsc;
					con_msc = con->msc_con;
					con_filter = con->con_local;
				}
			}
			break;
		case SCCP_MSG_TYPE_RLC:
			con = patch_sccp_src_ref_to_msc(msg, parsed, bsc);
			if (con) {
				con_bsc = con->bsc;
				con_msc = con->msc_con;
				con_filter = con->con_local;
			}
			remove_sccp_src_ref(bsc, msg, parsed);
			break;
		case SCCP_MSG_TYPE_UDT:
			/* simply forward everything */
			con = NULL;
			break;
		default:
			LOGP(DNAT, LOGL_ERROR, "Not forwarding to msc sccp type: 0x%x\n", parsed->sccp_type);
			con = NULL;
			goto exit2;
			break;
		}
        } else if (parsed->ipa_proto == NAT_IPAC_PROTO_MGCP) {
                bsc_mgcp_forward(bsc, msg);
                goto exit2;
	} else {
		LOGP(DNAT, LOGL_ERROR, "Not forwarding unknown stream id: 0x%x\n", parsed->ipa_proto);
		goto exit2;
	}

	if (con_msc && con_bsc != bsc) {
		LOGP(DNAT, LOGL_ERROR, "The connection belongs to a different BTS: input: %d con: %d\n",
		     bsc->cfg->nr, con_bsc->cfg->nr);
		goto exit2;
	}

	/* do not forward messages to the MSC */
	if (con_filter)
		goto exit2;

	if (!con_msc) {
		LOGP(DNAT, LOGL_ERROR, "Not forwarding data bsc_nr: %d ipa: %d type: 0x%x\n",
			bsc->cfg->nr,
			parsed ? parsed->ipa_proto : -1,
			parsed ? parsed->sccp_type : -1);
		goto exit2;
	}

	/* send the non-filtered but maybe modified msg */
	queue_for_msc(con_msc, msg);
	talloc_free(parsed);
	return 0;

exit:
	/* if we filter out the reset send an ack to the BSC */
	if (parsed->bssap == 0 && parsed->gsm_type == BSS_MAP_MSG_RESET) {
		send_reset_ack(bsc);
		send_reset_ack(bsc);
	} else if (parsed->ipa_proto == IPAC_PROTO_IPACCESS) {
		/* do we know who is handling this? */
		if (msg->l2h[0] == IPAC_MSGT_ID_RESP) {
			struct tlv_parsed tvp;
			ipaccess_idtag_parse(&tvp,
					     (unsigned char *) msg->l2h + 2,
					     msgb_l2len(msg) - 2);
			if (TLVP_PRESENT(&tvp, IPAC_IDTAG_UNITNAME))
				ipaccess_auth_bsc(&tvp, bsc);
		}

		goto exit2;
	}

exit2:
	talloc_free(parsed);
	msgb_free(msg);
	return -1;

exit3:
	/* send a SCCP Connection Refused */
	bsc_send_con_refuse(bsc, parsed, con_type);
	talloc_free(parsed);
	msgb_free(msg);
	return -1;
}

static int ipaccess_bsc_read_cb(struct bsc_fd *bfd)
{
	int error;
	struct bsc_connection *bsc = bfd->data;
	struct msgb *msg = ipaccess_read_msg(bfd, &error);
	struct ipaccess_head *hh;

	if (!msg) {
		if (error == 0)
			LOGP(DNAT, LOGL_ERROR,
			     "The connection to the BSC Nr: %d was lost. Cleaning it\n",
			     bsc->cfg ? bsc->cfg->nr : -1);
		else
			LOGP(DNAT, LOGL_ERROR,
			     "Stream error on BSC Nr: %d. Failed to parse ip access message: %d\n",
			     bsc->cfg ? bsc->cfg->nr : -1, error);

		bsc_close_connection(bsc);
		return -1;
	}


	LOGP(DNAT, LOGL_DEBUG, "MSG from BSC: %s proto: %d\n", hexdump(msg->data, msg->len), msg->l2h[0]);

	/* Handle messages from the BSC */
	hh = (struct ipaccess_head *) msg->data;

	/* stop the pong timeout */
	if (hh->proto == IPAC_PROTO_IPACCESS) {
		if (msg->l2h[0] == IPAC_MSGT_PONG) {
			bsc_del_timer(&bsc->pong_timeout);
			msgb_free(msg);
			return 0;
		} else if (msg->l2h[0] == IPAC_MSGT_PING) {
			send_pong(bsc);
			msgb_free(msg);
			return 0;
		}
	}

	/* FIXME: Currently no PONG is sent to the BSC */
	/* FIXME: Currently no ID ACK is sent to the BSC */
	forward_sccp_to_msc(bsc, msg);

	return 0;
}

static int ipaccess_bsc_write_cb(struct bsc_fd *bfd, struct msgb *msg)
{
	int rc;

	rc = write(bfd->fd, msg->data, msg->len);
	if (rc != msg->len)
		LOGP(DNAT, LOGL_ERROR, "Failed to write message to the BSC.\n");

	return rc;
}

static int ipaccess_listen_bsc_cb(struct bsc_fd *bfd, unsigned int what)
{
	struct bsc_connection *bsc;
	int fd, rc, on;
	struct sockaddr_in sa;
	socklen_t sa_len = sizeof(sa);

	if (!(what & BSC_FD_READ))
		return 0;

	fd = accept(bfd->fd, (struct sockaddr *) &sa, &sa_len);
	if (fd < 0) {
		perror("accept");
		return fd;
	}

	/* count the reconnect */
	counter_inc(nat->stats.bsc.reconn);

	/*
	 * if we are not connected to a msc... just close the socket
	 */
	if (!bsc_nat_msc_is_connected(nat)) {
		LOGP(DNAT, LOGL_NOTICE, "Disconnecting BSC due lack of MSC connection.\n");
		close(fd);
		return 0;
	}

	on = 1;
	rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	if (rc != 0)
                LOGP(DNAT, LOGL_ERROR, "Failed to set TCP_NODELAY: %s\n", strerror(errno));

	rc = setsockopt(fd, IPPROTO_IP, IP_TOS,
			&nat->bsc_ip_dscp, sizeof(nat->bsc_ip_dscp));
	if (rc != 0)
		LOGP(DNAT, LOGL_ERROR, "Failed to set IP_TOS: %s\n", strerror(errno));

	/* todo... do something with the connection */
	/* todo... use GNUtls to see if we want to trust this as a BTS */

	/*
	 *
	 */
	bsc = bsc_connection_alloc(nat);
	if (!bsc) {
		LOGP(DNAT, LOGL_ERROR, "Failed to allocate BSC struct.\n");
		close(fd);
		return -1;
	}

	bsc->write_queue.bfd.data = bsc;
	bsc->write_queue.bfd.fd = fd;
	bsc->write_queue.read_cb = ipaccess_bsc_read_cb;
	bsc->write_queue.write_cb = ipaccess_bsc_write_cb;
	bsc->write_queue.bfd.when = BSC_FD_READ;
	if (bsc_register_fd(&bsc->write_queue.bfd) < 0) {
		LOGP(DNAT, LOGL_ERROR, "Failed to register BSC fd.\n");
		close(fd);
		talloc_free(bsc);
		return -2;
	}

	LOGP(DNAT, LOGL_NOTICE, "BSC connection on %d with IP: %s\n",
		fd, inet_ntoa(sa.sin_addr));
	llist_add(&bsc->list_entry, &nat->bsc_connections);
	send_id_ack(bsc);
	send_id_req(bsc);
	send_mgcp_reset(bsc);

	/*
	 * start the hangup timer
	 */
	bsc->id_timeout.data = bsc;
	bsc->id_timeout.cb = ipaccess_close_bsc;
	bsc_schedule_timer(&bsc->id_timeout, nat->auth_timeout, 0);
	return 0;
}

static int listen_for_bsc(struct bsc_fd *bfd, struct in_addr *in_addr, int port)
{
	struct sockaddr_in addr;
	int ret, on = 1;

	bfd->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	bfd->cb = ipaccess_listen_bsc_cb;
	bfd->when = BSC_FD_READ;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = in_addr->s_addr;

	setsockopt(bfd->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	ret = bind(bfd->fd, (struct sockaddr *) &addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Could not bind the BSC socket: %s\n",
			strerror(errno));
		return -EIO;
	}

	ret = listen(bfd->fd, 1);
	if (ret < 0) {
		perror("listen");
		return ret;
	}

	ret = bsc_register_fd(bfd);
	if (ret < 0) {
		perror("register_listen_fd");
		return ret;
	}
	return 0;
}

static void print_usage()
{
	printf("Usage: bsc_nat\n");
}

static void print_help()
{
	printf("  Some useful help...\n");
	printf("  -h --help this text\n");
	printf("  -d option --debug=DRLL:DCC:DMM:DRR:DRSL:DNM enable debugging\n");
	printf("  -D --daemonize Fork the process into a background daemon\n");
	printf("  -s --disable-color\n");
	printf("  -c --config-file filename The config file to use.\n");
	printf("  -m --msc=IP. The address of the MSC.\n");
	printf("  -l --local=IP. The local address of this BSC.\n");
}

static void handle_options(int argc, char **argv)
{
	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
			{"help", 0, 0, 'h'},
			{"debug", 1, 0, 'd'},
			{"config-file", 1, 0, 'c'},
			{"disable-color", 0, 0, 's'},
			{"timestamp", 0, 0, 'T'},
			{"msc", 1, 0, 'm'},
			{"local", 1, 0, 'l'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "hd:sTPc:m:l:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_usage();
			print_help();
			exit(0);
		case 's':
			log_set_use_color(stderr_target, 0);
			break;
		case 'd':
			log_parse_category_mask(stderr_target, optarg);
			break;
		case 'c':
			config_file = strdup(optarg);
			break;
		case 'T':
			log_set_print_timestamp(stderr_target, 1);
			break;
		case 'm':
			msc_ip = optarg;
			break;
		case 'l':
			inet_aton(optarg, &local_addr);
			break;
		default:
			/* ignore */
			break;
		}
	}
}

static void signal_handler(int signal)
{
	switch (signal) {
	case SIGABRT:
		/* in case of abort, we want to obtain a talloc report
		 * and then return to the caller, who will abort the process */
	case SIGUSR1:
		talloc_report_full(tall_bsc_ctx, stderr);
		break;
	default:
		break;
	}
}

static void sccp_close_unconfirmed(void *_data)
{
	struct sccp_connections *conn, *tmp1;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	llist_for_each_entry_safe(conn, tmp1, &nat->sccp_connections, list_entry) {
		if (conn->has_remote_ref)
			continue;

		int diff = (now.tv_sec - conn->creation_time.tv_sec) / 60;
		if (diff < SCCP_CLOSE_TIME_TIMEOUT)
			continue;

		LOGP(DNAT, LOGL_ERROR, "SCCP connection 0x%x/0x%x was never confirmed.\n",
		     sccp_src_ref_to_int(&conn->real_ref),
		     sccp_src_ref_to_int(&conn->patched_ref));
		sccp_connection_destroy(conn);
	}

	bsc_schedule_timer(&sccp_close, SCCP_CLOSE_TIME, 0);
}

extern void *tall_msgb_ctx;
extern void *tall_ctr_ctx;
static void talloc_init_ctx()
{
	tall_bsc_ctx = talloc_named_const(NULL, 0, "nat");
	tall_msgb_ctx = talloc_named_const(tall_bsc_ctx, 0, "msgb");
	tall_ctr_ctx = talloc_named_const(tall_bsc_ctx, 0, "counter");
}

extern enum node_type bsc_vty_go_parent(struct vty *vty);

static struct vty_app_info vty_info = {
	.name 		= "BSC NAT",
	.version	= PACKAGE_VERSION,
	.go_parent_cb	= bsc_vty_go_parent,
	.is_config_node	= bsc_vty_is_config_node,
};

int main(int argc, char **argv)
{
	int rc;

	talloc_init_ctx();

	log_init(&log_info);
	stderr_target = log_target_create_stderr();
	log_add_target(stderr_target);
	log_set_all_filter(stderr_target, 1);

	nat = bsc_nat_alloc();
	if (!nat) {
		fprintf(stderr, "Failed to allocate the BSC nat.\n");
		return -4;
	}

	nat->mgcp_cfg = mgcp_config_alloc();
	if (!nat->mgcp_cfg) {
		fprintf(stderr, "Failed to allocate MGCP cfg.\n");
		return -5;
	}

	vty_info.copyright = openbsc_copyright;
	vty_init(&vty_info);
	logging_vty_add_cmds();
	bsc_nat_vty_init(nat);


	/* parse options */
	local_addr.s_addr = INADDR_ANY;
	handle_options(argc, argv);

	rate_ctr_init(tall_bsc_ctx);

	/* init vty and parse */
	telnet_init(tall_bsc_ctx, NULL, 4244);
	if (mgcp_parse_config(config_file, nat->mgcp_cfg) < 0) {
		fprintf(stderr, "Failed to parse the config file: '%s'\n", config_file);
		return -3;
	}

	/* over rule the VTY config */
	if (msc_ip)
		bsc_nat_set_msc_ip(nat, msc_ip);

	/* seed the PRNG */
	srand(time(NULL));

	/*
	 * Setup the MGCP code..
	 */
	if (bsc_mgcp_nat_init(nat) != 0)
		return -4;

	/* connect to the MSC */
	nat->msc_con = bsc_msc_create(nat->msc_ip, nat->msc_port, 0);
	if (!nat->msc_con) {
		fprintf(stderr, "Creating a bsc_msc_connection failed.\n");
		exit(1);
	}

	nat->msc_con->connection_loss = msc_connection_was_lost;
	nat->msc_con->connected = msc_connection_connected;
	nat->msc_con->write_queue.read_cb = ipaccess_msc_read_cb;
	nat->msc_con->write_queue.write_cb = ipaccess_msc_write_cb;;
	nat->msc_con->write_queue.bfd.data = nat->msc_con;
	bsc_msc_connect(nat->msc_con);

	/* wait for the BSC */
	if (listen_for_bsc(&bsc_listen, &local_addr, 5000) < 0) {
		fprintf(stderr, "Failed to listen for BSC.\n");
		exit(1);
	}

	signal(SIGABRT, &signal_handler);
	signal(SIGUSR1, &signal_handler);
	signal(SIGPIPE, SIG_IGN);

	if (daemonize) {
		rc = osmo_daemonize();
		if (rc < 0) {
			perror("Error during daemonize");
			exit(1);
		}
	}

	/* recycle timer */
	sccp_set_log_area(DSCCP);
	sccp_close.cb = sccp_close_unconfirmed;
	sccp_close.data = NULL;
	bsc_schedule_timer(&sccp_close, SCCP_CLOSE_TIME, 0);

	while (1) {
		bsc_select_main(0);
	}

	return 0;
}
