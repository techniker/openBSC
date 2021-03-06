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

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <endian.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include <osmocore/msgb.h>
#include <osmocore/select.h>

#include <openbsc/debug.h>
#include <openbsc/mgcp.h>
#include <openbsc/mgcp_internal.h>

#warning "Make use of the rtp proxy code"

/* according to rtp_proxy.c RFC 3550 */
struct rtp_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t  csrc_count:4,
		  extension:1,
		  padding:1,
		  version:2;
	uint8_t  payload_type:7,
		  marker:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
	uint8_t  version:2,
		  padding:1,
		  extension:1,
		  csrc_count:4;
	uint8_t  marker:1,
		  payload_type:7;
#endif
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;
} __attribute__((packed));


enum {
	DEST_NETWORK = 0,
	DEST_BTS = 1,
};

enum {
	PROTO_RTP,
	PROTO_RTCP,
};

#define DUMMY_LOAD 0x23


static int udp_send(int fd, struct in_addr *addr, int port, char *buf, int len)
{
	struct sockaddr_in out;
	out.sin_family = AF_INET;
	out.sin_port = port;
	memcpy(&out.sin_addr, addr, sizeof(*addr));

	return sendto(fd, buf, len, 0, (struct sockaddr *)&out, sizeof(out));
}

int mgcp_send_dummy(struct mgcp_endpoint *endp)
{
	static char buf[] = { DUMMY_LOAD };

	return udp_send(endp->net_end.rtp.fd, &endp->net_end.addr,
			endp->net_end.rtp_port, buf, 1);
}

static void patch_and_count(struct mgcp_endpoint *endp, struct mgcp_rtp_state *state,
			    int payload, struct sockaddr_in *addr, char *data, int len)
{
	uint16_t seq;
	uint32_t timestamp;
	struct rtp_hdr *rtp_hdr;

	if (len < sizeof(*rtp_hdr))
		return;

	rtp_hdr = (struct rtp_hdr *) data;
	seq = ntohs(rtp_hdr->sequence);
	timestamp = ntohl(rtp_hdr->timestamp);

	if (!state->initialized) {
		state->seq_no = seq - 1;
		state->ssrc = state->orig_ssrc = rtp_hdr->ssrc;
		state->initialized = 1;
		state->last_timestamp = timestamp;
	} else if (state->ssrc != rtp_hdr->ssrc) {
		state->ssrc = rtp_hdr->ssrc;
		state->seq_offset = (state->seq_no + 1) - seq;
		state->timestamp_offset = state->last_timestamp - timestamp;
		state->patch = endp->allow_patch;
		LOGP(DMGCP, LOGL_NOTICE,
			"The SSRC changed on 0x%x SSRC: %u offset: %d from %s:%d in %d\n",
			ENDPOINT_NUMBER(endp), state->ssrc, state->seq_offset,
			inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), endp->conn_mode);
	}

	/* apply the offset and store it back to the packet */
	if (state->patch) {
		seq += state->seq_offset;
		rtp_hdr->sequence = htons(seq);
		rtp_hdr->ssrc = state->orig_ssrc;

		timestamp += state->timestamp_offset;
		rtp_hdr->timestamp = htonl(timestamp);
	}

	/* seq changed, now compare if we have lost something */
	if (state->seq_no + 1u != seq)
		state->lost_no = abs(seq - (state->seq_no + 1));
	state->seq_no = seq;

	state->last_timestamp = timestamp;

	if (payload < 0)
		return;

	rtp_hdr->payload_type = payload;
}

/*
 * The below code is for dispatching. We have a dedicated port for
 * the data coming from the net and one to discover the BTS.
 */
static int forward_data(int fd, struct mgcp_rtp_tap *tap, const char *buf, int len)
{
	if (!tap->enabled)
		return 0;

	return sendto(fd, buf, len, 0,
		      (struct sockaddr *)&tap->forward, sizeof(tap->forward));
}

static int send_transcoder(struct mgcp_endpoint *endp, int is_rtp,
		       const char *buf, int len)
{
	int rc;
	int port;
	struct mgcp_config *cfg = endp->cfg;
	struct sockaddr_in addr;

	if (endp->transcoder_end.rtp_port == 0) {
		LOGP(DMGCP, LOGL_ERROR, "Transcoder port not known on 0x%x\n",
			ENDPOINT_NUMBER(endp));
		return -1;
	}

	port = rtp_calculate_port(ENDPOINT_NUMBER(endp), cfg->transcoder_remote_base);
	if (!is_rtp)
		port += 1;

	addr.sin_family = AF_INET;
	addr.sin_addr = cfg->transcoder_in;
	addr.sin_port = htons(port);

	rc = sendto(is_rtp ?
		endp->bts_end.rtp.fd :
		endp->bts_end.rtcp.fd, buf, len, 0,
		(struct sockaddr *) &addr, sizeof(addr));

	if (rc != len)
		LOGP(DMGCP, LOGL_ERROR,
			"Failed to send data to the transcoder: %s\n",
			strerror(errno));

	return rc;
}

static int send_to(struct mgcp_endpoint *endp, int dest, int is_rtp,
		   struct sockaddr_in *addr, char *buf, int rc)
{
	struct mgcp_config *cfg = endp->cfg;
	/* For loop toggle the destination and then dispatch. */
	if (cfg->audio_loop)
		dest = !dest;

	/* Loop based on the conn_mode, maybe undoing the above */
	if (endp->conn_mode == MGCP_CONN_LOOPBACK)
		dest = !dest;

	if (dest == DEST_NETWORK) {
		if (is_rtp) {
			patch_and_count(endp, &endp->bts_state,
					endp->net_end.payload_type,
					addr, buf, rc);
			forward_data(endp->net_end.rtp.fd,
				     &endp->taps[MGCP_TAP_NET_OUT], buf, rc);
			return udp_send(endp->net_end.rtp.fd, &endp->net_end.addr,
					endp->net_end.rtp_port, buf, rc);
		} else {
			return udp_send(endp->net_end.rtcp.fd, &endp->net_end.addr,
					endp->net_end.rtcp_port, buf, rc);
		}
	} else {
		if (is_rtp) {
			patch_and_count(endp, &endp->net_state,
					endp->bts_end.payload_type,
					addr, buf, rc);
			forward_data(endp->bts_end.rtp.fd,
				     &endp->taps[MGCP_TAP_BTS_OUT], buf, rc);
			return udp_send(endp->bts_end.rtp.fd, &endp->bts_end.addr,
					endp->bts_end.rtp_port, buf, rc);
		} else {
			return udp_send(endp->bts_end.rtcp.fd, &endp->bts_end.addr,
					endp->bts_end.rtcp_port, buf, rc);
		}
	}
}

static int recevice_from(struct mgcp_endpoint *endp, int fd, struct sockaddr_in *addr,
			 char *buf, int bufsize)
{
	int rc;
	socklen_t slen = sizeof(*addr);

	rc = recvfrom(fd, buf, bufsize, 0,
			    (struct sockaddr *) addr, &slen);
	if (rc < 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to receive message on: 0x%x errno: %d/%s\n",
			ENDPOINT_NUMBER(endp), errno, strerror(errno));
		return -1;
	}

	/* do not forward aynthing... maybe there is a packet from the bts */
	if (!endp->allocated)
		return -1;

	#warning "Slight spec violation. With connection mode recvonly we should attempt to forward."

	return rc;
}

static int rtp_data_net(struct bsc_fd *fd, unsigned int what)
{
	char buf[4096];
	struct sockaddr_in addr;
	struct mgcp_endpoint *endp;
	int rc, proto;

	endp = (struct mgcp_endpoint *) fd->data;

	rc = recevice_from(endp, fd->fd, &addr, buf, sizeof(buf));
	if (rc <= 0)
		return -1;

	if (memcmp(&addr.sin_addr, &endp->net_end.addr, sizeof(addr.sin_addr)) != 0) {
		LOGP(DMGCP, LOGL_ERROR,
			"Data from wrong address %s on 0x%x\n",
			inet_ntoa(addr.sin_addr), ENDPOINT_NUMBER(endp));
		return -1;
	}

	if (endp->net_end.rtp_port != addr.sin_port &&
	    endp->net_end.rtcp_port != addr.sin_port) {
		LOGP(DMGCP, LOGL_ERROR,
			"Data from wrong source port %d on 0x%x\n",
			ntohs(addr.sin_port), ENDPOINT_NUMBER(endp));
		return -1;
	}

	/* throw away the dummy message */
	if (rc == 1 && buf[0] == DUMMY_LOAD) {
		LOGP(DMGCP, LOGL_NOTICE, "Filtered dummy from network on 0x%x\n",
			ENDPOINT_NUMBER(endp));
		return 0;
	}

	proto = fd == &endp->net_end.rtp ? PROTO_RTP : PROTO_RTCP;
	endp->net_end.packets += 1;

	forward_data(fd->fd, &endp->taps[MGCP_TAP_NET_IN], buf, rc);
	return send_to(endp, DEST_BTS, proto == PROTO_RTP, &addr, &buf[0], rc);
}

static void discover_bts(struct mgcp_endpoint *endp, int proto, struct sockaddr_in *addr)
{
	struct mgcp_config *cfg = endp->cfg;

	if (proto == PROTO_RTP && endp->bts_end.rtp_port == 0) {
		if (!cfg->bts_ip ||
		    memcmp(&addr->sin_addr,
			   &cfg->bts_in, sizeof(cfg->bts_in)) == 0 ||
		    memcmp(&addr->sin_addr,
			   &endp->bts_end.addr, sizeof(endp->bts_end.addr)) == 0) {

			endp->bts_end.rtp_port = addr->sin_port;
			endp->bts_end.addr = addr->sin_addr;

			LOGP(DMGCP, LOGL_NOTICE,
				"Found BTS for endpoint: 0x%x on port: %d/%d of %s\n",
				ENDPOINT_NUMBER(endp), ntohs(endp->bts_end.rtp_port),
				ntohs(endp->bts_end.rtcp_port), inet_ntoa(addr->sin_addr));
		}
	} else if (proto == PROTO_RTCP && endp->bts_end.rtcp_port == 0) {
		if (memcmp(&endp->bts_end.addr, &addr->sin_addr,
				sizeof(endp->bts_end.addr)) == 0) {
			endp->bts_end.rtcp_port = addr->sin_port;
		}
	}
}

static int rtp_data_bts(struct bsc_fd *fd, unsigned int what)
{
	char buf[4096];
	struct sockaddr_in addr;
	struct mgcp_endpoint *endp;
	struct mgcp_config *cfg;
	int rc, proto;

	endp = (struct mgcp_endpoint *) fd->data;
	cfg = endp->cfg;

	rc = recevice_from(endp, fd->fd, &addr, buf, sizeof(buf));
	if (rc <= 0)
		return -1;

	proto = fd == &endp->bts_end.rtp ? PROTO_RTP : PROTO_RTCP;

	/* We have no idea who called us, maybe it is the BTS. */
	/* it was the BTS... */
	discover_bts(endp, proto, &addr);

	if (memcmp(&endp->bts_end.addr, &addr.sin_addr, sizeof(addr.sin_addr)) != 0) {
		LOGP(DMGCP, LOGL_ERROR,
			"Data from wrong bts %s on 0x%x\n",
			inet_ntoa(addr.sin_addr), ENDPOINT_NUMBER(endp));
		return -1;
	}

	if (endp->bts_end.rtp_port != addr.sin_port &&
	    endp->bts_end.rtcp_port != addr.sin_port) {
		LOGP(DMGCP, LOGL_ERROR,
			"Data from wrong bts source port %d on 0x%x\n",
			ntohs(addr.sin_port), ENDPOINT_NUMBER(endp));
		return -1;
	}

	/* throw away the dummy message */
	if (rc == 1 && buf[0] == DUMMY_LOAD) {
		LOGP(DMGCP, LOGL_NOTICE, "Filtered dummy from bts on 0x%x\n",
			ENDPOINT_NUMBER(endp));
		return 0;
	}

	/* do this before the loop handling */
	endp->bts_end.packets += 1;

	forward_data(fd->fd, &endp->taps[MGCP_TAP_BTS_IN], buf, rc);
	if (cfg->transcoder_ip)
		return send_transcoder(endp, proto == PROTO_RTP, &buf[0], rc);
	else
		return send_to(endp, DEST_NETWORK, proto == PROTO_RTP, &addr, &buf[0], rc);
}

static int rtp_data_transcoder(struct bsc_fd *fd, unsigned int what)
{
	char buf[4096];
	struct sockaddr_in addr;
	struct mgcp_endpoint *endp;
	struct mgcp_config *cfg;
	int rc, proto;

	endp = (struct mgcp_endpoint *) fd->data;
	cfg = endp->cfg;

	rc = recevice_from(endp, fd->fd, &addr, buf, sizeof(buf));
	if (rc <= 0)
		return -1;

	proto = fd == &endp->transcoder_end.rtp ? PROTO_RTP : PROTO_RTCP;

	if (memcmp(&addr.sin_addr, &cfg->transcoder_in, sizeof(addr.sin_addr)) != 0) {
		LOGP(DMGCP, LOGL_ERROR,
			"Data not coming from transcoder: %s on 0x%x\n",
			inet_ntoa(addr.sin_addr), ENDPOINT_NUMBER(endp));
		return -1;
	}

	if (endp->transcoder_end.rtp_port != addr.sin_port &&
	    endp->transcoder_end.rtcp_port != addr.sin_port) {
		LOGP(DMGCP, LOGL_ERROR,
			"Data from wrong transcoder source port %d on 0x%x\n",
			ntohs(addr.sin_port), ENDPOINT_NUMBER(endp));
		return -1;
	}

	/* throw away the dummy message */
	if (rc == 1 && buf[0] == DUMMY_LOAD) {
		LOGP(DMGCP, LOGL_NOTICE, "Filtered dummy from transcoder on 0x%x\n",
			ENDPOINT_NUMBER(endp));
		return 0;
	}

	endp->transcoder_end.packets += 1;
	return send_to(endp, DEST_NETWORK, proto == PROTO_RTP, &addr, &buf[0], rc);
}

static int create_bind(const char *source_addr, struct bsc_fd *fd, int port)
{
	struct sockaddr_in addr;
	int on = 1;

	fd->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd->fd < 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to create UDP port.\n");
		return -1;
	}

	setsockopt(fd->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_aton(source_addr, &addr.sin_addr);

	if (bind(fd->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(fd->fd);
		fd->fd = -1;
		return -1;
	}

	return 0;
}

static int set_ip_tos(int fd, int tos)
{
	int ret;
	ret = setsockopt(fd, IPPROTO_IP, IP_TOS,
			 &tos, sizeof(tos));
	return ret != 0;
}

static int bind_rtp(struct mgcp_config *cfg, struct mgcp_rtp_end *rtp_end, int endpno)
{
	if (create_bind(cfg->source_addr, &rtp_end->rtp, rtp_end->local_port) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to create RTP port: %s:%d on 0x%x\n",
		       cfg->source_addr, rtp_end->local_port, endpno);
		goto cleanup0;
	}

	if (create_bind(cfg->source_addr, &rtp_end->rtcp, rtp_end->local_port + 1) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to create RTCP port: %s:%d on 0x%x\n",
		       cfg->source_addr, rtp_end->local_port + 1, endpno);
		goto cleanup1;
	}

	set_ip_tos(rtp_end->rtp.fd, cfg->endp_dscp);
	set_ip_tos(rtp_end->rtcp.fd, cfg->endp_dscp);

	rtp_end->rtp.when = BSC_FD_READ;
	if (bsc_register_fd(&rtp_end->rtp) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to register RTP port %d on 0x%x\n",
			rtp_end->local_port, endpno);
		goto cleanup2;
	}

	rtp_end->rtcp.when = BSC_FD_READ;
	if (bsc_register_fd(&rtp_end->rtcp) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to register RTCP port %d on 0x%x\n",
			rtp_end->local_port + 1, endpno);
		goto cleanup3;
	}

	return 0;

cleanup3:
	bsc_unregister_fd(&rtp_end->rtp);
cleanup2:
	close(rtp_end->rtcp.fd);
	rtp_end->rtcp.fd = -1;
cleanup1:
	close(rtp_end->rtp.fd);
	rtp_end->rtp.fd = -1;
cleanup0:
	return -1;
}

int mgcp_bind_bts_rtp_port(struct mgcp_endpoint *endp, int rtp_port)
{
	if (endp->bts_end.rtp.fd != -1 || endp->bts_end.rtcp.fd != -1) {
		LOGP(DMGCP, LOGL_ERROR, "Previous bts-port was still bound on %d\n",
			ENDPOINT_NUMBER(endp));
		mgcp_free_rtp_port(&endp->bts_end);
	}

	endp->bts_end.local_port = rtp_port;
	endp->bts_end.rtp.cb = rtp_data_bts;
	endp->bts_end.rtp.data = endp;
	endp->bts_end.rtcp.data = endp;
	endp->bts_end.rtcp.cb = rtp_data_bts;
	return bind_rtp(endp->cfg, &endp->bts_end, ENDPOINT_NUMBER(endp));
}

int mgcp_bind_net_rtp_port(struct mgcp_endpoint *endp, int rtp_port)
{
	if (endp->net_end.rtp.fd != -1 || endp->net_end.rtcp.fd != -1) {
		LOGP(DMGCP, LOGL_ERROR, "Previous net-port was still bound on %d\n",
			ENDPOINT_NUMBER(endp));
		mgcp_free_rtp_port(&endp->net_end);
	}

	endp->net_end.local_port = rtp_port;
	endp->net_end.rtp.cb = rtp_data_net;
	endp->net_end.rtp.data = endp;
	endp->net_end.rtcp.data = endp;
	endp->net_end.rtcp.cb = rtp_data_net;
	return bind_rtp(endp->cfg, &endp->net_end, ENDPOINT_NUMBER(endp));
}

int mgcp_bind_transcoder_rtp_port(struct mgcp_endpoint *endp, int rtp_port)
{
	if (endp->transcoder_end.rtp.fd != -1 || endp->transcoder_end.rtcp.fd != -1) {
		LOGP(DMGCP, LOGL_ERROR, "Previous net-port was still bound on %d\n",
			ENDPOINT_NUMBER(endp));
		mgcp_free_rtp_port(&endp->transcoder_end);
	}

	endp->transcoder_end.local_port = rtp_port;
	endp->transcoder_end.rtp.cb = rtp_data_transcoder;
	endp->transcoder_end.rtp.data = endp;
	endp->transcoder_end.rtcp.data = endp;
	endp->transcoder_end.rtcp.cb = rtp_data_transcoder;
	return bind_rtp(endp->cfg, &endp->transcoder_end, ENDPOINT_NUMBER(endp));
}

int mgcp_free_rtp_port(struct mgcp_rtp_end *end)
{
	if (end->rtp.fd != -1) {
		close(end->rtp.fd);
		end->rtp.fd = -1;
		bsc_unregister_fd(&end->rtp);
	}

	if (end->rtcp.fd != -1) {
		close(end->rtcp.fd);
		end->rtcp.fd = -1;
		bsc_unregister_fd(&end->rtcp);
	}

	return 0;
}
