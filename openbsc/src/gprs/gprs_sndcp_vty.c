/* VTY interface for our GPRS SNDCP implementation */

/* (C) 2010 by Harald Welte <laforge@gnumonks.org>
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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include <arpa/inet.h>

#include <openbsc/gsm_data.h>
#include <osmocore/msgb.h>
#include <osmocore/tlv.h>
#include <osmocore/talloc.h>
#include <osmocore/select.h>
#include <osmocore/rate_ctr.h>
#include <openbsc/debug.h>
#include <openbsc/signal.h>
#include <openbsc/gprs_llc.h>

#include "gprs_sndcp.h"

#include <osmocom/vty/vty.h>
#include <osmocom/vty/command.h>

static void vty_dump_sne(struct vty *vty, struct gprs_sndcp_entity *sne)
{
	unsigned int i;

	vty_out(vty, " TLLI %08x SAPI=%u NSAPI=%u:%s",
		sne->lle->llme->tlli, sne->lle->sapi, sne->nsapi, VTY_NEWLINE);
	vty_out(vty, "  Defrag: npdu=%u highest_seg=%u seg_have=0x%08x tot_len=%u%s",
		sne->defrag.npdu, sne->defrag.highest_seg, sne->defrag.seg_have,
		sne->defrag.tot_len, VTY_NEWLINE);
}


DEFUN(show_sndcp, show_sndcp_cmd,
	"show sndcp",
	SHOW_STR "Display information about the SNDCP protocol")
{
	struct gprs_sndcp_entity *sne;

	vty_out(vty, "State of SNDCP Entities%s", VTY_NEWLINE);
	llist_for_each_entry(sne, &gprs_sndcp_entities, list)
		vty_dump_sne(vty, sne);

	return CMD_SUCCESS;
}

int gprs_sndcp_vty_init(void)
{
	install_element_ve(&show_sndcp_cmd);

	return 0;
}
