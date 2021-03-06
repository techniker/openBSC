/* A Media Gateway Control Protocol Media Gateway: RFC 3435 */
/* The protocol implementation */

/*
 * (C) 2009-2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2009-2010 by On-Waves
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

#include <openbsc/debug.h>
#include <osmocore/msgb.h>
#include <osmocore/talloc.h>
#include <openbsc/gsm_data.h>
#include <osmocore/select.h>
#include <openbsc/mgcp.h>
#include <openbsc/mgcp_internal.h>

/**
 * Macro for tokenizing MGCP messages and SDP in one go.
 *
 */
#define MSG_TOKENIZE_START \
	line_start = 0;						\
	for (i = 0; i < msgb_l3len(msg); ++i) {			\
		/* we have a line end */			\
		if (msg->l3h[i] == '\n') {			\
			/* skip the first line */		\
			if (line_start == 0) {			\
				line_start = i + 1;		\
				continue;			\
			}					\
								\
			/* check if we have a proper param */	\
			if (i - line_start == 1 && msg->l3h[line_start] == '\r') { \
			} else if (i - line_start > 2		\
			    && islower(msg->l3h[line_start])	\
			    && msg->l3h[line_start + 1] == '=') { \
			} else if (i - line_start < 3		\
			    || msg->l3h[line_start + 1] != ':'	\
			    || msg->l3h[line_start + 2] != ' ')	\
				goto error;			\
								\
			msg->l3h[i] = '\0';			\
			if (msg->l3h[i-1] == '\r')		\
				msg->l3h[i-1] = '\0';

#define MSG_TOKENIZE_END \
			line_start = i + 1; \
		}			    \
	}

static void mgcp_rtp_end_reset(struct mgcp_rtp_end *end);

struct mgcp_request {
	char *name;
	struct msgb *(*handle_request) (struct mgcp_config *cfg, struct msgb *msg);
	char *debug_name;
};

#define MGCP_REQUEST(NAME, REQ, DEBUG_NAME) \
	{ .name = NAME, .handle_request = REQ, .debug_name = DEBUG_NAME },

static struct msgb *handle_audit_endpoint(struct mgcp_config *cfg, struct msgb *msg);
static struct msgb *handle_create_con(struct mgcp_config *cfg, struct msgb *msg);
static struct msgb *handle_delete_con(struct mgcp_config *cfg, struct msgb *msg);
static struct msgb *handle_modify_con(struct mgcp_config *cfg, struct msgb *msg);
static struct msgb *handle_rsip(struct mgcp_config *cfg, struct msgb *msg);

static void create_transcoder(struct mgcp_endpoint *endp);
static void delete_transcoder(struct mgcp_endpoint *endp);

static uint32_t generate_call_id(struct mgcp_config *cfg)
{
	int i;

	/* use the call id */
	++cfg->last_call_id;

	/* handle wrap around */
	if (cfg->last_call_id == CI_UNUSED)
		++cfg->last_call_id;

	/* callstack can only be of size number_of_endpoints */
	/* verify that the call id is free, e.g. in case of overrun */
	for (i = 1; i < cfg->number_endpoints; ++i)
		if (cfg->endpoints[i].ci == cfg->last_call_id)
			return generate_call_id(cfg);

	return cfg->last_call_id;
}

/*
 * array of function pointers for handling various
 * messages. In the future this might be binary sorted
 * for performance reasons.
 */
static const struct mgcp_request mgcp_requests [] = {
	MGCP_REQUEST("AUEP", handle_audit_endpoint, "AuditEndpoint")
	MGCP_REQUEST("CRCX", handle_create_con, "CreateConnection")
	MGCP_REQUEST("DLCX", handle_delete_con, "DeleteConnection")
	MGCP_REQUEST("MDCX", handle_modify_con, "ModifiyConnection")

	/* SPEC extension */
	MGCP_REQUEST("RSIP", handle_rsip, "ReSetInProgress")
};

static struct msgb *mgcp_msgb_alloc(void)
{
	struct msgb *msg;
	msg = msgb_alloc_headroom(4096, 128, "MGCP msg");
	if (!msg)
	    LOGP(DMGCP, LOGL_ERROR, "Failed to msgb for MGCP data.\n");

	return msg;
}

struct msgb *mgcp_create_response_with_data(int code, const char *msg, const char *trans,
				    const char *data)
{
	int len;
	struct msgb *res;

	res = mgcp_msgb_alloc();
	if (!res)
		return NULL;

	if (data) {
		len = snprintf((char *) res->data, 2048, "%d %s\n%s", code, trans, data);
	} else {
		len = snprintf((char *) res->data, 2048, "%d %s\n", code, trans);
	}

	res->l2h = msgb_put(res, len);
	LOGP(DMGCP, LOGL_DEBUG, "Sending response: code: %d for '%s'\n", code, res->l2h);
	return res;
}

static struct msgb *create_response(int code, const char *msg, const char *trans)
{
	return mgcp_create_response_with_data(code, msg, trans, NULL);
}

static struct msgb *create_response_with_sdp(struct mgcp_endpoint *endp,
					     const char *msg, const char *trans_id)
{
	const char *addr = endp->cfg->local_ip;
	char sdp_record[4096];

	if (!addr)
		addr = endp->cfg->source_addr;

	snprintf(sdp_record, sizeof(sdp_record) - 1,
			"I: %u\n\n"
			"v=0\r\n"
			"c=IN IP4 %s\r\n"
			"m=audio %d RTP/AVP %d\r\n"
			"a=rtpmap:%d %s\r\n",
			endp->ci, addr, endp->net_end.local_port,
			endp->bts_end.payload_type, endp->bts_end.payload_type,
		        endp->cfg->audio_name);
	return mgcp_create_response_with_data(200, msg, trans_id, sdp_record);
}

/*
 * handle incoming messages:
 *   - this can be a command (four letters, space, transaction id)
 *   - or a response (three numbers, space, transaction id)
 */
struct msgb *mgcp_handle_message(struct mgcp_config *cfg, struct msgb *msg)
{
        int code;
	struct msgb *resp = NULL;

	if (msgb_l2len(msg) < 4) {
		LOGP(DMGCP, LOGL_ERROR, "mgs too short: %d\n", msg->len);
		return NULL;
	}

        /* attempt to treat it as a response */
        if (sscanf((const char *)&msg->l2h[0], "%3d %*s", &code) == 1) {
		LOGP(DMGCP, LOGL_DEBUG, "Response: Code: %d\n", code);
	} else {
		int i, handled = 0;
		msg->l3h = &msg->l2h[4];
		for (i = 0; i < ARRAY_SIZE(mgcp_requests); ++i)
			if (strncmp(mgcp_requests[i].name, (const char *) &msg->l2h[0], 4) == 0) {
				handled = 1;
				resp = mgcp_requests[i].handle_request(cfg, msg);
				break;
			}
		if (!handled) {
			LOGP(DMGCP, LOGL_NOTICE, "MSG with type: '%.4s' not handled\n", &msg->l2h[0]);
		}
	}

	return resp;
}

/* string tokenizer for the poor */
static int find_msg_pointers(struct msgb *msg, struct mgcp_msg_ptr *ptrs, int ptrs_length)
{
	int i, found = 0;

	int whitespace = 1;
	for (i = 0; i < msgb_l3len(msg) && ptrs_length > 0; ++i) {
		/* if we have a space we found an end */
		if (msg->l3h[i]	== ' ' || msg->l3h[i] == '\r' || msg->l3h[i] == '\n') {
			if (!whitespace) {
				++found;
				whitespace = 1;
				ptrs->length = i - ptrs->start - 1;
				++ptrs;
				--ptrs_length;
			} else {
			    /* skip any number of whitespace */
			}

			/* line end... stop */
			if (msg->l3h[i] == '\r' || msg->l3h[i] == '\n')
				break;
		} else if (msg->l3h[i] == '\r' || msg->l3h[i] == '\n') {
			/* line end, be done */
			break;
		} else if (whitespace) {
			whitespace = 0;
			ptrs->start = i;
		}
	}

	if (ptrs_length == 0)
		return -1;
	return found;
}

static struct mgcp_endpoint *find_endpoint(struct mgcp_config *cfg, const char *mgcp)
{
	char *endptr = NULL;
	unsigned int gw = INT_MAX;

	gw = strtoul(mgcp, &endptr, 16);
	if (gw == 0 || gw >= cfg->number_endpoints || strcmp(endptr, "@mgw") != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Not able to find endpoint: '%s'\n", mgcp);
		return NULL;
	}

	return &cfg->endpoints[gw];
}

int mgcp_analyze_header(struct mgcp_config *cfg, struct msgb *msg,
			struct mgcp_msg_ptr *ptr, int size,
			const char **transaction_id, struct mgcp_endpoint **endp)
{
	int found;

	*transaction_id = "000000";

	if (size < 3) {
		LOGP(DMGCP, LOGL_ERROR, "Not enough space in ptr\n");
		return -1;
	}

	found = find_msg_pointers(msg, ptr, size);

	if (found <= 3) {
		LOGP(DMGCP, LOGL_ERROR, "Gateway: Not enough params. Found: %d\n", found);
		return -1;
	}

	/*
	 * replace the space with \0. the main method gurantess that
	 * we still have + 1 for null termination
	 */
	msg->l3h[ptr[3].start + ptr[3].length + 1] = '\0';
	msg->l3h[ptr[2].start + ptr[2].length + 1] = '\0';
	msg->l3h[ptr[1].start + ptr[1].length + 1] = '\0';
	msg->l3h[ptr[0].start + ptr[0].length + 1] = '\0';

	if (strncmp("1.0", (const char *)&msg->l3h[ptr[3].start], 3) != 0
	    || strncmp("MGCP", (const char *)&msg->l3h[ptr[2].start], 4) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Wrong MGCP version. Not handling: '%s' '%s'\n",
			(const char *)&msg->l3h[ptr[3].start],
			(const char *)&msg->l3h[ptr[2].start]);
		return -1;
	}

	*transaction_id = (const char *)&msg->l3h[ptr[0].start];
	if (endp) {
		*endp = find_endpoint(cfg, (const char *)&msg->l3h[ptr[1].start]);
		return *endp == NULL;
	}
	return 0;
}

static int verify_call_id(const struct mgcp_endpoint *endp,
			  const char *callid)
{
	if (strcmp(endp->callid, callid) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "CallIDs does not match on 0x%x. '%s' != '%s'\n",
			ENDPOINT_NUMBER(endp), endp->callid, callid);
		return -1;
	}

	return 0;
}

static int verify_ci(const struct mgcp_endpoint *endp,
		     const char *_ci)
{
	uint32_t ci = strtoul(_ci, NULL, 10);

	if (ci != endp->ci) {
		LOGP(DMGCP, LOGL_ERROR, "ConnectionIdentifiers do not match on 0x%x. %u != %s\n",
			ENDPOINT_NUMBER(endp), endp->ci, _ci);
		return -1;
	}

	return 0;
}

static struct msgb *handle_audit_endpoint(struct mgcp_config *cfg, struct msgb *msg)
{
	struct mgcp_msg_ptr data_ptrs[6];
	int found, response;
	const char *trans_id;
	struct mgcp_endpoint *endp;

	found = mgcp_analyze_header(cfg, msg, data_ptrs, ARRAY_SIZE(data_ptrs), &trans_id, &endp);
	if (found != 0)
	    response = 500;
	else
	    response = 200;

	return create_response(response, "AUEP", trans_id);
}

static int parse_conn_mode(const char *msg, int *conn_mode)
{
	int ret = 0;
	if (strcmp(msg, "recvonly") == 0)
		*conn_mode = MGCP_CONN_RECV_ONLY;
	else if (strcmp(msg, "sendrecv") == 0)
		*conn_mode = MGCP_CONN_RECV_SEND;
	else if (strcmp(msg, "sendonly") == 0)
		*conn_mode = MGCP_CONN_SEND_ONLY;
	else if (strcmp(msg, "loopback") == 0)
		*conn_mode = MGCP_CONN_LOOPBACK;
	else {
		LOGP(DMGCP, LOGL_ERROR, "Unknown connection mode: '%s'\n", msg);
		ret = -1;
	}

	return ret;
}

static int allocate_port(struct mgcp_endpoint *endp, struct mgcp_rtp_end *end,
			 struct mgcp_port_range *range,
			 int (*alloc)(struct mgcp_endpoint *endp, int port))
{
	int i;

	if (range->mode == PORT_ALLOC_STATIC) {
		end->local_port = rtp_calculate_port(ENDPOINT_NUMBER(endp), range->base_port);
		end->local_alloc = PORT_ALLOC_STATIC;
		return 0;
	}

	/* attempt to find a port */
	for (i = 0; i < 200; ++i) {
		int rc;

		if (range->last_port >= range->range_end)
			range->last_port = range->range_start;

		rc = alloc(endp, range->last_port);

		range->last_port += 2;
		if (rc == 0) {
			end->local_alloc = PORT_ALLOC_DYNAMIC;
			return 0;
		}

	}

	LOGP(DMGCP, LOGL_ERROR, "Allocating a RTP/RTCP port failed 200 times 0x%x.\n",
	     ENDPOINT_NUMBER(endp));
	return -1;
}

static int allocate_ports(struct mgcp_endpoint *endp)
{
	if (allocate_port(endp, &endp->net_end, &endp->cfg->net_ports,
			  mgcp_bind_net_rtp_port) != 0)
		return -1;

	if (allocate_port(endp, &endp->bts_end, &endp->cfg->bts_ports,
			  mgcp_bind_bts_rtp_port) != 0) {
		mgcp_rtp_end_reset(&endp->net_end);
		return -1;
	}

	if (endp->cfg->transcoder_ip &&
	    allocate_port(endp, &endp->transcoder_end, &endp->cfg->transcoder_ports,
			  mgcp_bind_transcoder_rtp_port) != 0) {
		mgcp_rtp_end_reset(&endp->net_end);
		mgcp_rtp_end_reset(&endp->bts_end);
		return -1;
	}

	return 0;
}

static struct msgb *handle_create_con(struct mgcp_config *cfg, struct msgb *msg)
{
	struct mgcp_msg_ptr data_ptrs[6];
	int found, i, line_start;
	const char *trans_id;
	struct mgcp_endpoint *endp;
	int error_code = 400;

	found = mgcp_analyze_header(cfg, msg, data_ptrs, ARRAY_SIZE(data_ptrs), &trans_id, &endp);
	if (found != 0)
		return create_response(510, "CRCX", trans_id);

	if (endp->allocated) {
		if (cfg->force_realloc) {
			LOGP(DMGCP, LOGL_NOTICE, "Endpoint 0x%x already allocated. Forcing realloc.\n",
			    ENDPOINT_NUMBER(endp));
			mgcp_free_endp(endp);
			if (cfg->realloc_cb)
				cfg->realloc_cb(cfg, ENDPOINT_NUMBER(endp));
		} else {
			LOGP(DMGCP, LOGL_ERROR, "Endpoint is already used. 0x%x\n",
			     ENDPOINT_NUMBER(endp));
			return create_response(400, "CRCX", trans_id);
		}
	}

	/* parse CallID C: and LocalParameters L: */
	MSG_TOKENIZE_START
	switch (msg->l3h[line_start]) {
	case 'L':
		endp->local_options = talloc_strdup(cfg->endpoints,
			(const char *)&msg->l3h[line_start + 3]);
		break;
	case 'C':
		endp->callid = talloc_strdup(cfg->endpoints,
			(const char *)&msg->l3h[line_start + 3]);
		break;
	case 'M':
		if (parse_conn_mode((const char *)&msg->l3h[line_start + 3],
			    &endp->conn_mode) != 0) {
		    error_code = 517;
		    goto error2;
		}

		endp->orig_mode = endp->conn_mode;
		break;
	default:
		LOGP(DMGCP, LOGL_NOTICE, "Unhandled option: '%c'/%d on 0x%x\n",
			msg->l3h[line_start], msg->l3h[line_start],
			ENDPOINT_NUMBER(endp));
		break;
	}
	MSG_TOKENIZE_END

	/* initialize */
	endp->net_end.rtp_port = endp->net_end.rtcp_port = endp->bts_end.rtp_port = endp->bts_end.rtcp_port = 0;

	/* set to zero until we get the info */
	memset(&endp->net_end.addr, 0, sizeof(endp->net_end.addr));

	/* bind to the port now */
	if (allocate_ports(endp) != 0)
		goto error2;

	/* assign a local call identifier or fail */
	endp->ci = generate_call_id(cfg);
	if (endp->ci == CI_UNUSED)
		goto error2;

	endp->allocated = 1;
	endp->bts_end.payload_type = cfg->audio_payload;

	/* policy CB */
	if (cfg->policy_cb) {
		switch (cfg->policy_cb(cfg, ENDPOINT_NUMBER(endp), MGCP_ENDP_CRCX, trans_id)) {
		case MGCP_POLICY_REJECT:
			LOGP(DMGCP, LOGL_NOTICE, "CRCX rejected by policy on 0x%x\n",
			     ENDPOINT_NUMBER(endp));
			mgcp_free_endp(endp);
			return create_response(400, "CRCX", trans_id);
			break;
		case MGCP_POLICY_DEFER:
			/* stop processing */
			create_transcoder(endp);
			return NULL;
			break;
		case MGCP_POLICY_CONT:
			/* just continue */
			break;
		}
	}

	LOGP(DMGCP, LOGL_DEBUG, "Creating endpoint on: 0x%x CI: %u port: %u/%u\n",
		ENDPOINT_NUMBER(endp), endp->ci,
		endp->net_end.local_port, endp->bts_end.local_port);
	if (cfg->change_cb)
		cfg->change_cb(cfg, ENDPOINT_NUMBER(endp), MGCP_ENDP_CRCX);

	create_transcoder(endp);
	return create_response_with_sdp(endp, "CRCX", trans_id);
error:
	LOGP(DMGCP, LOGL_ERROR, "Malformed line: %s on 0x%x with: line_start: %d %d\n",
		    hexdump(msg->l3h, msgb_l3len(msg)),
		    ENDPOINT_NUMBER(endp), line_start, i);
	return create_response(error_code, "CRCX", trans_id);

error2:
	mgcp_free_endp(endp);
	LOGP(DMGCP, LOGL_NOTICE, "Resource error on 0x%x\n", ENDPOINT_NUMBER(endp));
	return create_response(error_code, "CRCX", trans_id);
}

static struct msgb *handle_modify_con(struct mgcp_config *cfg, struct msgb *msg)
{
	struct mgcp_msg_ptr data_ptrs[6];
	int found, i, line_start;
	const char *trans_id;
	struct mgcp_endpoint *endp;
	int error_code = 500;
	int silent = 0;

	found = mgcp_analyze_header(cfg, msg, data_ptrs, ARRAY_SIZE(data_ptrs), &trans_id, &endp);
	if (found != 0)
		return create_response(510, "MDCX", trans_id);

	if (endp->ci == CI_UNUSED) {
		LOGP(DMGCP, LOGL_ERROR, "Endpoint is not holding a connection. 0x%x\n", ENDPOINT_NUMBER(endp));
		return create_response(400, "MDCX", trans_id);
	}

	MSG_TOKENIZE_START
	switch (msg->l3h[line_start]) {
	case 'C': {
		if (verify_call_id(endp, (const char *)&msg->l3h[line_start + 3]) != 0)
			goto error3;
		break;
	}
	case 'I': {
		if (verify_ci(endp, (const char *)&msg->l3h[line_start + 3]) != 0)
			goto error3;
		break;
	}
	case 'L':
		/* skip */
		break;
	case 'M':
		if (parse_conn_mode((const char *)&msg->l3h[line_start + 3],
			    &endp->conn_mode) != 0) {
		    error_code = 517;
		    goto error3;
		}
		endp->orig_mode = endp->conn_mode;
		break;
	case 'Z':
		silent = strcmp("noanswer", (const char *)&msg->l3h[line_start + 3]) == 0;
		break;
	case '\0':
		/* SDP file begins */
		break;
	case 'a':
	case 'o':
	case 's':
	case 't':
	case 'v':
		/* skip these SDP attributes */
		break;
	case 'm': {
		int port;
		int payload;
		const char *param = (const char *)&msg->l3h[line_start];

		if (sscanf(param, "m=audio %d RTP/AVP %d", &port, &payload) == 2) {
			endp->net_end.rtp_port = htons(port);
			endp->net_end.rtcp_port = htons(port + 1);
			endp->net_end.payload_type = payload;
		}
		break;
	}
	case 'c': {
		char ipv4[16];
		const char *param = (const char *)&msg->l3h[line_start];

		if (sscanf(param, "c=IN IP4 %15s", ipv4) == 1) {
			inet_aton(ipv4, &endp->net_end.addr);
		}
		break;
	}
	default:
		LOGP(DMGCP, LOGL_NOTICE, "Unhandled option: '%c'/%d on 0x%x\n",
			msg->l3h[line_start], msg->l3h[line_start],
			ENDPOINT_NUMBER(endp));
		break;
	}
	MSG_TOKENIZE_END

	/* policy CB */
	if (cfg->policy_cb) {
		switch (cfg->policy_cb(cfg, ENDPOINT_NUMBER(endp), MGCP_ENDP_MDCX, trans_id)) {
		case MGCP_POLICY_REJECT:
			LOGP(DMGCP, LOGL_NOTICE, "MDCX rejected by policy on 0x%x\n",
			     ENDPOINT_NUMBER(endp));
			if (silent)
				goto out_silent;
			return create_response(400, "MDCX", trans_id);
			break;
		case MGCP_POLICY_DEFER:
			/* stop processing */
			return NULL;
			break;
		case MGCP_POLICY_CONT:
			/* just continue */
			break;
		}
	}

	/* modify */
	LOGP(DMGCP, LOGL_DEBUG, "Modified endpoint on: 0x%x Server: %s:%u\n",
		ENDPOINT_NUMBER(endp), inet_ntoa(endp->net_end.addr), ntohs(endp->net_end.rtp_port));
	if (cfg->change_cb)
		cfg->change_cb(cfg, ENDPOINT_NUMBER(endp), MGCP_ENDP_MDCX);
	if (silent)
		goto out_silent;

	return create_response_with_sdp(endp, "MDCX", trans_id);

error:
	LOGP(DMGCP, LOGL_ERROR, "Malformed line: %s on 0x%x with: line_start: %d %d %d\n",
		    hexdump(msg->l3h, msgb_l3len(msg)),
		    ENDPOINT_NUMBER(endp), line_start, i, msg->l3h[line_start]);
	return create_response(error_code, "MDCX", trans_id);

error3:
	return create_response(error_code, "MDCX", trans_id);


out_silent:
	return NULL;
}

static struct msgb *handle_delete_con(struct mgcp_config *cfg, struct msgb *msg)
{
	struct mgcp_msg_ptr data_ptrs[6];
	int found, i, line_start;
	const char *trans_id;
	struct mgcp_endpoint *endp;
	int error_code = 400;
	int silent = 0;

	found = mgcp_analyze_header(cfg, msg, data_ptrs, ARRAY_SIZE(data_ptrs), &trans_id, &endp);
	if (found != 0)
		return create_response(error_code, "DLCX", trans_id);

	if (!endp->allocated) {
		LOGP(DMGCP, LOGL_ERROR, "Endpoint is not used. 0x%x\n", ENDPOINT_NUMBER(endp));
		return create_response(400, "DLCX", trans_id);
	}

	MSG_TOKENIZE_START
	switch (msg->l3h[line_start]) {
	case 'C': {
		if (verify_call_id(endp, (const char *)&msg->l3h[line_start + 3]) != 0)
			goto error3;
		break;
	}
	case 'I': {
		if (verify_ci(endp, (const char *)&msg->l3h[line_start + 3]) != 0)
			goto error3;
		break;
	case 'Z':
		silent = strcmp("noanswer", (const char *)&msg->l3h[line_start + 3]) == 0;
		break;
	}
	default:
		LOGP(DMGCP, LOGL_NOTICE, "Unhandled option: '%c'/%d on 0x%x\n",
			msg->l3h[line_start], msg->l3h[line_start],
			ENDPOINT_NUMBER(endp));
		break;
	}
	MSG_TOKENIZE_END

	/* policy CB */
	if (cfg->policy_cb) {
		switch (cfg->policy_cb(cfg, ENDPOINT_NUMBER(endp), MGCP_ENDP_DLCX, trans_id)) {
		case MGCP_POLICY_REJECT:
			LOGP(DMGCP, LOGL_NOTICE, "DLCX rejected by policy on 0x%x\n",
			     ENDPOINT_NUMBER(endp));
			if (silent)
				goto out_silent;
			return create_response(400, "DLCX", trans_id);
			break;
		case MGCP_POLICY_DEFER:
			/* stop processing */
			delete_transcoder(endp);
			return NULL;
			break;
		case MGCP_POLICY_CONT:
			/* just continue */
			break;
		}
	}

	/* free the connection */
	LOGP(DMGCP, LOGL_DEBUG, "Deleted endpoint on: 0x%x Server: %s:%u\n",
		ENDPOINT_NUMBER(endp), inet_ntoa(endp->net_end.addr), ntohs(endp->net_end.rtp_port));

	delete_transcoder(endp);
	mgcp_free_endp(endp);
	if (cfg->change_cb)
		cfg->change_cb(cfg, ENDPOINT_NUMBER(endp), MGCP_ENDP_DLCX);

	if (silent)
		goto out_silent;
	return create_response(250, "DLCX", trans_id);

error:
	LOGP(DMGCP, LOGL_ERROR, "Malformed line: %s on 0x%x with: line_start: %d %d\n",
		    hexdump(msg->l3h, msgb_l3len(msg)),
		    ENDPOINT_NUMBER(endp), line_start, i);
	return create_response(error_code, "DLCX", trans_id);

error3:
	return create_response(error_code, "DLCX", trans_id);

out_silent:
	return NULL;
}

static struct msgb *handle_rsip(struct mgcp_config *cfg, struct msgb *msg)
{
	if (cfg->reset_cb)
		cfg->reset_cb(cfg);
	return NULL;
}

struct mgcp_config *mgcp_config_alloc(void)
{
	struct mgcp_config *cfg;

	cfg = talloc_zero(NULL, struct mgcp_config);
	if (!cfg) {
		LOGP(DMGCP, LOGL_FATAL, "Failed to allocate config.\n");
		return NULL;
	}

	cfg->source_port = 2427;
	cfg->source_addr = talloc_strdup(cfg, "0.0.0.0");
	cfg->audio_name = talloc_strdup(cfg, "AMR/8000");
	cfg->audio_payload = 126;
	cfg->transcoder_remote_base = 4000;

	cfg->bts_ports.base_port = RTP_PORT_DEFAULT;
	cfg->net_ports.base_port = RTP_PORT_NET_DEFAULT;

	return cfg;
}

static void mgcp_rtp_end_reset(struct mgcp_rtp_end *end)
{
	if (end->local_alloc == PORT_ALLOC_DYNAMIC)
		mgcp_free_rtp_port(end);

	end->packets = 0;
	memset(&end->addr, 0, sizeof(end->addr));
	end->rtp_port = end->rtcp_port = end->local_port = 0;
	end->payload_type = -1;
	end->local_alloc = -1;
}

static void mgcp_rtp_end_init(struct mgcp_rtp_end *end)
{
	mgcp_rtp_end_reset(end);
	end->rtp.fd = -1;
	end->rtcp.fd = -1;
}

int mgcp_endpoints_allocate(struct mgcp_config *cfg)
{
	int i;

	/* Initialize all endpoints */
	cfg->endpoints = _talloc_zero_array(cfg,
				       sizeof(struct mgcp_endpoint),
				       cfg->number_endpoints, "endpoints");
	if (!cfg->endpoints)
		return -1;

	for (i = 0; i < cfg->number_endpoints; ++i) {
		cfg->endpoints[i].ci = CI_UNUSED;
		cfg->endpoints[i].cfg = cfg;
		mgcp_rtp_end_init(&cfg->endpoints[i].net_end);
		mgcp_rtp_end_init(&cfg->endpoints[i].bts_end);
		mgcp_rtp_end_init(&cfg->endpoints[i].transcoder_end);
	}

	return 0;
}

void mgcp_free_endp(struct mgcp_endpoint *endp)
{
	LOGP(DMGCP, LOGL_DEBUG, "Deleting endpoint on: 0x%x\n", ENDPOINT_NUMBER(endp));
	endp->ci = CI_UNUSED;
	endp->allocated = 0;

	if (endp->callid) {
		talloc_free(endp->callid);
		endp->callid = NULL;
	}

	if (endp->local_options) {
		talloc_free(endp->local_options);
		endp->local_options = NULL;
	}

	mgcp_rtp_end_reset(&endp->bts_end);
	mgcp_rtp_end_reset(&endp->net_end);
	mgcp_rtp_end_reset(&endp->transcoder_end);

	memset(&endp->net_state, 0, sizeof(endp->net_state));
	memset(&endp->bts_state, 0, sizeof(endp->bts_state));

	endp->conn_mode = endp->orig_mode = MGCP_CONN_NONE;
	endp->allow_patch = 0;

	memset(&endp->taps, 0, sizeof(endp->taps));
}

/* For transcoding we need to manage an in and an output that are connected */
static int back_channel(int endpoint)
{
	return endpoint + 60;
}

static int send_trans(struct mgcp_config *cfg, const char *buf, int len)
{
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr = cfg->transcoder_in;
	addr.sin_port = htons(2427);
	return sendto(cfg->gw_fd.bfd.fd, buf, len, 0,
		      (struct sockaddr *) &addr, sizeof(addr));
}

static void send_msg(struct mgcp_endpoint *endp, int endpoint, int port,
		     const char *msg, const char *mode)
{
	char buf[2096];
	int len;

	/* hardcoded to AMR right now, we do not know the real type at this point */
	len = snprintf(buf, sizeof(buf),
			"%s 42 %x@mgw MGCP 1.0\r\n"
			"C: 4256\r\n"
			"M: %s\r\n"
			"\r\n"
			"c=IN IP4 %s\r\n"
			"m=audio %d RTP/AVP %d\r\n"
			"a=rtpmap:%d %s\r\n",
			msg, endpoint, mode, endp->cfg->source_addr,
			port, endp->cfg->audio_payload,
			endp->cfg->audio_payload, endp->cfg->audio_name);

	if (len < 0)
		return;

	buf[sizeof(buf) - 1] = '\0';

	send_trans(endp->cfg, buf, len);
}

static void send_dlcx(struct mgcp_endpoint *endp, int endpoint)
{
	char buf[2096];
	int len;

	len = snprintf(buf, sizeof(buf),
			"DLCX 43 %x@mgw MGCP 1.0\r\n"
			"C: 4256\r\n"
			, endpoint);

	if (len < 0)
		return;

	buf[sizeof(buf) - 1] = '\0';

	send_trans(endp->cfg, buf, len);
}

static void create_transcoder(struct mgcp_endpoint *endp)
{
	int port;
	int in_endp = ENDPOINT_NUMBER(endp);
	int out_endp = back_channel(in_endp);

	if (!endp->cfg->transcoder_ip)
		return;

	send_msg(endp, in_endp, endp->bts_end.local_port, "CRCX", "recvonly");
	send_msg(endp, in_endp, endp->bts_end.local_port, "MDCX", "recvonly");
	send_msg(endp, out_endp, endp->transcoder_end.local_port, "CRCX", "sendrecv");
	send_msg(endp, out_endp, endp->transcoder_end.local_port, "MDCX", "sendrecv");

	port = rtp_calculate_port(out_endp, endp->cfg->transcoder_remote_base);
	endp->transcoder_end.rtp_port = htons(port);
	endp->transcoder_end.rtcp_port = htons(port + 1);
}

static void delete_transcoder(struct mgcp_endpoint *endp)
{
	int in_endp = ENDPOINT_NUMBER(endp);
	int out_endp = back_channel(in_endp);

	if (!endp->cfg->transcoder_ip)
		return;

	send_dlcx(endp, in_endp);
	send_dlcx(endp, out_endp);
}

int mgcp_reset_transcoder(struct mgcp_config *cfg)
{
	if (!cfg->transcoder_ip)
		return 0;

	static const char mgcp_reset[] = {
	    "RSIP 1 13@mgw MGCP 1.0\r\n"
	};

	return send_trans(cfg, mgcp_reset, sizeof mgcp_reset -1);
}
