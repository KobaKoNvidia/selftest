// SPDX-License-Identifier: GPL-2.0
/*
 * Resctrl tests
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

#define BENCHMARK_ARGS		64
#define BENCHMARK_ARG_SIZE	64

static void cmd_help(void)
{
	printf("usage: resctrl_tests [-h] [-b \"benchmark_cmd [options]\"] [-t test list]\n");
	printf("\t-b benchmark_cmd [options]: run specified benchmark\n");
	printf("\t default benchmark is builtin fill_buf\n");
	printf("\t-t test list: run tests specified in the test list, ");
	printf("e.g. -t mbm,mba\n");
	printf("\t-h: help\n");
}

void tests_cleanup(void)
{
	mbm_test_cleanup();
	mba_test_cleanup();
}

int main(int argc, char **argv)
{
	char benchmark_cmd_area[BENCHMARK_ARGS][BENCHMARK_ARG_SIZE];
	int res, c, cpu_no = 1, span = 250, argc_new = argc, i;
	int ben_ind;
	bool has_ben = false, mbm_test = true, mba_test = true;
	char *benchmark_cmd[BENCHMARK_ARGS];
	char bw_report[64], bm_type[64];

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-b") == 0) {
			ben_ind = i + 1;
			argc_new = ben_ind - 1;
			has_ben = 1;
			break;
		}
	}

	while ((c = getopt(argc_new, argv, "ht:b:")) != -1) {
		char *token;

		switch (c) {
		case 't':
			token = strtok(optarg, ",");

			mbm_test = false;
			mba_test = false;
			while (token) {
				if (!strcmp(token, "mbm")) {
					mbm_test = true;
				} else if (!strcmp(token, "mba")) {
					mba_test = true;
				} else {
					printf("invalid argument\n");

					return -1;
				}
				token = strtok(NULL, ":\t");
			}
			break;
		case 'h':
			cmd_help();

			return 0;
		default:
			printf("invalid argument\n");

			return -1;
		}
	}

	printf("TAP version 13\n");

	/*
	 * Typically we need root privileges, because:
	 * 1. We write to resctrl FS
	 * 2. We execute perf commands
	 */
	if (geteuid() != 0)
		printf("# WARNING: not running as root, tests may fail.\n");

	if (!has_ben) {
		/* If no benchmark is given by "-b" argument, use fill_buf. */
		for (i = 0; i < 5; i++)
			benchmark_cmd[i] = benchmark_cmd_area[i];
		strcpy(benchmark_cmd[0], "fill_buf");
		sprintf(benchmark_cmd[1], "%d", span);
		strcpy(benchmark_cmd[2], "1");
		strcpy(benchmark_cmd[3], "1");
		strcpy(benchmark_cmd[4], "0");
		benchmark_cmd[5] = NULL;
	}

	sprintf(bw_report, "reads");
	sprintf(bm_type, "fill_buf");

	check_resctrlfs_support();
	filter_dmesg();

	if (mbm_test) {
		printf("# Starting MBM BW change ...\n");
		res = mbm_bw_change(span, cpu_no, bw_report, benchmark_cmd);
		printf("%sok MBM: bw change\n", res ? "not " : "");
		tests_run++;
	}

	if (mba_test) {
		printf("# Starting MBA Schemata change ...\n");
		if (!has_ben)
			sprintf(benchmark_cmd[1], "%d", span);
		res = mba_schemata_change(cpu_no, bw_report, benchmark_cmd);
		printf("%sok MBA: schemata change\n", res ? "not " : "");
		tests_run++;
	}

	printf("1..%d\n", tests_run);

	return 0;
}