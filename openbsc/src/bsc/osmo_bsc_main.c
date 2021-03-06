/* (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
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

#include <openbsc/debug.h>
#include <openbsc/gsm_data.h>
#include <openbsc/osmo_bsc.h>
#include <openbsc/osmo_bsc_rf.h>
#include <openbsc/osmo_msc_data.h>
#include <openbsc/signal.h>
#include <openbsc/vty.h>

#include <osmocore/talloc.h>
#include <osmocore/process.h>

#include <osmocom/sccp/sccp.h>

#define _GNU_SOURCE
#include <getopt.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>


#include "bscconfig.h"

static struct log_target *stderr_target;
struct gsm_network *bsc_gsmnet = 0;
static const char *config_file = "openbsc.cfg";
static const char *rf_ctl = NULL;
extern const char *openbsc_copyright;
static int daemonize = 0;

extern void bsc_vty_init(void);
extern int bsc_bootstrap_network(int (*layer4)(struct gsm_network *, int, void *), const char *cfg_file);

static void print_usage()
{
	printf("Usage: bsc_msc_ip\n");
}

static void print_help()
{
	printf("  Some useful help...\n");
	printf("  -h --help this text\n");
	printf("  -D --daemonize Fork the process into a background daemon\n");
	printf("  -d option --debug=DRLL:DCC:DMM:DRR:DRSL:DNM enable debugging\n");
	printf("  -s --disable-color\n");
	printf("  -T --timestamp. Print a timestamp in the debug output.\n");
	printf("  -c --config-file filename The config file to use.\n");
	printf("  -l --local=IP. The local address of the MGCP.\n");
	printf("  -e --log-level number. Set a global loglevel.\n");
	printf("  -r --rf-ctl NAME. A unix domain socket to listen for cmds.\n");
	printf("  -t --testmode. A special mode to provoke failures at the MSC.\n");
}

static void handle_options(int argc, char **argv)
{
	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
			{"help", 0, 0, 'h'},
			{"debug", 1, 0, 'd'},
			{"daemonize", 0, 0, 'D'},
			{"config-file", 1, 0, 'c'},
			{"disable-color", 0, 0, 's'},
			{"timestamp", 0, 0, 'T'},
			{"local", 1, 0, 'l'},
			{"log-level", 1, 0, 'e'},
			{"rf-ctl", 1, 0, 'r'},
			{"testmode", 0, 0, 't'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "hd:DsTc:e:r:t",
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
		case 'D':
			daemonize = 1;
			break;
		case 'c':
			config_file = strdup(optarg);
			break;
		case 'T':
			log_set_print_timestamp(stderr_target, 1);
			break;
		case 'P':
			ipacc_rtp_direct = 0;
			break;
		case 'e':
			log_set_log_level(stderr_target, atoi(optarg));
			break;
		case 'r':
			rf_ctl = optarg;
			break;
		default:
			/* ignore */
			break;
		}
	}
}

extern int bts_model_unknown_init(void);
extern int bts_model_bs11_init(void);
extern int bts_model_nanobts_init(void);

extern enum node_type bsc_vty_go_parent(struct vty *vty);

static struct vty_app_info vty_info = {
	.name 		= "OpenBSC Osmo BSC",
	.version	= PACKAGE_VERSION,
	.go_parent_cb	= bsc_vty_go_parent,
	.is_config_node	= bsc_vty_is_config_node,
};

extern int bsc_shutdown_net(struct gsm_network *net);
static void signal_handler(int signal)
{
	fprintf(stdout, "signal %u received\n", signal);

	switch (signal) {
	case SIGINT:
		bsc_shutdown_net(bsc_gsmnet);
		dispatch_signal(SS_GLOBAL, S_GLOBAL_SHUTDOWN, NULL);
		sleep(3);
		exit(0);
		break;
	case SIGABRT:
		/* in case of abort, we want to obtain a talloc report
		 * and then return to the caller, who will abort the process */
	case SIGUSR1:
		talloc_report(tall_vty_ctx, stderr);
		talloc_report_full(tall_bsc_ctx, stderr);
		break;
	case SIGUSR2:
		if (!bsc_gsmnet->msc_data)
			return;
		if (!bsc_gsmnet->msc_data->msc_con)
			return;
		if (!bsc_gsmnet->msc_data->msc_con->is_connected)
			return;
		bsc_msc_lost(bsc_gsmnet->msc_data->msc_con);
		break;
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	int rc;

	log_init(&log_info);
	tall_bsc_ctx = talloc_named_const(NULL, 1, "openbsc");
	stderr_target = log_target_create_stderr();
	log_add_target(stderr_target);

	bts_model_unknown_init();
	bts_model_bs11_init();
	bts_model_nanobts_init();

	/* enable filters */
	log_set_all_filter(stderr_target, 1);

	/* This needs to precede handle_options() */
	vty_info.copyright = openbsc_copyright;
	vty_init(&vty_info);
	bsc_vty_init();

	/* parse options */
	handle_options(argc, argv);

	/* seed the PRNG */
	srand(time(NULL));

	/* initialize SCCP */
	sccp_set_log_area(DSCCP);


	rc = bsc_bootstrap_network(NULL, config_file);
	if (rc < 0) {
		fprintf(stderr, "Bootstrapping the network failed. exiting.\n");
		exit(1);
	}
	bsc_api_init(bsc_gsmnet, osmo_bsc_api());

	if (rf_ctl) {
		struct osmo_msc_data *data = bsc_gsmnet->msc_data;
		data->rf_ctl = osmo_bsc_rf_create(rf_ctl, bsc_gsmnet);
		if (!data->rf_ctl) {
			fprintf(stderr, "Failed to create the RF service.\n");
			exit(1);
		}
	}

	if (osmo_bsc_msc_init(bsc_gsmnet) != 0) {
		LOGP(DNAT, LOGL_ERROR, "Failed to start up. Exiting.\n");
		exit(1);
	}

	signal(SIGINT, &signal_handler);
	signal(SIGABRT, &signal_handler);
	signal(SIGUSR1, &signal_handler);
	signal(SIGUSR2, &signal_handler);
	signal(SIGPIPE, SIG_IGN);

	if (daemonize) {
		rc = osmo_daemonize();
		if (rc < 0) {
			perror("Error during daemonize");
			exit(1);
		}
	}

	while (1) {
		bsc_select_main(0);
	}

	return 0;
}
