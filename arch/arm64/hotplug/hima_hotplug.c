/*
 * Intelli Hotplug Driver
 *
 * Copyright (c) 2015, Chad Cormier Roussel <chadcormierroussel@gmail.com>
 * Copyright (c) 2013-2014, Paul Reioux <reioux@gmail.com>
 * Copyright (c) 2010-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/kobject.h>

#ifdef CONFIG_STATE_NOTIFIER
#include <linux/state_notifier.h>
#endif

#include <linux/cpufreq.h>

#define HIMA_HOTPLUG		       "hima_hotplug"
#define HIMA_HOTPLUG_MAJOR_VERSION     	7
#define HIMA_HOTPLUG_MINOR_VERSION     	0

#define DEF_SAMPLING_MS                	300
#define RESUME_SAMPLING_MS             	100
#define START_DELAY_MS                 	10000

#define DEFAULT_MIN_CPUS_ONLINE        	4
#define DEFAULT_MAX_CPUS_ONLINE        	8
#define DEFAULT_MIN_UP_TIME            	2000

#define DEFAULT_NR_FSHIFT              	4

/* Tuned for MSM8994 */
#define THREAD_CAPACITY			350
#define CPU_NR_THRESHOLD		((THREAD_CAPACITY << 1) - (THREAD_CAPACITY >> 1))

static struct delayed_work hima_hotplug_work;
static struct work_struct up_down_work;
static struct workqueue_struct *hima_hotplug_wq;
#ifdef CONFIG_STATE_NOTIFIER
static struct notifier_block notif;
#endif

struct ip_cpu_info {
	unsigned long cpu_nr_running;
	unsigned long cpu_up_time;
};
static DEFINE_PER_CPU(struct ip_cpu_info, ip_info);

/* Driver Controls */
static atomic_t hima_hotplug_active = ATOMIC_INIT(1);
static unsigned int min_cpus_online = DEFAULT_MIN_CPUS_ONLINE;
static unsigned int max_cpus_online = DEFAULT_MAX_CPUS_ONLINE;
static unsigned int min_cpu_up_time = DEFAULT_MIN_UP_TIME;

/* Driver Tuning */
static unsigned int def_sampling_ms = DEF_SAMPLING_MS;
static unsigned int nr_fshift = DEFAULT_NR_FSHIFT;

/* Profile Controls*/
static unsigned int current_profile_no = 0;
static unsigned int cpu_nr_run_threshold = CPU_NR_THRESHOLD;

/* Profile Tuning */
static unsigned int nr_run_thresholds_balanced[] = {
	12, 35, 53, 71, UINT_MAX
};

static unsigned int nr_run_thresholds_disable[] = {
	0, 0, 0, 0, 0, 0, 0, 0, UINT_MAX
};

static unsigned int *nr_run_profiles[] = {
	nr_run_thresholds_balanced,
	nr_run_thresholds_disable
};

static unsigned int calculate_thread_stats(void)
{
	unsigned int avg_nr_run = avg_nr_running();
	unsigned int nr_run;
	unsigned int threshold_size;
	unsigned int *current_profile = nr_run_profiles[current_profile_no];

	threshold_size = ARRAY_SIZE(nr_run_thresholds_balanced);

	for (nr_run = min_cpus_online; (nr_run < threshold_size) - min_cpus_online; nr_run++) {
 		unsigned int nr_threshold;
		nr_threshold = current_profile[nr_run - min_cpus_online];

		if (avg_nr_run <= (nr_threshold << (FSHIFT - nr_fshift)) || nr_run >= max_cpus_online)
			break;
	}

	return nr_run;
}

static void update_per_cpu_stat(void)
{
	unsigned int cpu;
	struct ip_cpu_info *l_ip_info;

	for_each_online_cpu(cpu) {
		l_ip_info = &per_cpu(ip_info, cpu);
		l_ip_info->cpu_nr_running = avg_cpu_nr_running(cpu);
	}

	for_each_cpu_not(cpu, cpu_online_mask) {
		l_ip_info = &per_cpu(ip_info, cpu);
                l_ip_info->cpu_up_time = 0;
	}
}

static void __ref cpu_up_down_work(struct work_struct *work)
{
	int online_cpus = 0, cpu = 0, l_nr_threshold = 0;
	int target = 1;
	struct ip_cpu_info *l_ip_info;

	target = calculate_thread_stats();

	online_cpus = num_online_cpus();

	/* Break early if we are on target */
	if(target == online_cpus)
		return;
	else if (target < online_cpus) {
		update_per_cpu_stat();
		for_each_online_cpu(cpu) {
			l_ip_info = &per_cpu(ip_info, cpu);

			if (cpu <= 3 ||
				((ktime_to_ms(ktime_get()) - l_ip_info->cpu_up_time) < min_cpu_up_time))
				continue;
			l_nr_threshold = cpu_nr_run_threshold << 1 / (num_online_cpus());
			if (l_ip_info->cpu_nr_running < l_nr_threshold)
				cpu_down(cpu);

			if (num_online_cpus() <= target)
				break;
		}
	} else {
		update_per_cpu_stat();
		for_each_cpu_not(cpu, cpu_online_mask) {
			if(cpu <= 3)
				continue;
			cpu_up(cpu);
			l_ip_info = &per_cpu(ip_info, cpu);
			l_ip_info->cpu_up_time = ktime_to_ms(ktime_get());
			if (num_online_cpus() >= target)
				break;
		}
	}
}

static void hima_hotplug_work_fn(struct work_struct *work)
{
	queue_work_on(0, hima_hotplug_wq, &up_down_work);

	if (atomic_read(&hima_hotplug_active) == 1)
		queue_delayed_work_on(0, hima_hotplug_wq, &hima_hotplug_work,
					msecs_to_jiffies(def_sampling_ms));
}


#ifdef CONFIG_STATE_NOTIFIER
static void __ref hima_hotplug_suspend(void)
{
}

static void __ref hima_hotplug_resume(void)
{
	int cpu = 0;

	/* Bring all cores on for fast resume */
	for_each_cpu_not(cpu, cpu_online_mask)
		cpu_up(cpu);
}

static int state_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	if (atomic_read(&hima_hotplug_active) == 0)
		return NOTIFY_OK;

	switch (event) {
		case STATE_NOTIFIER_ACTIVE:
			hima_hotplug_resume();
			break;
		case STATE_NOTIFIER_SUSPEND:
			hima_hotplug_suspend();
			break;
		default:
			break;
	}

	return NOTIFY_OK;
}
#endif

static int __ref hima_hotplug_start(void)
{
	int cpu, ret = 0;

	hima_hotplug_wq = alloc_workqueue("hima_hotplug", WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!hima_hotplug_wq) {
		pr_err("%s: Failed to allocate hotplug workqueue\n",
		       HIMA_HOTPLUG);
		ret = -ENOMEM;
		goto err_out;
	}

#ifdef CONFIG_STATE_NOTIFIER
	notif.notifier_call = state_notifier_callback;
	if (state_register_client(&notif)) {
		pr_err("%s: Failed to register State notifier callback\n",
			HIMA_HOTPLUG);
		goto err_dev;
	}
#endif

	INIT_WORK(&up_down_work, cpu_up_down_work);
	INIT_DELAYED_WORK(&hima_hotplug_work, hima_hotplug_work_fn);

	/* Fire up all CPUs */
	for_each_cpu_not(cpu, cpu_online_mask) {
		cpu_up(cpu);
	}

	queue_delayed_work_on(0, hima_hotplug_wq, &hima_hotplug_work,
			      START_DELAY_MS);

	return ret;

#ifdef CONFIG_STATE_NOTIFIER
err_dev:
	destroy_workqueue(hima_hotplug_wq);
#endif
err_out:
	atomic_set(&hima_hotplug_active, 0);
	return ret;
}

static void hima_hotplug_stop(void)
{
	flush_workqueue(hima_hotplug_wq);
	cancel_work_sync(&up_down_work);
	cancel_delayed_work_sync(&hima_hotplug_work);
#ifdef CONFIG_STATE_NOTIFIER
	state_unregister_client(&notif);
#endif
	destroy_workqueue(hima_hotplug_wq);
}

static void hima_hotplug_active_eval_fn(unsigned int status)
{
	int ret = 0;

	if (status == 1) {
		ret = hima_hotplug_start();
		if (ret)
			status = 0;
	} else
		hima_hotplug_stop();

	atomic_set(&hima_hotplug_active, status);
}

#define show_one(file_name, object)				\
static ssize_t show_##file_name					\
(struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
{								\
	return sprintf(buf, "%u\n", object);			\
}

show_one(min_cpus_online, min_cpus_online);
show_one(max_cpus_online, max_cpus_online);
show_one(current_profile_no, current_profile_no);
show_one(cpu_nr_run_threshold, cpu_nr_run_threshold);
show_one(def_sampling_ms, def_sampling_ms);
show_one(nr_fshift, nr_fshift);

#define store_one(file_name, object)		\
static ssize_t store_##file_name		\
(struct kobject *kobj, 				\
 struct kobj_attribute *attr, 			\
 const char *buf, size_t count)			\
{						\
	unsigned int input;			\
	int ret;				\
	ret = sscanf(buf, "%u", &input);	\
	if (ret != 1 || input > 100)		\
		return -EINVAL;			\
	if (input == object) {			\
		return count;			\
	}					\
	object = input;				\
	return count;				\
}

store_one(current_profile_no, current_profile_no);
store_one(cpu_nr_run_threshold, cpu_nr_run_threshold);
store_one(def_sampling_ms, def_sampling_ms);
store_one(nr_fshift, nr_fshift);

static ssize_t show_hima_hotplug_active(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n",
			atomic_read(&hima_hotplug_active));
}

static ssize_t store_hima_hotplug_active(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int ret;
	unsigned int input;

	ret = sscanf(buf, "%d", &input);
	if (ret < 0)
		return ret;

	if (input < 0)
		input = 0;
	else if (input > 0)
		input = 1;

	if (input == atomic_read(&hima_hotplug_active))
		return count;

	hima_hotplug_active_eval_fn(input);

	return count;
}

static ssize_t store_min_cpus_online(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > NR_CPUS)
		return -EINVAL;

	if (max_cpus_online < val)
		max_cpus_online = val;

	min_cpus_online = val;

	return count;
}

static ssize_t store_max_cpus_online(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > NR_CPUS)
		return -EINVAL;

	if (min_cpus_online > val)
		min_cpus_online = val;

	max_cpus_online = val;

	return count;
}

#define KERNEL_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = \
	__ATTR(_name, 0664, show_##_name, store_##_name)

KERNEL_ATTR_RW(hima_hotplug_active);
KERNEL_ATTR_RW(min_cpus_online);
KERNEL_ATTR_RW(max_cpus_online);
KERNEL_ATTR_RW(current_profile_no);
KERNEL_ATTR_RW(cpu_nr_run_threshold);
KERNEL_ATTR_RW(def_sampling_ms);
KERNEL_ATTR_RW(nr_fshift);

static struct attribute *hima_hotplug_attrs[] = {
	&hima_hotplug_active_attr.attr,
	&min_cpus_online_attr.attr,
	&max_cpus_online_attr.attr,
	&current_profile_no_attr.attr,
	&cpu_nr_run_threshold_attr.attr,
	&def_sampling_ms_attr.attr,
	&nr_fshift_attr.attr,
	NULL,
};

static struct attribute_group hima_hotplug_attr_group = {
	.attrs = hima_hotplug_attrs,
	.name = "hima_hotplug",
};

static int __init hima_hotplug_init(void)
{
	int rc;

	rc = sysfs_create_group(kernel_kobj, &hima_hotplug_attr_group);

	pr_info("HIMA_HOTPLUG: version %d.%d\n",
		 HIMA_HOTPLUG_MAJOR_VERSION,
		 HIMA_HOTPLUG_MINOR_VERSION);

	if (atomic_read(&hima_hotplug_active) == 1)
		hima_hotplug_start();

	return 0;
}

static void __exit hima_hotplug_exit(void)
{

	if (atomic_read(&hima_hotplug_active) == 1)
		hima_hotplug_stop();
	sysfs_remove_group(kernel_kobj, &hima_hotplug_attr_group);
}

arch_initcall(hima_hotplug_init);
module_exit(hima_hotplug_exit);

MODULE_AUTHOR("Chad Cormier Roussel <chadcormierroussel@gmail.com>");
MODULE_DESCRIPTION("An intelligent cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPLv2");
