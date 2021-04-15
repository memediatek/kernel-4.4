/*
 * Copyright (C) 2016-2017 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) "[eas_ctrl]"fmt

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "boost_ctrl.h"
#include "eas_ctrl_plat.h"
#include "eas_ctrl.h"
#include "mtk_perfmgr_internal.h"
#include <mt-plat/mtk_sched.h>
#include <linux/sched.h>
#include <sched_ctl.h>

#ifdef CONFIG_TRACING
#include <linux/kallsyms.h>
#include <linux/trace_events.h>
#endif

/* boost value */
static struct mutex boost_eas;
#ifdef CONFIG_SCHED_TUNE
static int current_boost_value[NR_CGROUP];
static unsigned long policy_mask[NR_CGROUP];
#endif
static int boost_value[NR_CGROUP][EAS_MAX_KIR];
static int debug_boost_value[NR_CGROUP];
static int debug_fix_boost;

static int cur_schedplus_down_throttle_ns;
static int default_schedplus_down_throttle_ns;
static unsigned long schedplus_down_throttle_ns_policy_mask;
static int schedplus_down_throttle_ns[EAS_THRES_MAX_KIR];
static int debug_schedplus_down_throttle_nsec;

static int cur_schedplus_sync_flag;
static int default_schedplus_sync_flag;
static unsigned long schedplus_sync_flag_policy_mask;
static int schedplus_sync_flag[EAS_SYNC_FLAG_MAX_KIR];
static int debug_schedplus_sync_flag;

/* log */
static int log_enable;

#define MAX_BOOST_VALUE	(5000)
#define MIN_BOOST_VALUE	(-100)

/************************/

static void walt_mode(int enable)
{
#ifdef CONFIG_SCHED_WALT
	sched_walt_enable(LT_WALT_POWERHAL, enable);
#else
	pr_debug("walt not be configured\n");
#endif
}

void ext_launch_start(void)
{
	pr_debug("ext_launch_start\n");
	/*--feature start from here--*/
#ifdef CONFIG_TRACING
	perfmgr_trace_begin("ext_launch_start", 0, 1, 0);
#endif
	walt_mode(1);

#ifdef CONFIG_TRACING
	perfmgr_trace_end();
#endif
}

void ext_launch_end(void)
{
	pr_debug("ext_launch_end\n");
	/*--feature end from here--*/
#ifdef CONFIG_TRACING
	perfmgr_trace_begin("ext_launch_end", 0, 0, 1);
#endif
	walt_mode(0);

#ifdef CONFIG_TRACING
	perfmgr_trace_end();
#endif
}
/************************/

static int check_boost_value(int boost_value)
{
	return clamp(boost_value, MIN_BOOST_VALUE, MAX_BOOST_VALUE);
}

/************************/
int update_schedplus_down_throttle_ns(int kicker, int nsec)
{
	int i;
	int final_down_thres = -1;

	mutex_lock(&boost_eas);

	schedplus_down_throttle_ns[kicker] = nsec;

	for (i = 0; i < EAS_THRES_MAX_KIR; i++) {
		if (schedplus_down_throttle_ns[i] == -1) {
			clear_bit(i, &schedplus_down_throttle_ns_policy_mask);
			continue;
		}

		if (final_down_thres >= 0)
			final_down_thres = MAX(final_down_thres,
				schedplus_down_throttle_ns[i]);
		else
			final_down_thres = schedplus_down_throttle_ns[i];

		set_bit(i, &schedplus_down_throttle_ns_policy_mask);
	}

	cur_schedplus_down_throttle_ns = final_down_thres < 0 ?
		-1 : final_down_thres;

	if (debug_schedplus_down_throttle_nsec == -1) {
		if (cur_schedplus_down_throttle_ns >= 0)
			temporary_dvfs_down_throttle_change(1,
				cur_schedplus_down_throttle_ns);
		else
			temporary_dvfs_down_throttle_change(1,
				default_schedplus_down_throttle_ns);
	}

	pr_debug("%s %d %d %d %d", __func__, kicker, nsec,
		cur_schedplus_down_throttle_ns,
		debug_schedplus_down_throttle_nsec);

	mutex_unlock(&boost_eas);

	return 0;
}

int update_schedplus_sync_flag(int kicker, int enable)
{
	int i;
	int final_sync_flag = -1;

	mutex_lock(&boost_eas);


	schedplus_sync_flag[kicker] = clamp(enable, -1, 1);

	for (i = 0; i < EAS_SYNC_FLAG_MAX_KIR; i++) {
		if (schedplus_sync_flag[i] == -1) {
			clear_bit(i, &schedplus_sync_flag_policy_mask);
			continue;
		}

		if (final_sync_flag >= 0)
			final_sync_flag = MAX(final_sync_flag,
				schedplus_sync_flag[i]);
		else
			final_sync_flag = schedplus_sync_flag[i];

		set_bit(i, &schedplus_sync_flag_policy_mask);
	}

	cur_schedplus_sync_flag = final_sync_flag < 0 ?
		-1 : final_sync_flag;

	if (debug_schedplus_sync_flag == -1) {
		if (cur_schedplus_sync_flag >= 0)
			sysctl_sched_sync_hint_enable =
				cur_schedplus_sync_flag;
		else
			sysctl_sched_sync_hint_enable =
				default_schedplus_sync_flag;
	}
	pr_debug("%s %d %d %d %d", __func__, kicker, enable,
		cur_schedplus_sync_flag, debug_schedplus_sync_flag);

	mutex_unlock(&boost_eas);

	return cur_schedplus_sync_flag;
}

#ifdef CONFIG_SCHED_TUNE
int update_eas_boost_value(int kicker, int cgroup_idx, int value)
{
	int final_boost = 0;
	int i, len = 0, len1 = 0;

	char msg[LOG_BUF_SIZE];
	char msg1[LOG_BUF_SIZE];

	mutex_lock(&boost_eas);

	if (cgroup_idx >= NR_CGROUP) {
		mutex_unlock(&boost_eas);
		pr_debug(" cgroup_idx >= NR_CGROUP, error\n");
		perfmgr_trace_printk("cpu_ctrl", "cgroup_idx >= NR_CGROUP\n");
		return -1;
	}

	boost_value[cgroup_idx][kicker] = value;
	len += snprintf(msg + len, sizeof(msg) - len, "[%d] [%d] [%d]",
			kicker, cgroup_idx, value);

	/*ptr return error EIO:I/O error */
	if (len < 0) {
		perfmgr_trace_printk("cpu_ctrl", "return -EIO 1\n");
		mutex_unlock(&boost_eas);
		return -EIO;
	}

	for (i = 0; i < EAS_MAX_KIR; i++) {
		if (boost_value[cgroup_idx][i] == 0) {
			clear_bit(i, &policy_mask[cgroup_idx]);
			continue;
		}

		/* Always set first to handle negative input */
		if (final_boost == 0)
			final_boost = boost_value[cgroup_idx][i];
		else
			final_boost = MAX(final_boost,
				boost_value[cgroup_idx][i]);

		set_bit(i, &policy_mask[cgroup_idx]);
	}

	current_boost_value[cgroup_idx] = check_boost_value(final_boost);

	len += snprintf(msg + len, sizeof(msg) - len, "{%d} ", final_boost);
	/*ptr return error EIO:I/O error */
	if (len < 0) {
		perfmgr_trace_printk("cpu_ctrl", "return -EIO 2\n");
		mutex_unlock(&boost_eas);
		return -EIO;
	}
	len1 += snprintf(msg1 + len1, sizeof(msg1) - len1, "[0x %lx] ",
			policy_mask[cgroup_idx]);

	if (len1 < 0) {
		perfmgr_trace_printk("cpu_ctrl", "return -EIO 3\n");
		mutex_unlock(&boost_eas);
		return -EIO;
	}
	if (!debug_fix_boost)
		boost_write_for_perf_idx(cgroup_idx,
				current_boost_value[cgroup_idx]);

	if (strlen(msg) + strlen(msg1) < LOG_BUF_SIZE)
		strncat(msg, msg1, strlen(msg1));

	if (log_enable)
		pr_debug("%s\n", msg);

#ifdef CONFIG_TRACING
	perfmgr_trace_printk("eas_ctrl", msg);
#endif
	mutex_unlock(&boost_eas);

	return current_boost_value[cgroup_idx];
}
#else
int update_eas_boost_value(int kicker, int cgroup_idx, int value)
{
	return -1;
}
#endif

/****************/
static ssize_t perfmgr_perfserv_fg_boost_proc_write(struct file *filp
		, const char *ubuf, size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	data = check_boost_value(data);

	update_eas_boost_value(EAS_KIR_PERF, CGROUP_FG, data);

	return cnt;
}

static int perfmgr_perfserv_fg_boost_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", boost_value[CGROUP_FG][EAS_KIR_PERF]);

	return 0;
}

/************************************************/
static int perfmgr_current_fg_boost_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_SCHED_TUNE
	seq_printf(m, "%d\n", current_boost_value[CGROUP_FG]);
#else
	seq_printf(m, "%d\n", -1);
#endif

	return 0;
}

/************************************************************/
static ssize_t perfmgr_debug_fg_boost_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	debug_boost_value[CGROUP_FG] = check_boost_value(data);

	debug_fix_boost = debug_boost_value[CGROUP_FG] > 0 ? 1:0;

#ifdef CONFIG_SCHED_TUNE
	boost_write_for_perf_idx(CGROUP_FG,
			debug_boost_value[CGROUP_FG]);
#endif
	return cnt;
}

static int perfmgr_debug_fg_boost_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", debug_boost_value[CGROUP_FG]);

	return 0;
}
/******************************************************/
static ssize_t perfmgr_perfserv_bg_boost_proc_write(struct file *filp,
		const char *ubuf, size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	data = check_boost_value(data);

	update_eas_boost_value(EAS_KIR_PERF, CGROUP_BG, data);

	return cnt;
}

static int perfmgr_perfserv_bg_boost_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", boost_value[CGROUP_BG][EAS_KIR_PERF]);

	return 0;
}
/*******************************************************/
static int perfmgr_current_bg_boost_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_SCHED_TUNE
	seq_printf(m, "%d\n", current_boost_value[CGROUP_BG]);
#else
	seq_printf(m, "%d\n", -1);
#endif

	return 0;
}
/**************************************************/
static ssize_t perfmgr_debug_bg_boost_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	debug_boost_value[CGROUP_BG] = check_boost_value(data);

	debug_fix_boost = debug_boost_value[CGROUP_BG] > 0 ? 1:0;

#ifdef CONFIG_SCHED_TUNE
	boost_write_for_perf_idx(CGROUP_BG,
			debug_boost_value[CGROUP_BG]);
#endif

	return cnt;
}

static int perfmgr_debug_bg_boost_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", debug_boost_value[CGROUP_BG]);

	return 0;
}
/************************************************/
static ssize_t perfmgr_perfserv_ta_boost_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	data = check_boost_value(data);

	update_eas_boost_value(EAS_KIR_PERF, CGROUP_TA, data);

	return cnt;
}

static int perfmgr_perfserv_ta_boost_proc_show(
		struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", boost_value[CGROUP_TA][EAS_KIR_PERF]);

	return 0;
}
/************************************************/
static ssize_t perfmgr_boot_boost_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int cgroup = 0, data = 0;

	int rv = check_boot_boost_proc_write(&cgroup, &data, ubuf, cnt);

	if (rv != 0)
		return rv;

	data = check_boost_value(data);

	if (cgroup >= 0 && cgroup < NR_CGROUP)
		update_eas_boost_value(EAS_KIR_BOOT, cgroup, data);

	return cnt;
}

static int perfmgr_boot_boost_proc_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < NR_CGROUP; i++)
		seq_printf(m, "%d\n", boost_value[i][EAS_KIR_BOOT]);

	return 0;
}
/************************************************/
static int perfmgr_current_ta_boost_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_SCHED_TUNE
	seq_printf(m, "%d\n", current_boost_value[CGROUP_TA]);
#else
	seq_printf(m, "%d\n", -1);
#endif

	return 0;
}

/**********************************/
static ssize_t perfmgr_debug_ta_boost_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	debug_boost_value[CGROUP_TA] = check_boost_value(data);

	debug_fix_boost = debug_boost_value[CGROUP_TA] > 0 ? 1:0;

#ifdef CONFIG_SCHED_TUNE
	boost_write_for_perf_idx(CGROUP_TA,
			debug_boost_value[CGROUP_TA]);
#endif

	return cnt;
}

static int perfmgr_debug_ta_boost_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", debug_boost_value[CGROUP_TA]);

	return 0;
}

/********************************************************************/
static int ext_launch_state;
static ssize_t perfmgr_perfserv_ext_launch_mon_proc_write(struct file *filp,
		const char *ubuf, size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	if (data) {
		ext_launch_start();
		ext_launch_state = 1;
	} else {
		ext_launch_end();
		ext_launch_state = 0;
	}

	pr_debug("perfmgr_perfserv_ext_launch_mon");
	return cnt;
}

	static int
perfmgr_perfserv_ext_launch_mon_proc_show(
		struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", ext_launch_state);

	return 0;
}


static ssize_t perfmgr_perfserv_schedplus_sync_flag_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	update_schedplus_sync_flag(EAS_SYNC_FLAG_KIR_PERF, data);

	return cnt;
}

static int perfmgr_perfserv_schedplus_sync_flag_proc_show(
	struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", schedplus_sync_flag[EAS_SYNC_FLAG_KIR_PERF]);

	return 0;
}

static ssize_t perfmgr_debug_schedplus_sync_flag_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	mutex_lock(&boost_eas);
	debug_schedplus_sync_flag = data;
	if (data == -1)
		sysctl_sched_sync_hint_enable = default_schedplus_sync_flag;
	else
		sysctl_sched_sync_hint_enable = data ? 1 : 0;
	mutex_unlock(&boost_eas);

	return cnt;
}

static int perfmgr_debug_schedplus_sync_flag_proc_show(
	struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", debug_schedplus_sync_flag);

	return 0;
}

static ssize_t perfmgr_perfserv_schedplus_down_throttle_proc_write(
	struct file *filp, const char *ubuf,
	size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	update_schedplus_down_throttle_ns(EAS_THRES_KIR_PERF, data);

	return cnt;
}

static int perfmgr_perfserv_schedplus_down_throttle_proc_show(
	struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n",
		schedplus_down_throttle_ns[EAS_THRES_KIR_PERF]);

	return 0;
}

static ssize_t perfmgr_debug_schedplus_down_throttle_proc_write(
		struct file *filp, const char *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	mutex_lock(&boost_eas);
	debug_schedplus_down_throttle_nsec = data;
	if (data == -1)
		temporary_dvfs_down_throttle_change(1,
			default_schedplus_down_throttle_ns);
	else if (data >= 0)
		temporary_dvfs_down_throttle_change(1, data);
	mutex_unlock(&boost_eas);

	return cnt;
}

static int perfmgr_debug_schedplus_down_throttle_proc_show(
	struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", debug_schedplus_down_throttle_nsec);

	return 0;
}

/* Add procfs to control sysctl_sched_migration_cost */
/* sysctl_sched_migration_cost: eas_ctrl_plat.h */
static ssize_t perfmgr_m_sched_migrate_cost_n_proc_write(struct file *filp,
		const char *ubuf, size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	sysctl_sched_migration_cost = data;

	return cnt;
}

static int perfmgr_m_sched_migrate_cost_n_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_sched_migration_cost);

	return 0;
}

static ssize_t perfmgr_perfmgr_log_proc_write(
		struct file *filp, const char __user *ubuf,
		size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	log_enable = data > 0 ? 1 : 0;

	return cnt;
}

static int perfmgr_perfmgr_log_proc_show(struct seq_file *m, void *v)
{
	if (m)
		seq_printf(m, "%d\n", log_enable);
	return 0;
}

/* redundant API */
void perfmgr_forcelimit_cpuset_cancel(void)
{

}

/* boost value */
PROC_FOPS_RW(perfserv_fg_boost);
PROC_FOPS_RO(current_fg_boost);
PROC_FOPS_RW(debug_fg_boost);
PROC_FOPS_RW(perfserv_bg_boost);
PROC_FOPS_RO(current_bg_boost);
PROC_FOPS_RW(debug_bg_boost);
PROC_FOPS_RW(perfserv_ta_boost);
PROC_FOPS_RO(current_ta_boost);
PROC_FOPS_RW(debug_ta_boost);
PROC_FOPS_RW(boot_boost);

PROC_FOPS_RW(perfserv_schedplus_down_throttle);
PROC_FOPS_RW(debug_schedplus_down_throttle);
PROC_FOPS_RW(perfserv_schedplus_sync_flag);
PROC_FOPS_RW(debug_schedplus_sync_flag);

/* others */
PROC_FOPS_RW(perfserv_ext_launch_mon);
PROC_FOPS_RW(m_sched_migrate_cost_n);
PROC_FOPS_RW(perfmgr_log);

/*******************************************/
int eas_ctrl_init(struct proc_dir_entry *parent)
{
	int i, ret = 0;
#if defined(CONFIG_SCHED_TUNE)
	int j;
#endif
	struct proc_dir_entry *boost_dir = NULL;

	struct pentry {
		const char *name;
		const struct file_operations *fops;
	};

	const struct pentry entries[] = {
		/* boost value */
		PROC_ENTRY(perfserv_fg_boost),
		PROC_ENTRY(current_fg_boost),
		PROC_ENTRY(debug_fg_boost),
		PROC_ENTRY(perfserv_bg_boost),
		PROC_ENTRY(current_bg_boost),
		PROC_ENTRY(debug_bg_boost),
		PROC_ENTRY(perfserv_ta_boost),
		PROC_ENTRY(current_ta_boost),
		PROC_ENTRY(debug_ta_boost),
		PROC_ENTRY(boot_boost),

		PROC_ENTRY(perfserv_schedplus_down_throttle),
		PROC_ENTRY(debug_schedplus_down_throttle),
		PROC_ENTRY(perfserv_schedplus_sync_flag),
		PROC_ENTRY(debug_schedplus_sync_flag),

		/* log */
		PROC_ENTRY(perfmgr_log),
		/*--ext_launch--*/
		PROC_ENTRY(perfserv_ext_launch_mon),
		/*--sched migrate cost n--*/
		PROC_ENTRY(m_sched_migrate_cost_n),
	};
	mutex_init(&boost_eas);
	boost_dir = proc_mkdir("eas_ctrl", parent);

	if (!boost_dir)
		pr_debug("boost_dir null\n ");

	/* create procfs */
	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!proc_create(entries[i].name, 0644,
					boost_dir, entries[i].fops)) {
			pr_debug("%s(), create /eas_ctrl%s failed\n",
					__func__, entries[i].name);
			ret = -EINVAL;
			goto out;
		}
	}

#if defined(CONFIG_SCHED_TUNE)
	/* boost value */
	for (i = 0; i < NR_CGROUP; i++) {
		current_boost_value[i] = 0;
		for (j = 0; j < EAS_MAX_KIR; j++)
			boost_value[i][j] = 0;
	}
#endif

	default_schedplus_down_throttle_ns = 4000000;
	default_schedplus_sync_flag = 1;
	cur_schedplus_down_throttle_ns = -1;
	cur_schedplus_sync_flag = -1;
	debug_schedplus_down_throttle_nsec = -1;
	debug_schedplus_sync_flag = -1;
	for (i = 0; i < EAS_THRES_MAX_KIR; i++)
		schedplus_down_throttle_ns[i] = -1;
	for (i = 0; i < EAS_SYNC_FLAG_MAX_KIR; i++)
		schedplus_sync_flag[i] = -1;

	debug_fix_boost = 0;

out:
	return ret;
}
