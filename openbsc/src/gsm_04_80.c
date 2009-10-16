/* GSM Mobile Radio Interface Layer 3 messages on the A-bis interface 
 * 3GPP TS 04.08 version 7.21.0 Release 1998 / ETSI TS 100 940 V7.21.0 */

/* (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
 * (C) 2008, 2009 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2009 by Mike Haben <michael.haben@btinternet.com>
 *
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <openbsc/msgb.h>
#include <openbsc/tlv.h>
#include <openbsc/debug.h>
#include <openbsc/gsm_data.h>
#include <openbsc/gsm_utils.h>
#include <openbsc/gsm_04_08.h>
#include <openbsc/gsm_04_80.h>

static char ussd_string_buff[32];
static u_int8_t last_transaction_id;
static u_int8_t last_invoke_id;

/* Forward declarations */
static int parse_ussd(u_int8_t *ussd);
static int parse_ussd_information_elements(u_int8_t *ussd_ie);
static int parse_facility_ie(u_int8_t *facility_ie, u_int8_t length);
static int parse_ss_invoke(u_int8_t *invoke_data, u_int8_t length);
static int parse_process_uss_req(u_int8_t *uss_req_data, u_int8_t length);

static inline unsigned char *msgb_wrap_with_TL(struct msgb *msgb, u_int8_t tag)
{
	msgb->data -= 2;
	msgb->data[0] = tag;
	msgb->data[1] = msgb->len;
	msgb->len += 2;
	return msgb->data;
}

static inline unsigned char *msgb_push_TLV1(struct msgb *msgb, u_int8_t tag, u_int8_t value)
{
	msgb->data -= 3;
	msgb->len += 3;
	msgb->data[0] = tag;
	msgb->data[1] = 1;
	msgb->data[2] = value;
	return msgb->data;
}


/* Receive a mobile-originated USSD message and return the decoded text */
char* gsm0480_rcv_ussd(struct msgb *msg)
{
	int rc = 0;
	u_int8_t* parse_ptr = msgb_l3(msg);

	memset(ussd_string_buff, 0, sizeof(ussd_string_buff));

	if ((*parse_ptr & 0x0F) == GSM48_PDISC_NC_SS) {
	    last_transaction_id = *parse_ptr & 0x70;
	    rc = parse_ussd(parse_ptr + 1);
	}

	if (!rc)
		DEBUGP(DMM, "Error occurred while parsing received USSD!\n"); 

	return ussd_string_buff;
}

static int parse_ussd(u_int8_t *ussd)
{
	int rc = 1;
	u_int8_t msg_type = ussd[0] & 0xBF;  /* message-type - section 3.4 */

	switch(msg_type) {  
	case GSM0480_MTYPE_RELEASE_COMPLETE:
		DEBUGP(DMM, "USS Release Complete\n");  /* could also parse out the optional Cause/Facility data */
		ussd_string_buff[0] = 0xFF;
		break;
	case GSM0480_MTYPE_REGISTER:
	case GSM0480_MTYPE_FACILITY:
		rc &= parse_ussd_information_elements(ussd+1);
		break;
	default:
		fprintf(stderr, "Unknown GSM 04.80 message-type field 0x%02x\n",
			ussd[0]);
		rc = 0;
		break;
	}

	return rc;
}

static int parse_ussd_information_elements(u_int8_t *ussd_ie)
{
	int rc;

	u_int8_t iei = ussd_ie[0];  /* Information Element Identifier - table 3.2 & GSM 04.08 section 10.5 */		
	u_int8_t iei_length = ussd_ie[1];  
	switch(iei) {
	case GSM48_IE_CAUSE:
		break;
	case GSM0480_IE_FACILITY:
		rc = parse_facility_ie(ussd_ie+2, iei_length);
		break;
	case GSM0480_IE_SS_VERSION:
		break;
	default:
		fprintf(stderr, "Unhandled GSM 04.08 or 04.80 Information Element Identifier 0x%02x\n",
			iei);
		rc = 0;
		break;
	}

	return rc;
}

static int parse_facility_ie(u_int8_t *facility_ie, u_int8_t length)
{
	int rc = 1;
	u_int8_t offset = 0;

	do {
		u_int8_t component_type = facility_ie[offset]; /* Component Type tag - table 3.7 */ 
		u_int8_t component_length = facility_ie[offset+1];
		switch(component_type) {
		case GSM0480_CTYPE_INVOKE:
			rc &= parse_ss_invoke(facility_ie+2, component_length);
			break;
		case GSM0480_CTYPE_RETURN_RESULT:
			break;
		case GSM0480_CTYPE_RETURN_ERROR:
			break;
		case GSM0480_CTYPE_REJECT:
			break;
		default:
			fprintf(stderr, "Unknown GSM 04.80 Facility Component Type 0x%02x\n",
				component_type);
			rc = 0;
			break;
		}
		offset += (component_length+2);
	} while(offset < length);

	return rc;
}

/* Parse an Invoke component - see table 3.3 */
static int parse_ss_invoke(u_int8_t *invoke_data, u_int8_t length)
{
	int rc = 1;
	
	if (invoke_data[0] != GSM0480_COMPIDTAG_INVOKE_ID) {  /* mandatory part */
		fprintf(stderr, "Unexpected GSM 04.80 Component-ID tag 0x%02x (expecting Invoke ID tag)\n",
				invoke_data[0]);
	}
	u_int8_t offset = invoke_data[1] + 2;
	last_invoke_id = invoke_data[2];

	if (invoke_data[offset] == GSM0480_COMPIDTAG_LINKED_ID)  /* optional part */
		offset += invoke_data[offset+1] + 2;  /* skip over it */
		
	if (invoke_data[offset] == GSM0480_OPERATION_CODE) {  /* mandatory part */
		u_int8_t operation_code = invoke_data[offset+2];
		switch(operation_code) {
		case GSM0480_OP_CODE_PROCESS_USS_REQ:
			rc = parse_process_uss_req(invoke_data + offset + 3, length - offset - 3);
			break;
		default:
			fprintf(stderr, "GSM 04.80 operation code 0x%02x is not yet handled\n",
				operation_code);
			rc = 0;
			break;
		}
	} else {
		fprintf(stderr, "Unexpected GSM 04.80 Component-ID tag 0x%02x (expecting Operation Code tag)\n",
			invoke_data[0]);
		rc = 0;
	}

	return rc;
}

/* Parse the parameters of a Process UnstructuredSS Request */
static int parse_process_uss_req(u_int8_t *uss_req_data, u_int8_t length)
{
	int rc = 1;
	int num_chars;
	u_int8_t dcs;

	/* FIXME: most phones send USSD text as a 7-bit encoded octet string; the following code
	also handles the case of plain ASCII text (IA5String), but other encodings might be used */
	if (uss_req_data[0] == GSM_0480_SEQUENCE_TAG) {
		if (uss_req_data[2] == ASN1_OCTET_STRING_TAG) {
			dcs = uss_req_data[4];
			if ((dcs == 0x0F) && (uss_req_data[5] == ASN1_OCTET_STRING_TAG)) {
				num_chars = (uss_req_data[6] * 8) / 7;
				gsm_7bit_decode(ussd_string_buff, &(uss_req_data[7]), num_chars);
			}
		}
	} else if (uss_req_data[0] == ASN1_IA5_STRING_TAG) {
		num_chars = uss_req_data[1];
		memcpy(ussd_string_buff, &(uss_req_data[2]), num_chars);
	}	

	return rc;
}

/* Send response to a mobile-originated ProcessUnstructuredSS-Request */
int gsm0480_send_ussd_response(struct msgb *in_msg, const char* response_text)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;
	u_int8_t *ptr8;
	int response_len;

	response_len = (strlen(response_text) * 7) / 8;
	if (((strlen(response_text) * 7) % 8) != 0)
		response_len += 1;

	msg->bts_link = in_msg->bts_link;
	msg->lchan = in_msg->lchan;

	/* First put the payload text into the message */
	ptr8 = msgb_put(msg, response_len);
	gsm_7bit_encode(ptr8, response_text);

	/* Then wrap it as an Octet String */
	msgb_wrap_with_TL(msg, ASN1_OCTET_STRING_TAG);

	/* Pre-pend the DCS octet string */
	msgb_push_TLV1(msg, ASN1_OCTET_STRING_TAG, 0x0F);

	/* Then wrap these as a Sequence */
	msgb_wrap_with_TL(msg, GSM_0480_SEQUENCE_TAG);

	/* Pre-pend the operation code */
	msgb_push_TLV1(msg, GSM0480_OPERATION_CODE, GSM0480_OP_CODE_PROCESS_USS_REQ);

	/* Wrap the operation code and IA5 string as a sequence */
	msgb_wrap_with_TL(msg, GSM_0480_SEQUENCE_TAG);

	/* Pre-pend the invoke ID */
	msgb_push_TLV1(msg, GSM0480_COMPIDTAG_INVOKE_ID, last_invoke_id);

	/* Wrap this up as a Return Result component */
	msgb_wrap_with_TL(msg, GSM0480_CTYPE_RETURN_RESULT);

	/* Wrap the component in a Facility message */
	msgb_wrap_with_TL(msg, GSM0480_IE_FACILITY);

	/* And finally pre-pend the L3 header */
	gh = (struct gsm48_hdr *) msgb_push(msg, sizeof(*gh));
	gh->proto_discr = GSM48_PDISC_NC_SS | last_transaction_id | (1<<7);  /* TI direction = 1 */
	gh->msg_type = GSM0480_MTYPE_RELEASE_COMPLETE;

	return gsm48_sendmsg(msg, NULL);
}

int gsm0480_send_ussd_reject(struct msgb *in_msg)
{
	struct msgb *msg = gsm48_msgb_alloc();
	struct gsm48_hdr *gh;

	msg->bts_link = in_msg->bts_link;
	msg->lchan = in_msg->lchan;

	/* First insert the problem code */
	msgb_push_TLV1(msg, GSM_0480_PROBLEM_CODE_TAG_GENERAL, GSM_0480_GEN_PROB_CODE_UNRECOGNISED);

	/* Before it insert the invoke ID */
	msgb_push_TLV1(msg, GSM0480_COMPIDTAG_INVOKE_ID, last_invoke_id);

	/* Wrap this up as a Reject component */
	msgb_wrap_with_TL(msg, GSM0480_CTYPE_REJECT);

	/* Wrap the component in a Facility message */
	msgb_wrap_with_TL(msg, GSM0480_IE_FACILITY);

	/* And finally pre-pend the L3 header */
	gh = (struct gsm48_hdr *) msgb_push(msg, sizeof(*gh));
	gh->proto_discr = GSM48_PDISC_NC_SS | last_transaction_id | (1<<7);  /* TI direction = 1 */
	gh->msg_type = GSM0480_MTYPE_RELEASE_COMPLETE;

	return gsm48_sendmsg(msg, NULL);
}
