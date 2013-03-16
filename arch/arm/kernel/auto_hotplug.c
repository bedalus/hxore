/* Copyright (c) 2012, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 *
 * Based on Will Tilsdale's code. See above.
 * Badly modified by bedalus, whilst drunk and asleep. DO NOT TRUST MY CODE. 
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/delay.h>

#include <linux/io.h>
#include <linux/clk.h>
#include "../mach-tegra/pm.h"
#include "../mach-tegra/sleep.h"
#include "../mach-tegra/cpu-tegra.h"
#include "../mach-tegra/clock.h"

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define MIN_SAMPLING_RATE	msecs_to_jiffies(5000)

/* Control flags */
unsigned char flags;
#define HOTPLUG_PAUSED		(1 << 1)
#define EARLYSUSPEND_ACTIVE	(1 << 3)

static DEFINE_MUTEX(tegra3_cpu_lock);

struct delayed_work hotplug_decision_work;
struct delayed_work hotplug_unpause_work;
struct work_struct hotplug_online_single_work;
struct work_struct hotplug_offline_single_work;
struct work_struct hotplug_offline_all_work;

static int cpusallowed = 2;

static ssize_t cpusallowed_status_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf,"%u\n", cpusallowed);
}

static ssize_t cpusallowed_status_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1)
		cpusallowed = data;
	else
		pr_info("%s: input error\n", __FUNCTION__);

	return size;
}

static DEVICE_ATTR(cpusallowed, S_IRUGO | S_IWUGO, cpusallowed_status_read, cpusallowed_status_write);

static struct attribute *cpusallowed_attributes[] = {
	&dev_attr_cpusallowed.attr,
	NULL
};

static struct attribute_group cpusallowed_group = {
	.attrs  = cpusallowed_attributes,
};

static struct miscdevice cpusallowed_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cpusallowed",
};

static void hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int online_cpus, available_cpus;

	online_cpus = num_online_cpus();
	available_cpus = cpusallowed;
	if (available_cpus > 4) available_cpus = 4;
	if (available_cpus < 1) available_cpus = 1;

	if (flags & HOTPLUG_PAUSED) {
		schedule_delayed_work_on(0, &hotplug_decision_work, MIN_SAMPLING_RATE);
		return;
	} else if (flags & EARLYSUSPEND_ACTIVE) {
		schedule_work_on(0, &hotplug_offline_all_work);
		return;
	} else if ((online_cpus < available_cpus)) {
		schedule_work(&hotplug_online_single_work);
		return;
	} else if ((online_cpus > available_cpus)) {
		schedule_work(&hotplug_offline_single_work);
		return;
	} 
	schedule_delayed_work_on(0, &hotplug_decision_work, MIN_SAMPLING_RATE);
}

static void hotplug_offline_all_work_fn(struct work_struct *work)
{
	int cpu;

	mutex_lock(&tegra3_cpu_lock);
	pr_info("auto_hotplug: Attempt to enter cpu_lp");
	for_each_online_cpu(cpu) {
		if(cpu==0)
			continue;
		cpu_down(cpu);
	}
	if(!clk_set_parent(clk_get_sys(NULL, "cpu"), clk_get_sys(NULL, "cpu_lp")))
		tegra_cpu_set_speed_cap(NULL);
	mutex_unlock(&tegra3_cpu_lock);

	schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(200));		
}

static void hotplug_offline_single_work_fn(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if ((cpu) && (cpu_online(cpu))) {
			cpu_down(cpu);
			pr_info("auto_hotplug: CPU%d down.\n", cpu);
			break;
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, MIN_SAMPLING_RATE);
}

static void hotplug_online_single_work_fn(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if ((cpu) && (!cpu_online(cpu))) {
			cpu_up(cpu);
			pr_info("auto_hotplug: CPU%d up.\n", cpu);
			break;
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, MIN_SAMPLING_RATE);
}

static void hotplug_unpause_work_fn(struct work_struct *work)
{
	pr_info("auto_hotplug: Clearing pause flag\n");
	flags &= ~HOTPLUG_PAUSED;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void auto_hotplug_early_suspend(struct early_suspend *handler)
{
	pr_info("auto_hotplug: early suspend handler\n");
	flags |= EARLYSUSPEND_ACTIVE;

	/* Cancel all scheduled delayed work to avoid races */
	cancel_delayed_work_sync(&hotplug_decision_work);
	if (num_online_cpus() > 0) {
		pr_info("auto_hotplug: Offlining CPUs for early suspend\n");
		schedule_work_on(0, &hotplug_offline_all_work);
	}
}

static void auto_hotplug_late_resume(struct early_suspend *handler)
{
	pr_info("auto_hotplug: late resume handler\n");
	flags &= ~EARLYSUSPEND_ACTIVE;
	mutex_lock(&tegra3_cpu_lock);
	if (is_lp_cluster()) {
		if (!clk_set_parent(clk_get_sys(NULL, "cpu"), clk_get_sys(NULL, "cpu_g")))
			tegra_cpu_set_speed_cap(NULL);
	}
	mutex_unlock(&tegra3_cpu_lock);
	schedule_delayed_work_on(0, &hotplug_decision_work, 0);
}

static struct early_suspend auto_hotplug_suspend = {
	.suspend = auto_hotplug_early_suspend,
	.resume = auto_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int __init auto_hotplug_init(void)
{
	int ret;

	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_unpause_work, hotplug_unpause_work_fn);
	INIT_WORK(&hotplug_online_single_work, hotplug_online_single_work_fn);
	INIT_WORK(&hotplug_offline_single_work, hotplug_offline_single_work_fn);
	INIT_WORK(&hotplug_offline_all_work, hotplug_offline_all_work_fn);

	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */
	flags |= HOTPLUG_PAUSED;
	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 10);
	schedule_delayed_work(&hotplug_unpause_work, HZ * 30);

	// sysfs interface
	pr_info("%s misc_register(%s)\n", __FUNCTION__, cpusallowed_device.name);
	ret = misc_register(&cpusallowed_device);
	if (ret) {
		pr_err("%s misc_register(%s) fail\n", __FUNCTION__,
				cpusallowed_device.name);
		return 1;
	}
	if (sysfs_create_group(&cpusallowed_device.this_device->kobj,
				&cpusallowed_group) < 0) {
		pr_err("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n",
				cpusallowed_device.name);
	}


#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&auto_hotplug_suspend);
#endif
	return 0;
}
late_initcall(auto_hotplug_init);
