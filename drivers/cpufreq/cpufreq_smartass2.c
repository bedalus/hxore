/*
 * drivers/cpufreq/cpufreq_smartmax.c
 *
 * Copyright (C) 2013 maxwen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Author: maxwen
 *
 * Based on the ondemand and smartassV2 governor
 *
 * ondemand:
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *
 * smartassV2:
 * Author: Erasmux
 *
 * For a general overview of CPU governors see the relavent part in
 * Documentation/cpu-freq/governors.txt
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/moduleparam.h>
#include <asm/cputime.h>
#include <linux/earlysuspend.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include "../../arch/arm/mach-tegra/hxore.h"

static int boost_counter = 0;

/******************** Tunable parameters: ********************/

/*
 * The "ideal" frequency to use. The governor will ramp up faster
 * towards the ideal frequency and slower after it has passed it. Similarly,
 * lowering the frequency towards the ideal frequency is faster than below it.
 */
#define DEFAULT_IDEAL_FREQ 910000 // this seems to be the lowest fq at which everything is smooth enough
static unsigned int ideal_freq;

/*
 * Freqeuncy delta when ramping up above the ideal freqeuncy.
 * Zero disables and causes to always jump straight to max frequency.
 * When below the ideal freqeuncy we always ramp up to the ideal freq.
 */
#define DEFAULT_RAMP_UP_STEP 51000
static unsigned int ramp_up_step;

/*
 * Freqeuncy delta when ramping down below the ideal freqeuncy.
 * Zero disables and will calculate ramp down according to load heuristic.
 * When above the ideal freqeuncy we always ramp down to the ideal freq.
 */        
#define DEFAULT_RAMP_DOWN_STEP 51000
static unsigned int ramp_down_step;

/*
 * CPU freq will be increased if measured load > max_cpu_load;
 */
#define DEFAULT_MAX_CPU_LOAD 96
static unsigned int max_cpu_load;

/*
 * CPU freq will be decreased if measured load < min_cpu_load;
 */
#define DEFAULT_MIN_CPU_LOAD 40
static unsigned int min_cpu_load;

/*
 * The minimum amount of time in usecs to spend at a frequency before we can ramp up.
 * Notice we ignore this when we are below the ideal frequency.
 */
#define DEFAULT_UP_RATE 90000
static unsigned int up_rate;

/*
 * The minimum amount of time in usecs to spend at a frequency before we can ramp down.
 * Notice we ignore this when we are above the ideal frequency.
 */
#define DEFAULT_DOWN_RATE 30000
static unsigned int down_rate;

/* in usecs */
#define DEFAULT_SAMPLING_RATE 30000
static unsigned int sampling_rate;

/* Consider IO as busy */
#define DEFAULT_IO_IS_BUSY 1
static unsigned int io_is_busy;

#define DEFAULT_IGNORE_NICE 1
static unsigned int ignore_nice;

/*************** End of tunables ***************/

static unsigned int dbs_enable; /* number of CPUs using this policy */

static void do_dbs_timer(struct work_struct *work);

struct smartmax_info_s {
	struct cpufreq_policy *cur_policy;
	struct cpufreq_frequency_table *freq_table;
	struct delayed_work work;
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_iowait;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	cputime64_t freq_change_time;
	unsigned int cur_cpu_load;
	unsigned int old_freq;
	int ramp_dir;
	unsigned int ideal_speed;
	unsigned int cpu;
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct smartmax_info_s, smartmax_info);

#define SMARTMAX_DEBUG 0

enum {
	SMARTMAX_DEBUG_JUMPS = 1,
	SMARTMAX_DEBUG_LOAD = 2,
	SMARTMAX_DEBUG_ALG = 4,
	SMARTMAX_DEBUG_BOOST = 8,
	SMARTMAX_DEBUG_INPUT = 16
};

/*
 * Combination of the above debug flags.
 */
#if SMARTMAX_DEBUG
static unsigned long debug_mask = SMARTMAX_DEBUG_LOAD|SMARTMAX_DEBUG_JUMPS|SMARTMAX_DEBUG_ALG|SMARTMAX_DEBUG_BOOST|SMARTMAX_DEBUG_INPUT;
#else
static unsigned long debug_mask;
#endif

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);
static unsigned int min_sampling_rate;
#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

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
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)

static int cpufreq_governor_smartmax(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SMARTASS2
static
#endif
struct cpufreq_governor cpufreq_gov_smartass2 = { .name = "smartmax", .governor =
		cpufreq_governor_smartmax, .max_transition_latency = TRANSITION_LATENCY_LIMIT,
		.owner = THIS_MODULE , };

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
		cputime64_t *wall) {
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
		*wall = (cputime64_t) jiffies_to_usecs(cur_wall_time);

	return (cputime64_t) jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall) {
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);

	return idle_time;
}

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu,
		cputime64_t *wall) {
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

inline static void smartmax_update_min_max(
		struct smartmax_info_s *this_smartmax, struct cpufreq_policy *policy) {
	this_smartmax->ideal_speed = // ideal_freq; but make sure it obeys the policy min/max
			policy->min < ideal_freq ?
					(ideal_freq < policy->max ? ideal_freq : policy->max) :
					policy->min;
}

inline static void smartmax_update_min_max_allcpus(void) {
	unsigned int i;

	// block hotplugging
	get_online_cpus();

	for_each_online_cpu(i)
	{
		struct smartmax_info_s *this_smartmax = &per_cpu(smartmax_info, i);
		if (this_smartmax->cur_policy)
			smartmax_update_min_max(this_smartmax, this_smartmax->cur_policy);
	}

	// resume hotplugging
	put_online_cpus();
}

inline static unsigned int validate_freq(struct cpufreq_policy *policy,
		int freq) {
	if (freq > (int) policy->max)
		return policy->max;
	if (freq < (int) policy->min)
		return policy->min;
	return freq;
}

/* We want all CPUs to do sampling nearly on same jiffy */
static inline unsigned int get_timer_delay(void) {
	unsigned int delay = usecs_to_jiffies(sampling_rate);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;
	return delay;
}

static inline void dbs_timer_init(struct smartmax_info_s *this_smartmax) {
	int delay = get_timer_delay();

	INIT_DELAYED_WORK_DEFERRABLE(&this_smartmax->work, do_dbs_timer);
	schedule_delayed_work_on(this_smartmax->cpu, &this_smartmax->work, delay);
}

static inline void dbs_timer_exit(struct smartmax_info_s *this_smartmax) {
	cancel_delayed_work_sync(&this_smartmax->work);
}

inline static void target_freq(struct cpufreq_policy *policy,
		struct smartmax_info_s *this_smartmax, int new_freq, int old_freq,
		int prefered_relation) {
	int index, target;
	struct cpufreq_frequency_table *table = this_smartmax->freq_table;
#if SMARTMAX_DEBUG
	unsigned int cpu = this_smartmax->cpu;
#endif

	if (new_freq == old_freq)
		return;
	new_freq = validate_freq(policy, new_freq);
	if (new_freq == old_freq)
		return;

	if (table && !cpufreq_frequency_table_target(policy, table, new_freq,
					prefered_relation, &index)) {
		target = table[index].frequency;
		if (target == old_freq) {
			// if for example we are ramping up to *at most* current + ramp_up_step
			// but there is no such frequency higher than the current, try also
			// to ramp up to *at least* current + ramp_up_step.
			if (new_freq > old_freq && prefered_relation == CPUFREQ_RELATION_H
					&& !cpufreq_frequency_table_target(policy, table, new_freq,
							CPUFREQ_RELATION_L, &index))
				target = table[index].frequency;
			// simlarly for ramping down:
			else if (new_freq < old_freq
					&& prefered_relation == CPUFREQ_RELATION_L
					&& !cpufreq_frequency_table_target(policy, table, new_freq,
							CPUFREQ_RELATION_H, &index))
				target = table[index].frequency;
		}

		if (target == old_freq) {
			// We should not get here:
			// If we got here we tried to change to a validated new_freq which is different
			// from old_freq, so there is no reason for us to remain at same frequency.

			return;
		}
	} else
		target = new_freq;

	/*mutex_lock(&set_speed_lock); bedalus

	// only if all cpus get the target they will really scale down
	// cause the highest defines the speed for all
	for_each_online_cpu(j)
	{
		struct smartmax_info_s *j_this_smartmax = &per_cpu(smartmax_info, j);

		if (j_this_smartmax->enable) {
			struct cpufreq_policy *j_policy = j_this_smartmax->cur_policy;

			__cpufreq_driver_target(j_policy, target, prefered_relation);
		}
	}
	mutex_unlock(&set_speed_lock);*/
		__cpufreq_driver_target(policy, target, prefered_relation);

	// remember last time we changed frequency
	this_smartmax->freq_change_time = ktime_to_us(ktime_get());
}

/* We use the same work function to sale up and down */
static void cpufreq_smartmax_freq_change(struct smartmax_info_s *this_smartmax) {
	unsigned int cpu;
	unsigned int new_freq = 0;
	unsigned int old_freq;
	int ramp_dir;
	struct cpufreq_policy *policy;
	unsigned int relation = CPUFREQ_RELATION_L;

	ramp_dir = this_smartmax->ramp_dir;
	old_freq = this_smartmax->old_freq;
	policy = this_smartmax->cur_policy;
	cpu = this_smartmax->cpu;

	if (old_freq != policy->cur) {
		// frequency was changed by someone else?
		new_freq = old_freq;
	} else if (ramp_dir > 0 && nr_running() > 1) {
		// ramp up logic:
		if (old_freq < this_smartmax->ideal_speed)
			new_freq = this_smartmax->ideal_speed;
		else if (ramp_up_step) {
			new_freq = old_freq + ramp_up_step;
			relation = CPUFREQ_RELATION_H;
			if (new_freq > DEFAULT_IDEAL_FREQ)
				new_freq = policy->max; // skip 1.4 and 1.5GHz as they are barely used.
		}
	} else if (ramp_dir < 0) {
		// ramp down logic:
		if (old_freq > this_smartmax->ideal_speed) {
			new_freq = this_smartmax->ideal_speed;
			relation = CPUFREQ_RELATION_H;
		} else if (ramp_down_step)
			new_freq = old_freq - ramp_down_step;
		else {
			// Load heuristics: Adjust new_freq such that, assuming a linear
			// scaling of load vs. frequency, the load in the new frequency
			// will be max_cpu_load:
			new_freq = old_freq * this_smartmax->cur_cpu_load / max_cpu_load;
			if (new_freq > old_freq) // min_cpu_load > max_cpu_load ?!
				new_freq = old_freq - 1;
		}
	}

	if ((new_freq < DEFAULT_IDEAL_FREQ) && (boost_counter > 0) && !early_suspend_hook)
		new_freq = DEFAULT_IDEAL_FREQ;

	if (new_freq!=0){
		target_freq(policy, this_smartmax, new_freq, old_freq, relation);
	}
	
	this_smartmax->ramp_dir = 0;
}

static inline void cpufreq_smartmax_get_ramp_direction(unsigned int debug_load, unsigned int cur, struct smartmax_info_s *this_smartmax, struct cpufreq_policy *policy, cputime64_t now)
{
	// Scale up if load is above max or if there where no idle cycles since coming out of idle,
	// additionally, if we are at or above the ideal_speed, verify we have been at this frequency
	// for at least up_rate:
	int min_load_adjust, max_load_adjust;

	if (early_suspend_hook) //set other defaults for governor while in early_suspend
	{
		max_load_adjust = 65;
		min_load_adjust = 15;
		ramp_down_step = 436000;
		up_rate = 30000;
	} else
	{
		max_load_adjust = max_cpu_load;
		min_load_adjust = min_cpu_load;
		ramp_down_step = DEFAULT_RAMP_DOWN_STEP;
		up_rate = DEFAULT_UP_RATE;
	}

	if (debug_load > max_load_adjust && cur < policy->max
			&& (cur < this_smartmax->ideal_speed
				|| (now - this_smartmax->freq_change_time) >= up_rate))
		this_smartmax->ramp_dir = 1;
	// Similarly for scale down: load should be below min and if we are at or below ideal
	// frequency we require that we have been at this frequency for at least down_rate:
	else if (debug_load < min_load_adjust && cur > policy->min
			&& (cur > this_smartmax->ideal_speed
				|| (now - this_smartmax->freq_change_time) >= down_rate))
		this_smartmax->ramp_dir = -1;
}

static void cpufreq_smartmax_timer(struct smartmax_info_s *this_smartmax) {
	unsigned int cur;
	struct cpufreq_policy *policy = this_smartmax->cur_policy;
	cputime64_t now = ktime_to_us(ktime_get());
	unsigned int max_load_freq;
	unsigned int debug_load = 0;
	unsigned int debug_iowait = 0;
	unsigned int j = 0;

	cur = policy->cur;

	/* Get Absolute Load - in terms of freq */
	max_load_freq = 0;

	for_each_cpu(j, policy->cpus)
	{
		struct smartmax_info_s *j_this_smartmax;
		cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;
		unsigned int load, load_freq;
		int freq_avg;

		j_this_smartmax = &per_cpu(smartmax_info, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);
		cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);

		wall_time = (unsigned int) cputime64_sub(cur_wall_time,
				j_this_smartmax->prev_cpu_wall);
		j_this_smartmax->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int) cputime64_sub(cur_idle_time,
				j_this_smartmax->prev_cpu_idle);
		j_this_smartmax->prev_cpu_idle = cur_idle_time;

		iowait_time = (unsigned int) cputime64_sub(cur_iowait_time,
				j_this_smartmax->prev_cpu_iowait);
		j_this_smartmax->prev_cpu_iowait = cur_iowait_time;

		if (ignore_nice) {
			cputime64_t cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = cputime64_sub(kstat_cpu(j).cpustat.nice, j_this_smartmax->prev_cpu_nice);
			cur_nice_jiffies = (unsigned long) cputime64_to_jiffies64(cur_nice);

			j_this_smartmax->prev_cpu_nice = kstat_cpu(j) .cpustat.nice;
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		/*
		 * For the purpose of ondemand, waiting for disk IO is an
		 * indication that you're performance critical, and not that
		 * the system is actually idle. So subtract the iowait time
		 * from the cpu idle time.
		 */
		if (io_is_busy && idle_time >= iowait_time)
			idle_time -= iowait_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;

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

	this_smartmax->cur_cpu_load = debug_load;
	this_smartmax->old_freq = cur;
	this_smartmax->ramp_dir = 0;

	if (early_suspend_hook) 
	{
		ideal_freq = 640000;
		boost_counter = 0;
	}
	else
		ideal_freq = DEFAULT_IDEAL_FREQ;

	if (unlikely(boost_counter > 0))
		if (++boost_counter > 2)
			boost_counter = 0;

	cpufreq_smartmax_get_ramp_direction(debug_load, cur, this_smartmax, policy, now);
	// no changes
	if (this_smartmax->ramp_dir == 0)		
		return;
	cpufreq_smartmax_freq_change(this_smartmax);
}

static void do_dbs_timer(struct work_struct *work) {
	struct smartmax_info_s *this_smartmax =
			container_of(work, struct smartmax_info_s, work.work);
	unsigned int cpu = this_smartmax->cpu;
	int delay = get_timer_delay();

	mutex_lock(&this_smartmax->timer_mutex);

	cpufreq_smartmax_timer(this_smartmax);

	schedule_delayed_work_on(cpu, &this_smartmax->work, delay);
	mutex_unlock(&this_smartmax->timer_mutex);
}

static void update_idle_time(bool online) {
	int j = 0;

	for_each_possible_cpu(j)
	{
		struct smartmax_info_s *j_this_smartmax;

		if (online && !cpu_online(j)) {
			continue;
		}
		j_this_smartmax = &per_cpu(smartmax_info, j);

		j_this_smartmax->prev_cpu_idle = get_cpu_idle_time(j,
				&j_this_smartmax->prev_cpu_wall);
		if (ignore_nice)
			j_this_smartmax->prev_cpu_nice = kstat_cpu(j) .cpustat.nice;

	}
}

static ssize_t show_debug_mask(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%lu\n", debug_mask);
}

static ssize_t store_debug_mask(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0)
		debug_mask = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_up_rate(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", up_rate);
}

static ssize_t store_up_rate(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0 && input <= 100000000)
		up_rate = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_down_rate(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", down_rate);
}

static ssize_t store_down_rate(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0 && input <= 100000000)
		down_rate = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_ideal_freq(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", ideal_freq);
}

static ssize_t store_ideal_freq(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0) {
		ideal_freq = input;
		smartmax_update_min_max_allcpus();
	} else
		return -EINVAL;
	return count;
}

static ssize_t show_ramp_up_step(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", ramp_up_step);
}

static ssize_t store_ramp_up_step(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0)
		ramp_up_step = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_ramp_down_step(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", ramp_down_step);
}

static ssize_t store_ramp_down_step(struct kobject *kobj,
		struct attribute *attr, const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0)
		ramp_down_step = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_max_cpu_load(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", max_cpu_load);
}

static ssize_t store_max_cpu_load(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input > 0 && input <= 100)
		max_cpu_load = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_min_cpu_load(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", min_cpu_load);
}

static ssize_t store_min_cpu_load(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input > 0 && input < 100)
		min_cpu_load = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_sampling_rate(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%u\n", sampling_rate);
}

static ssize_t store_sampling_rate(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= min_sampling_rate)
		sampling_rate = input;
	else
		return -EINVAL;
	return count;
}

static ssize_t show_io_is_busy(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%d\n", io_is_busy);
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;

	res = strict_strtoul(buf, 0, &input);
	if (res >= 0) {
		if (input > 1)
			input = 1;
		if (input == io_is_busy) { /* nothing to do */
			return count;
		}
		io_is_busy = input;
	} else
		return -EINVAL;	
	return count;
}

static ssize_t show_ignore_nice(struct kobject *kobj, struct attribute *attr,
		char *buf) {
	return sprintf(buf, "%d\n", ignore_nice);
}

static ssize_t store_ignore_nice(struct kobject *a, struct attribute *b,
		const char *buf, size_t count) {
	ssize_t res;
	unsigned long input;

	res = strict_strtoul(buf, 0, &input);
	if (res >= 0) {
		if (input > 1)
			input = 1;
		if (input == ignore_nice) { /* nothing to do */
			return count;
		}
		ignore_nice = input;
		/* we need to re-evaluate prev_cpu_idle */
		update_idle_time(true);
	} else
		return -EINVAL;	
	return count;
}

#define define_global_rw_attr(_name)		\
static struct global_attr _name##_attr =	\
	__ATTR(_name, 0644, show_##_name, store_##_name)

define_global_rw_attr(debug_mask);
define_global_rw_attr(up_rate);
define_global_rw_attr(down_rate);
define_global_rw_attr(ideal_freq);
define_global_rw_attr(ramp_up_step);
define_global_rw_attr(ramp_down_step);
define_global_rw_attr(max_cpu_load);
define_global_rw_attr(min_cpu_load);
define_global_rw_attr(sampling_rate);
define_global_rw_attr(io_is_busy);
define_global_rw_attr(ignore_nice);

static struct attribute * smartmax_attributes[] = { &debug_mask_attr.attr,
		&up_rate_attr.attr, &down_rate_attr.attr, &ideal_freq_attr.attr,
		&ramp_up_step_attr.attr, &ramp_down_step_attr.attr,
		&max_cpu_load_attr.attr, &min_cpu_load_attr.attr,
		&sampling_rate_attr.attr,
	    	&io_is_busy_attr.attr,
		&ignore_nice_attr.attr, NULL , };

static struct attribute_group smartmax_attr_group = { .attrs =
		smartmax_attributes, .name = "smartmax", };

static void dbs_input_event(struct input_handle *handle, unsigned int type,
		unsigned int code, int value) {
		if (type == EV_SYN && code == SYN_REPORT && !early_suspend_hook)
			boost_counter = 1;
}

static int input_dev_filter(const char* input_dev_name) {
	int ret = 0;
	if (strstr(input_dev_name, "touchscreen")
			|| strstr(input_dev_name, "-keypad")
			|| strstr(input_dev_name, "-nav")
			|| strstr(input_dev_name, "-oj")) {
	} else {
		ret = 1;
	}
	return ret;
}

static int dbs_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id) {
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
	err1: input_unregister_handle(handle);
	err2: kfree(handle);
	return error;
}

static void dbs_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id dbs_ids[] = { { .driver_info = 1 }, { }, };

static struct input_handler dbs_input_handler = { .event = dbs_input_event,
		.connect = dbs_input_connect, .disconnect = dbs_input_disconnect,
		.name = "cpufreq_smartmax", .id_table = dbs_ids, };

static int cpufreq_governor_smartmax(struct cpufreq_policy *new_policy,
		unsigned int event) {
	unsigned int cpu = new_policy->cpu;
	int rc;
	struct smartmax_info_s *this_smartmax = &per_cpu(smartmax_info, cpu);
    unsigned int latency;

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!new_policy->cur))return -EINVAL;

		mutex_lock(&dbs_mutex);

		this_smartmax->cur_policy = new_policy;
		this_smartmax->cpu = cpu;

		smartmax_update_min_max(this_smartmax,new_policy);

		this_smartmax->freq_table = cpufreq_frequency_get_table(cpu);

		update_idle_time(false);

		dbs_enable++;
		
		if (dbs_enable == 1) {
			rc = input_register_handler(&dbs_input_handler);
			if (rc) {
				dbs_enable--;
				mutex_unlock(&dbs_mutex);
				return rc;
			}
			rc = sysfs_create_group(cpufreq_global_kobject,
					&smartmax_attr_group);
			if (rc) {
				dbs_enable--;
				mutex_unlock(&dbs_mutex);
				return rc;
			}
			/* policy latency is in nS. Convert it to uS first */
			latency = new_policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;

			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate, MIN_LATENCY_MULTIPLIER * latency);
			sampling_rate = max(min_sampling_rate, sampling_rate);
		}

		mutex_unlock(&dbs_mutex);
		dbs_timer_init(this_smartmax);

		break;
	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_smartmax->timer_mutex);
		smartmax_update_min_max(this_smartmax,new_policy);

		if (this_smartmax->cur_policy->cur > new_policy->max) {
			__cpufreq_driver_target(this_smartmax->cur_policy,
					new_policy->max, CPUFREQ_RELATION_H);
		}
		else if (this_smartmax->cur_policy->cur < new_policy->min) {
			__cpufreq_driver_target(this_smartmax->cur_policy,
					new_policy->min, CPUFREQ_RELATION_L);
		}
		mutex_unlock(&this_smartmax->timer_mutex);
		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_smartmax);

		mutex_lock(&dbs_mutex);
		this_smartmax->cur_policy = NULL;
		dbs_enable--;

		if (!dbs_enable){
			sysfs_remove_group(cpufreq_global_kobject, &smartmax_attr_group);
			input_unregister_handler(&dbs_input_handler);
		}
		mutex_unlock(&dbs_mutex);
		break;
	}

	return 0;
}

static int __init cpufreq_smartmax_init(void) {
	unsigned int i;
	struct smartmax_info_s *this_smartmax;
	u64 wall;
	u64 idle_time;
	int cpu = get_cpu();

	idle_time = get_cpu_idle_time_us(cpu, &wall);
	put_cpu();
	if (idle_time != -1ULL) {
		/*
		 * In no_hz/micro accounting case we set the minimum frequency
		 * not depending on HZ, but fixed (very low). The deferred
		 * timer might skip some samples if idle/sleeping as needed.
		*/
		min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
	} else {
		/* For correct statistics, we need 10 ticks for each measure */
		min_sampling_rate = MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10);
	}

	up_rate = DEFAULT_UP_RATE;
	down_rate = DEFAULT_DOWN_RATE;
	ideal_freq = DEFAULT_IDEAL_FREQ;
	ramp_up_step = DEFAULT_RAMP_UP_STEP;
	ramp_down_step = DEFAULT_RAMP_DOWN_STEP;
	max_cpu_load = DEFAULT_MAX_CPU_LOAD;
	min_cpu_load = DEFAULT_MIN_CPU_LOAD;
	sampling_rate = DEFAULT_SAMPLING_RATE;
	io_is_busy = DEFAULT_IO_IS_BUSY;
	ignore_nice = DEFAULT_IGNORE_NICE;

	/* Initalize per-cpu data: */for_each_possible_cpu(i)
	{
		this_smartmax = &per_cpu(smartmax_info, i);
		this_smartmax->cur_policy = NULL;
		this_smartmax->ramp_dir = 0;
		this_smartmax->freq_change_time = 0;
		this_smartmax->cur_cpu_load = 0;
		mutex_init(&this_smartmax->timer_mutex);
	}

	return cpufreq_register_governor(&cpufreq_gov_smartass2);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SMARTASS2
fs_initcall(cpufreq_smartmax_init);
#else
module_init(cpufreq_smartmax_init);
#endif

static void __exit cpufreq_smartmax_exit(void) {
	unsigned int i;
	struct smartmax_info_s *this_smartmax;

	cpufreq_unregister_governor(&cpufreq_gov_smartass2);

	for_each_possible_cpu(i)
	{
		this_smartmax = &per_cpu(smartmax_info, i);
		mutex_destroy(&this_smartmax->timer_mutex);
	}
}

module_exit(cpufreq_smartmax_exit);

MODULE_AUTHOR("maxwen");
MODULE_DESCRIPTION("'cpufreq_smartmax' - A smart cpufreq governor");
MODULE_LICENSE("GPL");
