/* Copyright (c) 2010-2013, The Linux Foundation. All rights reserved.
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
 */
/*
 * Qualcomm MSM Runqueue Stats and cpu utilization Interface for Userspace
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/cpu.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/cpufreq.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <asm/smp_plat.h>
#include "../mach-msm/acpuclock.h"
#include <linux/suspend.h>
#include <linux/hotplug.h>

struct cpu_load_data {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_iowait;
	unsigned int avg_load_maxfreq;
	unsigned int samples;
	unsigned int window_size;
	unsigned int cur_freq;
	unsigned int policy_max;
	cpumask_var_t related_cpus;
	struct mutex cpu_load_mutex;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
              cputime64_t *wall)
{
  	u64 idle_time;
  	u64 cur_wall_time;
  	u64 busy_time;

  	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

  	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
  	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
  	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
  	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
  	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
  	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

  	idle_time = cur_wall_time - busy_time;
  	
  	if (wall)
    	*wall = jiffies_to_usecs(cur_wall_time);

  	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu, cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu,
              cputime64_t *wall)
{
  	u64 idle_time = get_cpu_idle_time_us(cpu, wall) - 
  						get_cpu_iowait_time(cpu, wall);

  	if (idle_time == -1ULL)
    	idle_time = get_cpu_idle_time_jiffy(cpu, wall);

  	return idle_time;
}

static int update_average_load(unsigned int freq, unsigned int cpu)
{

	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
	cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
	unsigned int idle_time, wall_time, iowait_time;
	unsigned int cur_load, load_at_max_freq;

	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time);
	cur_iowait_time = get_cpu_iowait_time(cpu, &cur_wall_time);

	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;

	iowait_time = (unsigned int) (cur_iowait_time - pcpu->prev_cpu_iowait);
	pcpu->prev_cpu_iowait = cur_iowait_time;

	if (idle_time >= iowait_time)
		idle_time -= iowait_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	cur_load = 100 * (wall_time - idle_time) / wall_time;

	/* Calculate the scaled load across CPU */
	load_at_max_freq = (cur_load * freq) / pcpu->policy_max;

	if (!pcpu->avg_load_maxfreq) {
		/* This is the first sample in this window*/
		pcpu->avg_load_maxfreq = load_at_max_freq;
		pcpu->window_size = wall_time;
	} else {
		/*
		 * The is already a sample available in this window.
		 * Compute weighted average with prev entry, so that we get
		 * the precise weighted load.
		 */
		pcpu->avg_load_maxfreq =
			((pcpu->avg_load_maxfreq * pcpu->window_size) +
			(load_at_max_freq * wall_time)) /
			(wall_time + pcpu->window_size);

		pcpu->window_size += wall_time;
	}

	return 0;
}

unsigned int report_load_at_max_freq()
{
	int cpu = 0;
	struct cpu_load_data *pcpu;
	unsigned int total_load = 0;

	pcpu = &per_cpu(cpuload, cpu);
	update_average_load(acpuclk_get_rate(cpu), cpu);
	total_load = pcpu->avg_load_maxfreq;
	pcpu->avg_load_maxfreq = 0;

	return total_load;
}

static int __init msm_rq_stats_init(void)
{
	int cpu = 0;
	struct cpufreq_policy cpu_policy;
	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);

	cpufreq_get_policy(&cpu_policy, cpu);

	pcpu->policy_max = cpu_policy.cpuinfo.max_freq;
	pcpu->cur_freq = acpuclk_get_rate(cpu);

	cpumask_copy(pcpu->related_cpus, cpu_policy.cpus);

	return 0;
}
late_initcall(msm_rq_stats_init);