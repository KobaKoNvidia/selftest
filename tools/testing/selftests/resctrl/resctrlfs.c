// SPDX-License-Identifier: GPL-2.0
/*
 * Basic resctrl file system operations
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

int tests_run;

static int find_resctrl_mount(char *buffer)
{
	FILE *mounts;
	char line[256], *fs, *mntpoint;

	mounts = fopen("/proc/mounts", "r");
	if (!mounts) {
		perror("/proc/mounts");
		return -ENXIO;
	}
	while (!feof(mounts)) {
		if (!fgets(line, 256, mounts))
			break;
		fs = strtok(line, " \t");
		if (!fs)
			continue;
		mntpoint = strtok(NULL, " \t");
		if (!mntpoint)
			continue;
		fs = strtok(NULL, " \t");
		if (!fs)
			continue;
		if (strcmp(fs, "resctrl"))
			continue;

		fclose(mounts);
		if (buffer)
			strncpy(buffer, mntpoint, 256);

		return 0;
	}

	fclose(mounts);

	return -ENOENT;
}

/*
 * remount_resctrlfs - Remount resctrl FS at /sys/fs/resctrl
 * @mum_resctrlfs:	Should the resctrl FS be remounted?
 *
 * If not mounted, mount it.
 * If mounted and mum_resctrlfs then remount resctrl FS.
 * If mounted and !mum_resctrlfs then noop
 *
 * Return: 0 on success, non-zero on failure
 */
int remount_resctrlfs(bool mum_resctrlfs)
{
	char mountpoint[256];
	int ret;

	ret = find_resctrl_mount(mountpoint);
	if (ret)
		strcpy(mountpoint, RESCTRL_PATH);

	if (!ret && mum_resctrlfs && umount(mountpoint)) {
		printf("not ok unmounting \"%s\"\n", mountpoint);
		perror("# umount");
		tests_run++;
	}

	if (!ret && !mum_resctrlfs)
		return 0;

	ret = mount("resctrl", RESCTRL_PATH, "resctrl", 0, NULL);
	printf("%sok mounting resctrl to \"%s\"\n", ret ? "not " : "",
	       RESCTRL_PATH);
	if (ret)
		perror("# mount");

	tests_run++;

	return ret;
}

int umount_resctrlfs(void)
{
	if (umount(RESCTRL_PATH)) {
		perror("# Unable to umount resctrl");

		return errno;
	}

	return 0;
}

/*
 * get_resource_id - Get socket number/l3 id for a specified CPU
 * @cpu_no:	CPU number
 * @resource_id: Socket number or l3_id
 *
 * Return: >= 0 on success, < 0 on failure.
 */
int get_resource_id(int cpu_no, int *resource_id)
{
	char phys_pkg_path[1024];
	FILE *fp;

	sprintf(phys_pkg_path, "%s%d/topology/physical_package_id",
		PHYS_ID_PATH, cpu_no);
	fp = fopen(phys_pkg_path, "r");
	if (!fp) {
		perror("Failed to open physical_package_id");

		return -1;
	}
	if (fscanf(fp, "%d", resource_id) <= 0) {
		perror("Could not get socket number or l3 id");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	return 0;
}

/*
 * taskset_benchmark - Taskset PID (i.e. benchmark) to a specified cpu
 * @bm_pid:	PID that should be binded
 * @cpu_no:	CPU number at which the PID would be binded
 *
 * Return: 0 on success, non-zero on failure
 */
int taskset_benchmark(pid_t bm_pid, int cpu_no)
{
	cpu_set_t my_set;

	CPU_ZERO(&my_set);
	CPU_SET(cpu_no, &my_set);

	if (sched_setaffinity(bm_pid, sizeof(cpu_set_t), &my_set)) {
		perror("Unable to taskset benchmark");

		return -1;
	}

	return 0;
}

/*
 * run_benchmark - Run a specified benchmark or fill_buf (default benchmark)
 *		   in specified signal. Direct benchmark stdio to /dev/null.
 * @signum:	signal number
 * @info:	signal info
 * @ucontext:	user context in signal handling
 *
 * Return: void
 */
void run_benchmark(int signum, siginfo_t *info, void *ucontext)
{
	unsigned long long span;
	int operation, ret;
	char **benchmark_cmd;
	FILE *fp;

	benchmark_cmd = info->si_ptr;

	/*
	 * Direct stdio of child to /dev/null, so that only parent writes to
	 * stdio (console)
	 */
	fp = freopen("/dev/null", "w", stdout);
	if (!fp)
		PARENT_EXIT("Unable to direct benchmark status to /dev/null");

	if (strcmp(benchmark_cmd[0], "fill_buf") == 0) {
		/* Execute default fill_buf benchmark */
		span = strtoul(benchmark_cmd[1], NULL, 10);
		operation = atoi(benchmark_cmd[4]);
		if (run_fill_buf(span, 1, 1, operation, NULL))
			fprintf(stderr, "Error in running fill buffer\n");
	} else {
		/* Execute specified benchmark */
		ret = execvp(benchmark_cmd[0], benchmark_cmd);
		if (ret)
			perror("wrong\n");
	}

	fclose(stdout);
	PARENT_EXIT("Unable to run specified benchmark");
}

/*
 * create_grp - Create a group only if one doesn't exist
 * @grp_name:	Name of the group
 * @grp:	Full path and name of the group
 * @parent_grp:	Full path and name of the parent group
 *
 * Return: 0 on success, non-zero on failure
 */
static int create_grp(const char *grp_name, char *grp, const char *parent_grp)
{
	int found_grp = 0;
	struct dirent *ep;
	DIR *dp;

	/* Check if requested grp exists or not */
	dp = opendir(parent_grp);
	if (dp) {
		while ((ep = readdir(dp)) != NULL) {
			if (strcmp(ep->d_name, grp_name) == 0)
				found_grp = 1;
		}
		closedir(dp);
	} else {
		perror("Unable to open resctrl for group");

		return -1;
	}

	/* Requested grp doesn't exist, hence create it */
	if (found_grp == 0) {
		if (mkdir(grp, 0) == -1) {
			perror("Unable to create group");

			return -1;
		}
	}

	return 0;
}

static int write_pid_to_tasks(char *tasks, pid_t pid)
{
	FILE *fp;

	fp = fopen(tasks, "w");
	if (!fp) {
		perror("Failed to open tasks file");

		return -1;
	}
	if (fprintf(fp, "%d\n", pid) < 0) {
		perror("Failed to wr pid to tasks file");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	return 0;
}

/*
 * write_bm_pid_to_resctrl - Write a PID (i.e. benchmark) to resctrl FS
 * @bm_pid:		PID that should be written
 * @ctrlgrp:		Name of the control monitor group (con_mon grp)
 * @mongrp:		Name of the monitor group (mon grp)
 * @resctrl_val:	Resctrl feature (Eg: mbm, mba.. etc)
 *
 * If a con_mon grp is requested, create it and write pid to it, otherwise
 * write pid to root con_mon grp.
 * If a mon grp is requested, create it and write pid to it, otherwise
 * pid is not written, this means that pid is in con_mon grp and hence
 * should consult con_mon grp's mon_data directory for results.
 *
 * Return: 0 on success, non-zero on failure
 */
int write_bm_pid_to_resctrl(pid_t bm_pid, char *ctrlgrp, char *mongrp,
			    char *resctrl_val)
{
	char controlgroup[128], monitorgroup[512], monitorgroup_p[256];
	char tasks[256];
	int ret = 0;

	if (ctrlgrp)
		sprintf(controlgroup, "%s/%s", RESCTRL_PATH, ctrlgrp);
	else
		sprintf(controlgroup, "%s", RESCTRL_PATH);

	/* Create control and monitoring group and write pid into it */
	ret = create_grp(ctrlgrp, controlgroup, RESCTRL_PATH);
	if (ret)
		goto out;
	sprintf(tasks, "%s/tasks", controlgroup);
	ret = write_pid_to_tasks(tasks, bm_pid);
	if (ret)
		goto out;

	/* Create mon grp and write pid into it for "mbm" test */
	if ((strcmp(resctrl_val, "mbm") == 0)) {
		if (mongrp) {
			sprintf(monitorgroup_p, "%s/mon_groups", controlgroup);
			sprintf(monitorgroup, "%s/%s", monitorgroup_p, mongrp);
			ret = create_grp(mongrp, monitorgroup, monitorgroup_p);
			if (ret)
				goto out;

			sprintf(tasks, "%s/mon_groups/%s/tasks",
				controlgroup, mongrp);
			ret = write_pid_to_tasks(tasks, bm_pid);
			if (ret)
				goto out;
		}
	}

out:
	printf("%sok writing benchmark parameters to resctrl FS\n",
	       ret ? "not " : "");
	if (ret)
		perror("# writing to resctrlfs");

	tests_run++;

	return ret;
}

/*
 * write_schemata - Update schemata of a con_mon grp
 * @ctrlgrp:		Name of the con_mon grp
 * @schemata:		Schemata that should be updated to
 * @cpu_no:		CPU number that the benchmark PID is binded to
 * @resctrl_val:	Resctrl feature (Eg: mbm, mba.. etc)
 *
 * Update schemata of a con_mon grp *only* if requested resctrl feature is
 * allocation type
 *
 * Return: 0 on success, non-zero on failure
 */
int write_schemata(char *ctrlgrp, char *schemata, int cpu_no, char *resctrl_val)
{
	char controlgroup[1024], schema[1024], reason[64];
	int resource_id, ret = 0;
	FILE *fp;

	if (strcmp(resctrl_val, "mba") != 0)
		return -ENOENT;

	if (!schemata) {
		printf("# Skipping empty schemata update\n");

		return -1;
	}

	if (get_resource_id(cpu_no, &resource_id) < 0) {
		sprintf(reason, "Failed to get resource id");
		ret = -1;

		goto out;
	}

	if (strlen(ctrlgrp) != 0)
		sprintf(controlgroup, "%s/%s/schemata", RESCTRL_PATH, ctrlgrp);
	else
		sprintf(controlgroup, "%s/schemata", RESCTRL_PATH);

	sprintf(schema, "%s%d%c%s", "MB:", resource_id, '=', schemata);

	fp = fopen(controlgroup, "w");
	if (!fp) {
		sprintf(reason, "Failed to open control group");
		ret = -1;

		goto out;
	}

	if (fprintf(fp, "%s\n", schema) < 0) {
		sprintf(reason, "Failed to write schemata in control group");
		fclose(fp);
		ret = -1;

		goto out;
	}
	fclose(fp);

out:
	printf("%sok Write schema \"%s\" to resctrl FS%s%s\n",
	       ret ? "not " : "", schema, ret ? " # " : "",
	       ret ? reason : "");
	tests_run++;

	return ret;
}

char *fgrep(FILE *inf, const char *str)
{
	char line[256];
	int slen = strlen(str);

	while (!feof(inf)) {
		if (!fgets(line, 256, inf))
			break;
		if (strncmp(line, str, slen))
			continue;

		return strdup(line);
	}

	return NULL;
}

/*
 * validate_resctrl_feature_request - Check if requested feature is valid.
 * @resctrl_val:	Requested feature
 *
 * Return: 0 on success, non-zero on failure
 */
bool validate_resctrl_feature_request(char *resctrl_val)
{
	FILE *inf = fopen("/proc/cpuinfo", "r");
	bool found = false;
	char *res;

	if (!inf)
		return false;

	res = fgrep(inf, "flags");

	if (res) {
		char *s = strchr(res, ':');

		found = s && !strstr(s, resctrl_val);
		free(res);
	}
	fclose(inf);

	return found;
}

int validate_bw_report_request(char *bw_report)
{
	if (strcmp(bw_report, "reads") == 0)
		return 0;
	if (strcmp(bw_report, "writes") == 0)
		return 0;
	if (strcmp(bw_report, "nt-writes") == 0) {
		strcpy(bw_report, "writes");
		return 0;
	}
	if (strcmp(bw_report, "total") == 0)
		return 0;

	fprintf(stderr, "Requested iMC B/W report type unavailable\n");

	return -1;
}

int perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
		    int group_fd, unsigned long flags)
{
	int ret;

	ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
		      group_fd, flags);
	return ret;
}