/*
 *  drivers/cpufreq/cpufreq_pegasusq.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/highuid.h>
#include <linux/cpu_debug.h>
#include <linux/kthread.h>
#include <linux/suspend.h>
#include <linux/reboot.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

/* Google systrace just supports Interactive governor (option -l)
 * Just backport Interactive trace points for Ondemand governor use
 */
#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_interactive.h>

/*
 * runqueue average
 */

#define RQ_AVG_TIMER_RATE	10

struct runqueue_data {
	unsigned int nr_run_avg;
	unsigned int update_rate;
	int64_t last_time;
	int64_t total_time;
	struct delayed_work work;
	struct workqueue_struct *nr_run_wq;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;
static void rq_work_fn(struct work_struct *work);

static void start_rq_work(void)
{
	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;
	if (rq_data->nr_run_wq == NULL)
		rq_data->nr_run_wq =
			create_singlethread_workqueue("nr_run_avg");

	queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
			   msecs_to_jiffies(rq_data->update_rate));
	return;
}

static void stop_rq_work(void)
{
	if (rq_data->nr_run_wq)
		cancel_delayed_work(&rq_data->work);
	return;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}
	spin_lock_init(&rq_data->lock);
	rq_data->update_rate = RQ_AVG_TIMER_RATE;
	INIT_DELAYED_WORK_DEFERRABLE(&rq_data->work, rq_work_fn);

	return 0;
}

static void rq_work_fn(struct work_struct *work)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	if (rq_data->update_rate != 0)
		queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
				   msecs_to_jiffies(rq_data->update_rate));

	spin_unlock_irqrestore(&rq_data->lock, flags);
}

static unsigned int get_nr_run_avg(void)
{
	unsigned int nr_run_avg;
	unsigned long flags = 0;

	spin_lock_irqsave(&rq_data->lock, flags);
	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;
	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}


/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define MIN_SAMPLING_RATE			(30000)
#define MAX_HOTPLUG_RATE			(40u)
#define DEF_FREQUENCY_DOWN_DIFFERENTIAL		(10)
#define DEF_FREQUENCY_UP_THRESHOLD		(80)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(100000)
#define MICRO_FREQUENCY_DOWN_DIFFERENTIAL	(3)
#define MICRO_FREQUENCY_UP_THRESHOLD		(95)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)
#define MIN_FREQUENCY_UP_THRESHOLD		(11)
#define MAX_FREQUENCY_UP_THRESHOLD		(100)
#define DEF_SAMPLING_RATE			(50000)
#define DEF_IO_IS_BUSY				(1)
#define DEF_UI_DYNAMIC_SAMPLING_RATE		(30000)
#define DEF_UI_COUNTER				(5)
#define DEF_TWO_PHASE_FREQ			(1000000)
#define DEF_TWO_PHASE_BOTTOM_FREQ   (340000)
#define DEF_TWO_PHASE_GO_MAX_LOAD   (95)
#define DEF_UX_LOADING              (30)
#define DEF_UX_FREQ                 (0)
#define DEF_UX_BOOST_THRESHOLD      (0)
#define DEF_INPUT_BOOST_DURATION    (100000000)

/* new params */
#define DEF_MAX_CPU_LOCK			(0)
#define DEF_UP_NR_CPUS				(1)
#define DEF_CPU_UP_RATE				(10)
#define DEF_CPU_DOWN_RATE			(20)
#define DEF_FREQ_STEP				(40)
#define DEF_START_DELAY				(0)

#define UP_THRESHOLD_AT_MIN_FREQ		(90)
#define FREQ_FOR_RESPONSIVENESS			(500000)

#define HOTPLUG_DOWN_INDEX			(0)
#define HOTPLUG_UP_INDEX			(1)

/*
 * The polling frequency of this governor depends on the capability of
 * the processor. Default polling frequency is 1000 times the transition
 * latency of the processor. The governor will work on any processor with
 * transition latency <= 10mS, using appropriate sampling
 * rate.
 * For CPUs with transition latency > 10mS (mostly drivers with CPUFREQ_ETERNAL)
 * this governor will not work.
 * All times here are in uS.
 */
#define MIN_SAMPLING_RATE_RATIO			(2)

static int hotplug_rq[4][2] = {
	{0, 100}, {100, 200}, {200, 300}, {300, 0}
};

static int hotplug_freq[4][2] = {
	{0, 640000},
	{475000, 640000},
	{475000, 860000},
	{760000, 0}
};

static unsigned int min_sampling_rate;
static unsigned int def_sampling_rate;

#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

static void do_dbs_timer(struct work_struct *work);
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_PEGASUSQ
static
#endif
struct cpufreq_governor cpufreq_gov_pegasusq = {
       .name                   = "pegasusq",
       .governor               = cpufreq_governor_dbs,
       .max_transition_latency = TRANSITION_LATENCY_LIMIT,
       .owner                  = THIS_MODULE,
};

/* Sampling types */
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_iowait;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	struct work_struct up_work;
	struct work_struct down_work;
	struct cpufreq_frequency_table *freq_table;
	unsigned int freq_lo;
	unsigned int freq_lo_jiffies;
	unsigned int freq_hi_jiffies;
	unsigned int rate_mult;
	int cpu;
	unsigned int sample_type:1;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, od_cpu_dbs_info);

struct workqueue_struct *dvfs_workqueue;

static unsigned int dbs_enable;	/* number of CPUs using this policy */
static unsigned int g_ui_counter = 0;

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int down_differential;
	unsigned int ignore_nice;
	unsigned int sampling_down_factor;
	unsigned int powersave_bias;
	unsigned int io_is_busy;
	unsigned int freq_step;
	unsigned int cpu_up_rate;
	unsigned int cpu_down_rate;
	unsigned int up_nr_cpus;
	unsigned int max_cpu_lock;
	atomic_t hotplug_lock;
	unsigned int dvfs_debug;
	unsigned int max_freq;
	unsigned int min_freq;
#ifdef CONFIG_HAS_EARLYSUSPEND
	int early_suspend;
#endif
	unsigned int up_threshold_at_min_freq;
	unsigned int freq_for_responsiveness;
	unsigned int touch_poke;
    unsigned int floor_freq;
	cputime64_t floor_valid_time;
    unsigned int input_boost_duration;
	unsigned int origin_sampling_rate;
	unsigned int ui_sampling_rate;
	unsigned int ui_counter;
    struct {
        unsigned int freq;
        unsigned int loading;
        unsigned int boost_threshold;
    } ux;
} dbs_tuners_ins = {
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
	.down_differential = DEF_FREQUENCY_DOWN_DIFFERENTIAL,
	.ignore_nice = 0,
	.powersave_bias = 0,
	.touch_poke = 1,
	.floor_freq = 0UL,
	.floor_valid_time = 0ULL,
	.input_boost_duration = DEF_INPUT_BOOST_DURATION,
	.ui_sampling_rate = DEF_UI_DYNAMIC_SAMPLING_RATE,
	.freq_step = DEF_FREQ_STEP,
	.cpu_up_rate = DEF_CPU_UP_RATE,
	.cpu_down_rate = DEF_CPU_DOWN_RATE,
	.up_nr_cpus = DEF_UP_NR_CPUS,
	.max_cpu_lock = DEF_MAX_CPU_LOCK,
	.hotplug_lock = ATOMIC_INIT(0),
	.dvfs_debug = 0,
#ifdef CONFIG_HAS_EARLYSUSPEND
	.early_suspend = -1,
#endif
	.up_threshold_at_min_freq = UP_THRESHOLD_AT_MIN_FREQ,
	.freq_for_responsiveness = FREQ_FOR_RESPONSIVENESS,
	.ui_counter = DEF_UI_COUNTER,
    .ux = {
        .freq = DEF_UX_FREQ,
        .loading = DEF_UX_LOADING,
        .boost_threshold = DEF_UX_BOOST_THRESHOLD
    },
};

/*
 * CPU hotplug lock interface
 */

static atomic_t g_hotplug_count = ATOMIC_INIT(0);
static atomic_t g_hotplug_lock = ATOMIC_INIT(0);

static void apply_hotplug_lock(void)
{
	int online, possible, lock, flag;
	struct work_struct *work;
	struct cpu_dbs_info_s *dbs_info;

	/* do turn_on/off cpus */
	dbs_info = &per_cpu(od_cpu_dbs_info, 0); /* from CPU0 */
	online = num_online_cpus();
	possible = num_possible_cpus();
	lock = atomic_read(&g_hotplug_lock);
	flag = lock - online;

	if (flag == 0)
		return;

	work = flag > 0 ? &dbs_info->up_work : &dbs_info->down_work;

	pr_debug("%s online %d possible %d lock %d flag %d %d\n",
		 __func__, online, possible, lock, flag, (int)abs(flag));

	queue_work_on(dbs_info->cpu, dvfs_workqueue, work);
}

int cpufreq_pegasusq_cpu_lock(int num_core)
{
	int prev_lock;

	if (num_core < 1 || num_core > num_possible_cpus())
		return -EINVAL;

	prev_lock = atomic_read(&g_hotplug_lock);

	if (prev_lock != 0 && prev_lock < num_core)
		return -EINVAL;
	else if (prev_lock == num_core)
		atomic_inc(&g_hotplug_count);

	atomic_set(&g_hotplug_lock, num_core);
	atomic_set(&g_hotplug_count, 1);
	apply_hotplug_lock();

	return 0;
}

int cpufreq_pegasusq_cpu_unlock(int num_core)
{
	int prev_lock = atomic_read(&g_hotplug_lock);

	if (prev_lock < num_core)
		return 0;
	else if (prev_lock == num_core)
		atomic_dec(&g_hotplug_count);

	if (atomic_read(&g_hotplug_count) == 0)
		atomic_set(&g_hotplug_lock, 0);

	return 0;
}


/*
 * History of CPU usage
 */
struct cpu_usage {
	unsigned int freq;
	unsigned int load[NR_CPUS];
	unsigned int rq_avg;
};

struct cpu_usage_history {
	struct cpu_usage usage[MAX_HOTPLUG_RATE];
	unsigned int num_hist;
};

struct cpu_usage_history *hotplug_history;

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
							cputime64_t *wall)
{
	cputime64_t idle_time;
	cputime64_t cur_wall_time;
	cputime64_t busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
	busy_time = cputime64_add(kstat_cpu(cpu).cpustat.user,
			kstat_cpu(cpu).cpustat.system);

	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.irq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.softirq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.steal);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.nice);

	idle_time = cputime64_sub(cur_wall_time, busy_time);
	if (wall)
		*wall = (cputime64_t)jiffies_to_usecs(cur_wall_time);

	return (cputime64_t)jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);

	return idle_time;
}

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu, cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

/*
 * Find right freq to be set now with powersave_bias on.
 * Returns the freq_hi to be used right now and will set freq_hi_jiffies,
 * freq_lo, and freq_lo_jiffies in percpu area for averaging freqs.
 */
static unsigned int powersave_bias_target(struct cpufreq_policy *policy,
					  unsigned int freq_next,
					  unsigned int relation)
{
	unsigned int freq_req, freq_reduc, freq_avg;
	unsigned int freq_hi, freq_lo;
	unsigned int index = 0;
	unsigned int jiffies_total, jiffies_hi, jiffies_lo;
	struct cpu_dbs_info_s *dbs_info = &per_cpu(od_cpu_dbs_info,
						   policy->cpu);

	if (!dbs_info->freq_table) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_next;
	}

	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_next,
			relation, &index);
	freq_req = dbs_info->freq_table[index].frequency;
	freq_reduc = freq_req * dbs_tuners_ins.powersave_bias / 1000;
	freq_avg = freq_req - freq_reduc;

	/* Find freq bounds for freq_avg in freq_table */
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_H, &index);
	freq_lo = dbs_info->freq_table[index].frequency;
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_L, &index);
	freq_hi = dbs_info->freq_table[index].frequency;

	/* Find out how long we have to be in hi and lo freqs */
	if (freq_hi == freq_lo) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_lo;
	}
	jiffies_total = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	jiffies_hi = (freq_avg - freq_lo) * jiffies_total;
	jiffies_hi += ((freq_hi - freq_lo) / 2);
	jiffies_hi /= (freq_hi - freq_lo);
	jiffies_lo = jiffies_total - jiffies_hi;
	dbs_info->freq_lo = freq_lo;
	dbs_info->freq_lo_jiffies = jiffies_lo;
	dbs_info->freq_hi_jiffies = jiffies_hi;
	return freq_hi;
}

static void pegasusq_powersave_bias_init_cpu(int cpu)
{
	struct cpu_dbs_info_s *dbs_info = &per_cpu(od_cpu_dbs_info, cpu);
	dbs_info->freq_table = cpufreq_frequency_get_table(cpu);
	dbs_info->freq_lo = 0;
}

static void pegasusq_powersave_bias_init(void)
{
	int i;
	for_each_online_cpu(i) {
		pegasusq_powersave_bias_init_cpu(i);
	}
}

/************************** sysfs interface ************************/

static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", min_sampling_rate);
}

define_one_global_ro(sampling_rate_min);

/* cpufreq_pegasusq Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)              \
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(io_is_busy, io_is_busy);
show_one(up_threshold, up_threshold);
show_one(sampling_down_factor, sampling_down_factor);
show_one(down_differential, down_differential);
show_one(ignore_nice_load, ignore_nice);
show_one(powersave_bias, powersave_bias);
show_one(touch_poke, touch_poke);
show_one(input_boost_duration, input_boost_duration);
show_one(ui_sampling_rate, ui_sampling_rate);
show_one(ui_counter, ui_counter);
show_one(ux_freq, ux.freq);
show_one(ux_loading, ux.loading);
show_one(ux_boost_threshold, ux.boost_threshold);
show_one(freq_step, freq_step);
show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_one(up_nr_cpus, up_nr_cpus);
show_one(max_cpu_lock, max_cpu_lock);
show_one(dvfs_debug, dvfs_debug);
show_one(up_threshold_at_min_freq, up_threshold_at_min_freq);
show_one(freq_for_responsiveness, freq_for_responsiveness);
static ssize_t show_hotplug_lock(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&g_hotplug_lock));
}

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", file_name[num_core - 1][up_down]);	\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	file_name[num_core - 1][up_down] = input;			\
	return count;							\
}

show_hotplug_param(hotplug_freq, 1, 1);
show_hotplug_param(hotplug_freq, 2, 0);
show_hotplug_param(hotplug_freq, 2, 1);
show_hotplug_param(hotplug_freq, 3, 0);
show_hotplug_param(hotplug_freq, 3, 1);
show_hotplug_param(hotplug_freq, 4, 0);

show_hotplug_param(hotplug_rq, 1, 1);
show_hotplug_param(hotplug_rq, 2, 0);
show_hotplug_param(hotplug_rq, 2, 1);
show_hotplug_param(hotplug_rq, 3, 0);
show_hotplug_param(hotplug_rq, 3, 1);
show_hotplug_param(hotplug_rq, 4, 0);

store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);

store_hotplug_param(hotplug_rq, 1, 1);
store_hotplug_param(hotplug_rq, 2, 0);
store_hotplug_param(hotplug_rq, 2, 1);
store_hotplug_param(hotplug_rq, 3, 0);
store_hotplug_param(hotplug_rq, 3, 1);
store_hotplug_param(hotplug_rq, 4, 0);

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);

define_one_global_rw(hotplug_rq_1_1);
define_one_global_rw(hotplug_rq_2_0);
define_one_global_rw(hotplug_rq_2_1);
define_one_global_rw(hotplug_rq_3_0);
define_one_global_rw(hotplug_rq_3_1);
define_one_global_rw(hotplug_rq_4_0);

/**
 * update_sampling_rate - update sampling rate effective immediately if needed.
 * @new_rate: new sampling rate
 *
 * If new rate is smaller than the old, simply updaing
 * dbs_tuners_int.sampling_rate might not be appropriate. For example,
 * if the original sampling_rate was 1 second and the requested new sampling
 * rate is 10 ms because the user needs immediate reaction from pegasusq
 * governor, but not sure if higher frequency will be required or not,
 * then, the governor may change the sampling rate too late; up to 1 second
 * later. Thus, if we are reducing the sampling rate, we need to make the
 * new value effective immediately.
 */
static void update_sampling_rate(unsigned int new_rate)
{
	int cpu;

	dbs_tuners_ins.sampling_rate = new_rate
				     = max(new_rate, min_sampling_rate);

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct cpu_dbs_info_s *dbs_info;
		unsigned long next_sampling, appointed_at;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		dbs_info = &per_cpu(od_cpu_dbs_info, policy->cpu);
		cpufreq_cpu_put(policy);

		mutex_lock(&dbs_info->timer_mutex);

		if (!delayed_work_pending(&dbs_info->work)) {
			mutex_unlock(&dbs_info->timer_mutex);
			continue;
		}

		next_sampling  = jiffies + usecs_to_jiffies(new_rate);
		appointed_at = dbs_info->work.timer.expires;


		if (time_before(next_sampling, appointed_at)) {

			mutex_unlock(&dbs_info->timer_mutex);
			cancel_delayed_work_sync(&dbs_info->work);
			mutex_lock(&dbs_info->timer_mutex);

			schedule_delayed_work_on(dbs_info->cpu, &dbs_info->work,
						 usecs_to_jiffies(new_rate));

		}
		mutex_unlock(&dbs_info->timer_mutex);
	}
}

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	//dbs_tuners_ins.sampling_rate = max(input, min_sampling_rate);
	update_sampling_rate(input);
	dbs_tuners_ins.origin_sampling_rate = dbs_tuners_ins.sampling_rate;
	return count;
}

static unsigned int Touch_poke_attr[4] = {1500000, 880000, 0, 0};

static ssize_t store_touch_poke(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int ret;
	ret = sscanf(buf, "%u,%u,%u,%u", &Touch_poke_attr[0], &Touch_poke_attr[1],
		&Touch_poke_attr[2], &Touch_poke_attr[3]);
	if (ret < 4)
		return -EINVAL;

	if(Touch_poke_attr[0] == 0)
		dbs_tuners_ins.touch_poke = 0;
	else
		dbs_tuners_ins.touch_poke = 1;

	return count;
}

static ssize_t store_input_boost_duration(
   struct kobject *a,
   struct attribute *b,
   const char *buf,
   size_t count
   ) {
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    dbs_tuners_ins.input_boost_duration = input;

    return count;
}

static ssize_t store_ui_sampling_rate(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	dbs_tuners_ins.ui_sampling_rate = max(input, min_sampling_rate);

	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.io_is_busy = !!input;
	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
			input < MIN_FREQUENCY_UP_THRESHOLD) {
		return -EINVAL;
	}
	dbs_tuners_ins.up_threshold = input;
	return count;
}

static ssize_t store_down_differential(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if(ret != 1 || input > DEF_FREQUENCY_DOWN_DIFFERENTIAL ||
			input < MICRO_FREQUENCY_DOWN_DIFFERENTIAL) {
		return -EINVAL;
	}
	dbs_tuners_ins.down_differential = input;
	return count;
}

static ssize_t store_sampling_down_factor(struct kobject *a,
			struct attribute *b, const char *buf, size_t count)
{
	unsigned int input, j;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;
	dbs_tuners_ins.sampling_down_factor = input;

	/* Reset down sampling multiplier in case it was active */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(od_cpu_dbs_info, j);
		dbs_info->rate_mult = 1;
	}
	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_tuners_ins.ignore_nice) { /* nothing to do */
		return count;
	}
	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(od_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;

	}
	return count;
}

static ssize_t store_powersave_bias(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 1000)
		input = 1000;

	dbs_tuners_ins.powersave_bias = input;
	pegasusq_powersave_bias_init();
	return count;
}

static ssize_t store_ui_counter(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if(ret != 1)
		return -EINVAL;

	dbs_tuners_ins.ui_counter = input;
	return count;
}

static ssize_t store_ux_freq (
   struct kobject *a,
   struct attribute *b,
   const char *buf,
   size_t count
   )
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if(ret != 1)
		return -EINVAL;

	dbs_tuners_ins.ux.freq = input;
	return count;
}

static ssize_t store_ux_loading (
   struct kobject *a,
   struct attribute *b,
   const char *buf,
   size_t count
   )
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if(ret != 1)
		return -EINVAL;

	dbs_tuners_ins.ux.loading = input;
	return count;
}

static ssize_t store_ux_boost_threshold (
   struct kobject *a,
   struct attribute *b,
   const char *buf,
   size_t count
   )
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if(ret != 1)
		return -EINVAL;

	dbs_tuners_ins.ux.boost_threshold = input;
	return count;
}

static ssize_t store_freq_step(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.freq_step = min(input, 100u);
	return count;
}

static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.cpu_up_rate = min(input, MAX_HOTPLUG_RATE);
	return count;
}

static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.cpu_down_rate = min(input, MAX_HOTPLUG_RATE);
	return count;
}


static ssize_t store_up_nr_cpus(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.up_nr_cpus = min(input, num_possible_cpus());
	return count;
}

static ssize_t store_max_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.max_cpu_lock = min(input, num_possible_cpus());
	return count;
}

static ssize_t store_hotplug_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	int prev_lock;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	input = min(input, num_possible_cpus());
	prev_lock = atomic_read(&dbs_tuners_ins.hotplug_lock);

	if (prev_lock)
		cpufreq_pegasusq_cpu_unlock(prev_lock);

	if (input == 0) {
		atomic_set(&dbs_tuners_ins.hotplug_lock, 0);
		return count;
	}

	ret = cpufreq_pegasusq_cpu_lock(input);
	if (ret) {
		pr_info("[PEGASUSQ] [HOTPLUG] already locked with smaller value %d < %d\n",
			atomic_read(&g_hotplug_lock), input);
		return ret;
	}

	atomic_set(&dbs_tuners_ins.hotplug_lock, input);

	return count;
}

static ssize_t store_dvfs_debug(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.dvfs_debug = input > 0;
	return count;
}

static ssize_t store_up_threshold_at_min_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
	    input < MIN_FREQUENCY_UP_THRESHOLD) {
		return -EINVAL;
	}
	dbs_tuners_ins.up_threshold_at_min_freq = input;
	return count;
}

static ssize_t store_freq_for_responsiveness(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.freq_for_responsiveness = input;
	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(io_is_busy);
define_one_global_rw(up_threshold);
define_one_global_rw(down_differential);
define_one_global_rw(sampling_down_factor);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(powersave_bias);
define_one_global_rw(touch_poke);
define_one_global_rw(input_boost_duration);
define_one_global_rw(ui_sampling_rate);
define_one_global_rw(ui_counter);
define_one_global_rw(ux_freq);
define_one_global_rw(ux_loading);
define_one_global_rw(ux_boost_threshold);
define_one_global_rw(freq_step);
define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(up_nr_cpus);
define_one_global_rw(max_cpu_lock);
define_one_global_rw(hotplug_lock);
define_one_global_rw(dvfs_debug);
define_one_global_rw(up_threshold_at_min_freq);
define_one_global_rw(freq_for_responsiveness);

static struct attribute *dbs_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&up_threshold.attr,
	&down_differential.attr,
	&sampling_down_factor.attr,
	&ignore_nice_load.attr,
	&powersave_bias.attr,
	&io_is_busy.attr,
&down_differential.attr,
	&freq_step.attr,
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&up_nr_cpus.attr,
	/* priority: hotplug_lock > max_cpu_lock */
	&max_cpu_lock.attr,
	&hotplug_lock.attr,
	&dvfs_debug.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
	&up_threshold_at_min_freq.attr,
	&freq_for_responsiveness.attr,
	&touch_poke.attr,
    &input_boost_duration.attr,
	&ui_sampling_rate.attr,
	&ui_counter.attr,
    &ux_freq.attr,
    &ux_loading.attr,
    &ux_boost_threshold.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "pegasusq",
};

/************************** sysfs end ************************/

static void cpu_up_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_up = dbs_tuners_ins.up_nr_cpus;
	int hotplug_lock = atomic_read(&g_hotplug_lock);
	if (hotplug_lock)
		nr_up = hotplug_lock - online;

	if (online == 1) {
		printk(KERN_ERR "CPU_UP 3\n");
		cpu_up(num_possible_cpus() - 1);
		nr_up -= 1;
	}

	for_each_cpu_not(cpu, cpu_online_mask) {
		if (nr_up-- == 0)
			break;
		if (cpu == 0)
			continue;
		printk(KERN_ERR "CPU_UP %d\n", cpu);
		cpu_up(cpu);
	}
}

static void cpu_down_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_down = 1;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock)
		nr_down = online - hotplug_lock;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		printk(KERN_ERR "CPU_DOWN %d\n", cpu);
		cpu_down(cpu);
		if (--nr_down == 0)
			break;
	}
}

static void dbs_freq_increase(struct cpufreq_policy *p, unsigned int load, unsigned int freq)
{
	if (dbs_tuners_ins.powersave_bias)
		freq = powersave_bias_target(p, freq, CPUFREQ_RELATION_H);
	//else if (p->cur == p->max)
	//	return;

    trace_cpufreq_interactive_target (p->cpu, load, p->cur, freq);

	__cpufreq_driver_target(p, freq, dbs_tuners_ins.powersave_bias ?
			CPUFREQ_RELATION_L : CPUFREQ_RELATION_H);

    trace_cpufreq_interactive_up (p->cpu, freq, p->cur);
}

/*
 * print hotplug debugging info.
 * which 1 : UP, 0 : DOWN
 */
static void debug_hotplug_check(int which, int rq_avg, int freq,
			 struct cpu_usage *usage)
{
	int cpu;
	printk(KERN_ERR "CHECK %s rq %d.%02d freq %d [", which ? "up" : "down",
	       rq_avg / 100, rq_avg % 100, freq);
	for_each_online_cpu(cpu) {
		printk(KERN_ERR "(%d, %d), ", cpu, usage->load[cpu]);
	}
	printk(KERN_ERR "]\n");
}

static int check_up(void)
{
	int num_hist = hotplug_history->num_hist;
	struct cpu_usage *usage;
	int freq, rq_avg;
	int i;
	int up_rate = dbs_tuners_ins.cpu_up_rate;
	int up_freq, up_rq;
	int min_freq = INT_MAX;
	int min_rq_avg = INT_MAX;
	int online;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock > 0)
		return 0;

	online = num_online_cpus();
	up_freq = hotplug_freq[online - 1][HOTPLUG_UP_INDEX];
	up_rq = hotplug_rq[online - 1][HOTPLUG_UP_INDEX];

	if (online == num_possible_cpus())
		return 0;
	if (dbs_tuners_ins.max_cpu_lock != 0
		&& online >= dbs_tuners_ins.max_cpu_lock)
		return 0;

	if (num_hist == 0 || num_hist % up_rate)
		return 0;

	for (i = num_hist - 1; i >= num_hist - up_rate; --i) {
		usage = &hotplug_history->usage[i];

		freq = usage->freq;
		rq_avg =  usage->rq_avg;

		min_freq = min(min_freq, freq);
		min_rq_avg = min(min_rq_avg, rq_avg);

		if (dbs_tuners_ins.dvfs_debug)
			debug_hotplug_check(1, rq_avg, freq, usage);
	}

	if (min_freq >= up_freq && min_rq_avg > up_rq) {
		pr_info("[PEGASUSQ] [HOTPLUG IN] %s %d>=%d && %d>%d\n",
			__func__, min_freq, up_freq, min_rq_avg, up_rq);
		hotplug_history->num_hist = 0;
		return 1;
	}
	return 0;
}

static int check_down(void)
{
	int num_hist = hotplug_history->num_hist;
	struct cpu_usage *usage;
	int freq, rq_avg;
	int i;
	int down_rate = dbs_tuners_ins.cpu_down_rate;
	int down_freq, down_rq;
	int max_freq = 0;
	int max_rq_avg = 0;
	int online;
	int hotplug_lock = atomic_read(&g_hotplug_lock);

	if (hotplug_lock > 0)
		return 0;

	online = num_online_cpus();
	down_freq = hotplug_freq[online - 1][HOTPLUG_DOWN_INDEX];
	down_rq = hotplug_rq[online - 1][HOTPLUG_DOWN_INDEX];

	if (online == 1)
		return 0;

	if (dbs_tuners_ins.max_cpu_lock != 0
		&& online > dbs_tuners_ins.max_cpu_lock)
		return 1;

	if (num_hist == 0 || num_hist % down_rate)
		return 0;

	for (i = num_hist - 1; i >= num_hist - down_rate; --i) {
		usage = &hotplug_history->usage[i];

		freq = usage->freq;
		rq_avg =  usage->rq_avg;

		max_freq = max(max_freq, freq);
		max_rq_avg = max(max_rq_avg, rq_avg);

		if (dbs_tuners_ins.dvfs_debug)
			debug_hotplug_check(0, rq_avg, freq, usage);
	}

	if (max_freq <= down_freq && max_rq_avg <= down_rq) {
		pr_info("[PEGASUSQ] [HOTPLUG OUT] %s %d<=%d && %d<%d\n",
			__func__, max_freq, down_freq, max_rq_avg, down_rq);
		hotplug_history->num_hist = 0;
		return 1;
	}

	return 0;
}

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int max_load_freq;
	unsigned int debug_freq;
	unsigned int debug_load;
	unsigned int debug_iowait;
	int num_hist = hotplug_history->num_hist;
	int max_hotplug_rate = max(dbs_tuners_ins.cpu_up_rate,
				   dbs_tuners_ins.cpu_down_rate);
	int up_threshold = dbs_tuners_ins.up_threshold;

	struct cpufreq_policy *policy;
	unsigned int j;
    unsigned int final_up_threshold = dbs_tuners_ins.up_threshold;
    cputime64_t now = ktime_to_ns (ktime_get ());

	this_dbs_info->freq_lo = 0;
	policy = this_dbs_info->cur_policy;

	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * Every sampling_rate, we look for a the lowest
	 * frequency which can sustain the load while keeping idle time over
	 * 30%. If such a frequency exist, we try to decrease to this frequency.
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of current frequency
	 */

	hotplug_history->usage[num_hist].freq = policy->cur;
	hotplug_history->usage[num_hist].rq_avg = get_nr_run_avg();
	++hotplug_history->num_hist;

	/* Get Absolute Load - in terms of freq */
	max_load_freq = 0;

	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info_s *j_dbs_info;
		cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
		cputime64_t prev_wall_time, prev_idle_time, prev_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;
		unsigned int load, load_freq;
		int freq_avg;

		j_dbs_info = &per_cpu(od_cpu_dbs_info, j);
		prev_wall_time = j_dbs_info->prev_cpu_wall;
		prev_idle_time = j_dbs_info->prev_cpu_idle;
		prev_iowait_time = j_dbs_info->prev_cpu_iowait;

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);
		cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);

		wall_time = (unsigned int) cputime64_sub(cur_wall_time,
				j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int) cputime64_sub(cur_idle_time,
				j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		iowait_time = (unsigned int) cputime64_sub(cur_iowait_time,
				j_dbs_info->prev_cpu_iowait);
		j_dbs_info->prev_cpu_iowait = cur_iowait_time;

		if (dbs_tuners_ins.ignore_nice) {
			cputime64_t cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = cputime64_sub(kstat_cpu(j).cpustat.nice,
					 j_dbs_info->prev_cpu_nice);
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		/*
		 * For the purpose of ondemand, waiting for disk IO is an
		 * indication that you're performance critical, and not that
		 * the system is actually idle. So subtract the iowait time
		 * from the cpu idle time.
		 */

		if (dbs_tuners_ins.io_is_busy && idle_time >= iowait_time)
			idle_time -= iowait_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;
		hotplug_history->usage[num_hist].load[j] = load;

		freq_avg = __cpufreq_driver_getavg(policy, j);
		if (freq_avg <= 0)
			freq_avg = policy->cur;

		load_freq = load * freq_avg;
		if (load_freq > max_load_freq) {
			max_load_freq = load_freq;
			debug_load = load;
			debug_iowait = 100 * iowait_time / wall_time;
		}
	}

	if (g_ui_counter > 0){
		g_ui_counter--;
		if(g_ui_counter == 0)
			dbs_tuners_ins.sampling_rate = dbs_tuners_ins.origin_sampling_rate;
	}

    /* catch up with min. freq asap */
    if (policy->cur < policy->min) {
        dbs_freq_increase(policy, debug_load, policy->min);

/*        CPU_DEBUG_PRINTK(CPU_DEBUG_GOVERNOR,
                         " cpu%d,"
                         " load=%3u, iowait=%3u,"
                         " freq=%7u(%7u), counter=%d, phase=%d, min_freq=%7u",
                         policy->cpu,
                         debug_load, debug_iowait,
                         policy->min, policy->cur, counter, phase, policy->min); */
        return;
    }

    /* make boost up to phase 1 from quite low speed more easier */
    if (policy->cur < dbs_tuners_ins.ux.freq &&
        dbs_tuners_ins.ux.boost_threshold > 0)
    {
        final_up_threshold = dbs_tuners_ins.ux.boost_threshold;
    }

	/* Check for CPU hotplug */
	if (check_up()) {
		queue_work_on(this_dbs_info->cpu, dvfs_workqueue,
			      &this_dbs_info->up_work);
	} else if (check_down()) {
		queue_work_on(this_dbs_info->cpu, dvfs_workqueue,
			      &this_dbs_info->down_work);
	}
	if (hotplug_history->num_hist  == max_hotplug_rate)
		hotplug_history->num_hist = 0;

	/* Check for frequency increase */
	if (policy->cur < dbs_tuners_ins.freq_for_responsiveness) {
		up_threshold = dbs_tuners_ins.up_threshold_at_min_freq;
	}

	if (max_load_freq > up_threshold * policy->cur) {
		int inc = (policy->max * dbs_tuners_ins.freq_step) / 100;
		int target = min(policy->max, policy->cur + inc);
		/* If switching to max speed, apply sampling_down_factor */
		if (policy->cur < policy->max)
			this_dbs_info->rate_mult =
				dbs_tuners_ins.sampling_down_factor;
		debug_freq = policy->max;

		dbs_freq_increase(policy, debug_load, policy->max);

/*        CPU_DEBUG_PRINTK(CPU_DEBUG_GOVERNOR,
                         " cpu%d,"
                         " load=%3u, iowait=%3u,"
                         " freq=%7u(%7u), min_freq=%7u",
                         policy->cpu,
                         debug_load, debug_iowait,
                         debug_freq, policy->cur, policy->min); */

		return;
	}

    if (time_before64 (now, dbs_tuners_ins.floor_valid_time)) {
        trace_cpufreq_interactive_notyet (policy->cpu,
                                          debug_load,
                                          policy->cur,
                                          policy->cur);
        return;
    }

	/* Check for frequency decrease */
	/* if we cannot reduce the frequency anymore, break out early */
	if (policy->cur == policy->min) {
        trace_cpufreq_interactive_already (policy->cpu,
                                           debug_load,
                                           policy->cur,
                                           policy->cur);
		return;
    }

	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy. To be safe, DOWN_DIFFERENTIAL points under
	 * the threshold. 
	 */
	if (max_load_freq <
	    (dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential) * policy->cur)
	{
		unsigned int freq_next;
		unsigned int down_thres;
		freq_next = max_load_freq /
				(dbs_tuners_ins.up_threshold -
				 dbs_tuners_ins.down_differential);

		/* No longer fully busy, reset rate_mult */
		this_dbs_info->rate_mult = 1;

		if (freq_next < policy->min)
			freq_next = policy->min;

		down_thres = dbs_tuners_ins.up_threshold_at_min_freq
			- dbs_tuners_ins.down_differential;

		if (freq_next < dbs_tuners_ins.freq_for_responsiveness
			&& (max_load_freq / freq_next) > down_thres)
			freq_next = dbs_tuners_ins.freq_for_responsiveness;

		if (policy->cur == freq_next)
			return;

		/* NEVER go below ux_freq if current loading > ux_loading for UX sake */
		if (freq_next < dbs_tuners_ins.ux.freq &&
		    debug_load > dbs_tuners_ins.ux.loading)
		    freq_next = dbs_tuners_ins.ux.freq;

		if (!dbs_tuners_ins.powersave_bias) {
			debug_freq = freq_next;

            trace_cpufreq_interactive_target (policy->cpu,
                                              debug_load,
                                              policy->cur,
                                              freq_next);

			__cpufreq_driver_target(policy, freq_next,
					CPUFREQ_RELATION_L);
		} else {
			int freq = powersave_bias_target(policy, freq_next,
					CPUFREQ_RELATION_L);
			debug_freq = freq;

            trace_cpufreq_interactive_target (policy->cpu,
                                              debug_load,
                                              policy->cur,
                                              freq);

			__cpufreq_driver_target(policy, freq,
				CPUFREQ_RELATION_L);
		}

        trace_cpufreq_interactive_down (policy->cpu, debug_freq, policy->cur);

        CPU_DEBUG_PRINTK(CPU_DEBUG_GOVERNOR,
                         " cpu%d,"
                         " load=%3u, iowait=%3u,"
                         " freq=%7u(%7u), min_freq=%7u",
                         policy->cpu,
                         debug_load, debug_iowait,
                         debug_freq, policy->cur, policy->min);

//#endif
	}
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;
	int sample_type = dbs_info->sample_type;

	int delay;

	mutex_lock(&dbs_info->timer_mutex);

	dbs_check_cpu(dbs_info);
	/* We want all CPUs to do sampling nearly on
	 * same jiffy
	 */
	delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate
				 * dbs_info->rate_mult);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	queue_delayed_work_on(cpu, dvfs_workqueue, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(DEF_START_DELAY * 1000 * 1000
				     + dbs_tuners_ins.sampling_rate);
	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	INIT_WORK(&dbs_info->up_work, cpu_up_work);
	INIT_WORK(&dbs_info->down_work, cpu_down_work);

	queue_delayed_work_on(dbs_info->cpu, dvfs_workqueue,
			      &dbs_info->work, delay + 2 * HZ);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	cancel_delayed_work_sync(&dbs_info->work);
	cancel_work_sync(&dbs_info->up_work);
	cancel_work_sync(&dbs_info->down_work);
}

static int should_io_be_busy(void)
{
	return DEF_IO_IS_BUSY;
}

#define	AID_SYSTEM	(1000)
static void dbs_chown(void)
{
	int ret;

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/ignore_nice_load", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown ignore_nice_load returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegausq/io_is_busy", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown io_is_busy returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/powersave_bias", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown powersave_bias returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/sampling_down_factor", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown sampling_down_factor returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/sampling_rate", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown sampling_rate returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/two_phase_freq", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown two_phase_freq returns: %d", ret);

    ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/two_phase_dynamic", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown two_phase_dynamic returns: %d", ret);

    ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/two_phase_bottom_freq", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown two_phase_bottom_freq returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/up_threshold", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown up_threshold returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/down_differential", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown down_differential returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/touch_poke", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown touch_poke returns: %d", ret);

    ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/input_boost_duration", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown input_boost_duration returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/ui_sampling_rate", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown ui_sampling_rate returns: %d", ret);

	ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/ui_counter", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_warn("sys_chown ui_counter returns: %d", ret);

    ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/ux_freq", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_err("sys_chown ux_freq error: %d", ret);

    ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/ux_loading", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_err("sys_chown ux_loading error: %d", ret);

    ret = sys_chown("/sys/devices/system/cpu/cpufreq/pegasusq/ux_boost_threshold", low2highuid(AID_SYSTEM), low2highgid(0));
	if (ret)
		pr_err("sys_chown ux_boost_threshold error: %d", ret);
}

static void dbs_refresh_callback_pegasusq(struct work_struct *unused)
{
	struct cpufreq_policy *policy;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int nr_cpus;
	unsigned int touch_poke_freq;
	unsigned int cpu = smp_processor_id();

	if (lock_policy_rwsem_write(cpu) < 0)
		return;

	this_dbs_info = &per_cpu(od_cpu_dbs_info, cpu);
	policy = this_dbs_info->cur_policy;

	g_ui_counter = dbs_tuners_ins.ui_counter;
	if(dbs_tuners_ins.ui_counter > 0)
		dbs_tuners_ins.sampling_rate = dbs_tuners_ins.ui_sampling_rate;

	/* We poke the frequency base on the online cpu number */
	nr_cpus = num_online_cpus();

	touch_poke_freq = Touch_poke_attr[nr_cpus-1];

	if(touch_poke_freq == 0 || (policy && policy->cur >= touch_poke_freq)) {
		unlock_policy_rwsem_write(cpu);
		return;
	}

    if (policy) {
        __cpufreq_driver_target(policy, touch_poke_freq,
            CPUFREQ_RELATION_L);
        this_dbs_info->prev_cpu_idle = get_cpu_idle_time(cpu,
            &this_dbs_info->prev_cpu_wall);
    }

	unlock_policy_rwsem_write(cpu);
}

#if defined(CONFIG_BEST_TRADE_HOTPLUG)
extern void bthp_set_floor_cap (unsigned int floor_freq,
                                cputime64_t floor_time
                                );
#endif

static DECLARE_WORK(dbs_refresh_work, dbs_refresh_callback_pegasusq);

extern
int tegra_input_boost (
   int cpu,
   unsigned int target_freq
   );

static bool boost_task_alive = false;
static struct task_struct *input_boost_task;

static int cpufreq_pegasusq_input_boost_task (
   void *data
   )
{
    struct cpufreq_policy *policy;
    struct cpu_dbs_info_s *this_dbs_info;
    unsigned int nr_cpus;
    unsigned int touch_poke_freq;
    unsigned int cpu;

    while (1) {
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();

        if (kthread_should_stop())
            break;

        set_current_state(TASK_RUNNING);
        cpu = smp_processor_id();

        /* We poke the frequency base on the online cpu number */
        nr_cpus = num_online_cpus();
        touch_poke_freq = Touch_poke_attr[nr_cpus - 1];

        /* boost ASAP */
        if (!touch_poke_freq ||
            tegra_input_boost(cpu, touch_poke_freq) < 0)
            continue;

        dbs_tuners_ins.floor_freq = touch_poke_freq;
        dbs_tuners_ins.floor_valid_time =
            ktime_to_ns(ktime_get()) + dbs_tuners_ins.input_boost_duration;

#if defined(CONFIG_BEST_TRADE_HOTPLUG)
        bthp_set_floor_cap (dbs_tuners_ins.floor_freq,
                            dbs_tuners_ins.floor_valid_time);
#endif

        if (lock_policy_rwsem_write(cpu) < 0)
            continue;

        this_dbs_info = &per_cpu(od_cpu_dbs_info, cpu);
        if (this_dbs_info) {
            policy = this_dbs_info->cur_policy;

            g_ui_counter = dbs_tuners_ins.ui_counter;
            if (dbs_tuners_ins.ui_counter > 0)
                dbs_tuners_ins.sampling_rate = dbs_tuners_ins.ui_sampling_rate;

            if (policy) {
                this_dbs_info->prev_cpu_idle =
                   get_cpu_idle_time(cpu,
                                     &this_dbs_info->prev_cpu_wall);
            }
        }

        unlock_policy_rwsem_write(cpu);
    }

    return 0;
}

static void dbs_input_event(struct input_handle *handle, unsigned int type,
		unsigned int code, int value)
{
	if (dbs_tuners_ins.touch_poke && type == EV_SYN && code == SYN_REPORT) {
		/*schedule_work(&dbs_refresh_work);*/

        if (boost_task_alive)
            wake_up_process (input_boost_task);
    }
}

static int input_dev_filter(const char* input_dev_name)
{
	int ret = 0;
	if (strstr(input_dev_name, "touchscreen") ||
		strstr(input_dev_name, "-keypad") ||
		strstr(input_dev_name, "-nav") ||
		strstr(input_dev_name, "-oj")) {
	}
	else {
		ret = 1;
	}
	return ret;
}


static int dbs_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	/* filter out those input_dev that we don't care */
	if (input_dev_filter(dev->name))
		return 0;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void dbs_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id dbs_ids[] = {
	{ .driver_info = 1 },
	{ },
};
static struct input_handler dbs_input_handler = {
	.event          = dbs_input_event,
	.connect        = dbs_input_connect,
	.disconnect     = dbs_input_disconnect,
	.name           = "cpufreq_ond",
	.id_table       = dbs_ids,
};

//start
static int pm_notifier_call(struct notifier_block *this,
			    unsigned long event, void *ptr)
{
	static unsigned int prev_hotplug_lock;
	switch (event) {
	case PM_SUSPEND_PREPARE:
		prev_hotplug_lock = atomic_read(&g_hotplug_lock);
		atomic_set(&g_hotplug_lock, 1);
		apply_hotplug_lock();
		pr_debug("%s enter suspend\n", __func__);
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		atomic_set(&g_hotplug_lock, prev_hotplug_lock);
		if (prev_hotplug_lock)
			apply_hotplug_lock();
		prev_hotplug_lock = 0;
		pr_debug("%s exit suspend\n", __func__);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct notifier_block pm_notifier = {
	.notifier_call = pm_notifier_call,
};

static int reboot_notifier_call(struct notifier_block *this,
				unsigned long code, void *_cmd)
{
	atomic_set(&g_hotplug_lock, 1);
	return NOTIFY_DONE;
}

static struct notifier_block reboot_notifier = {
	.notifier_call = reboot_notifier_call,
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend early_suspend;
unsigned int prev_freq_step;
unsigned int prev_sampling_rate;
static void cpufreq_pegasusq_early_suspend(struct early_suspend *h)
{
	dbs_tuners_ins.early_suspend =
		atomic_read(&g_hotplug_lock);
	prev_freq_step = dbs_tuners_ins.freq_step;
	prev_sampling_rate = dbs_tuners_ins.sampling_rate;
	dbs_tuners_ins.freq_step = 20;
	dbs_tuners_ins.sampling_rate *= 4;
	atomic_set(&g_hotplug_lock, 1);
	apply_hotplug_lock();
	stop_rq_work();
}
static void cpufreq_pegasusq_late_resume(struct early_suspend *h)
{
	atomic_set(&g_hotplug_lock, dbs_tuners_ins.early_suspend);
	dbs_tuners_ins.early_suspend = -1;
	dbs_tuners_ins.freq_step = prev_freq_step;
	dbs_tuners_ins.sampling_rate = prev_sampling_rate;
	apply_hotplug_lock();
	start_rq_work();
}
#endif

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;
    struct sched_param param = { .sched_priority = 1 };

	this_dbs_info = &per_cpu(od_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		dbs_tuners_ins.max_freq = policy->max;
		dbs_tuners_ins.min_freq = policy->min;
		hotplug_history->num_hist = 0;
		start_rq_work();

		mutex_lock(&dbs_mutex);

        if (!cpu) {
            if (!boost_task_alive) {
                input_boost_task = kthread_create (
                   cpufreq_pegasusq_input_boost_task,
                   NULL,
                   "kinputboostd"
                   );

                if (IS_ERR(input_boost_task)) {
                    mutex_unlock(&dbs_mutex);
                    return PTR_ERR(input_boost_task);
                }

                sched_setscheduler_nocheck(input_boost_task, SCHED_RR, &param);
                get_task_struct(input_boost_task);
                boost_task_alive = true;
            }
        }

		dbs_enable++;
		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(od_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kstat_cpu(j).cpustat.nice;
			}
		}
		this_dbs_info->cpu = cpu;
		this_dbs_info->rate_mult = 1;
		pegasusq_powersave_bias_init_cpu(cpu);
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			unsigned int latency;

			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}

			dbs_chown();

			/* policy latency is in nS. Convert it to uS first */
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;
			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate,
					MIN_LATENCY_MULTIPLIER * latency);
			dbs_tuners_ins.sampling_rate =
				max(min_sampling_rate,
				    latency * LATENCY_MULTIPLIER);
			if (def_sampling_rate)
				dbs_tuners_ins.sampling_rate = def_sampling_rate;
			dbs_tuners_ins.origin_sampling_rate = dbs_tuners_ins.sampling_rate;
			dbs_tuners_ins.io_is_busy = 0;
		}
		if (!cpu)
			rc = input_register_handler(&dbs_input_handler);

		mutex_unlock(&dbs_mutex);

		register_reboot_notifier(&reboot_notifier);

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_timer_init(this_dbs_info);
#ifdef CONFIG_HAS_EARLYSUSPEND
		register_early_suspend(&early_suspend);
#endif
		break;

	case CPUFREQ_GOV_STOP:
#ifdef CONFIG_HAS_EARLYSUSPEND
		unregister_early_suspend(&early_suspend);
#endif
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		mutex_destroy(&this_dbs_info->timer_mutex);
		unregister_reboot_notifier(&reboot_notifier);
		dbs_enable--;

		if (!cpu)
			input_unregister_handler(&dbs_input_handler);
		stop_rq_work();
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);
		mutex_unlock(&dbs_mutex);
		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);
		break;
	}
	return 0;
}

static int __init cpufreq_gov_dbs_init(void)
{
//start
	int ret;

	ret = init_rq_avg();
	if (ret)
		return ret;

	hotplug_history = kzalloc(sizeof(struct cpu_usage_history), GFP_KERNEL);
	if (!hotplug_history) {
		pr_err("%s cannot create hotplug history array\n", __func__);
		ret = -ENOMEM;
		goto err_hist;
	}

	dvfs_workqueue = create_workqueue("kpegasusq");
	if (!dvfs_workqueue) {
		pr_err("%s cannot create workqueue\n", __func__);
		ret = -ENOMEM;
		goto err_queue;
	}

	ret = cpufreq_register_governor(&cpufreq_gov_pegasusq);
	if (ret)
		goto err_reg;

#ifdef CONFIG_HAS_EARLYSUSPEND
	early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	early_suspend.suspend = cpufreq_pegasusq_early_suspend;
	early_suspend.resume = cpufreq_pegasusq_late_resume;
#endif
//	manage_auto_hotplug(0);
	return ret;

err_reg:
	destroy_workqueue(dvfs_workqueue);
err_queue:
	kfree(hotplug_history);
err_hist:
	kfree(rq_data);
	return ret;

}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_pegasusq);
//	manage_auto_hotplug(1);
	destroy_workqueue(dvfs_workqueue);
	kfree(hotplug_history);
	kfree(rq_data);
}


MODULE_AUTHOR("Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>");
MODULE_AUTHOR("Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>");
MODULE_DESCRIPTION("'cpufreq_pegasusq' - A dynamic cpufreq governor for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_PEGASUSQ
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
