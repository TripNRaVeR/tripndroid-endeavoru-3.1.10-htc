/*
 * drivers/tripndroid/cpu_management/tdf_hotplug.c
 *
 * TripNDroid CPU auto-hotplug for Tegra 3
 *
 * Copyright (c) 2013, TripNDroid Mobile Engineering
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/earlysuspend.h>
#include <linux/io.h>
#include <linux/td_framework.h>
#include <linux/tdf_hotplug.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

enum {
	TRIPNDROID_HP_DISABLED = 0,
	TRIPNDROID_HP_IDLE,
	TRIPNDROID_HP_DOWN,
	TRIPNDROID_HP_UP,
};

struct tripndroid_hp_cpudata_t {
	int online;
	cputime64_t on_time;
};
static DEFINE_PER_CPU(struct tripndroid_hp_cpudata_t, tripndroid_hp_cpudata);

static DEFINE_MUTEX(tripndroid_hp_cpu_lock);

struct delayed_work tripndroid_hp_w;

static struct tripndroid_hp {
	unsigned int sample_ms;
	unsigned int pause;
        unsigned int max_cpus;
        unsigned int min_cpus;
} tripndroid_hp_config = {
	.sample_ms = TRIPNDROID_HP_SAMPLE_MS,
	.pause = TRIPNDROID_HP_PAUSE,
        .max_cpus = CONFIG_NR_CPUS,
        .min_cpus = 1,
};

static struct clk *cpu_clk;

unsigned int state = TRIPNDROID_HP_IDLE;

extern unsigned int powersaving_active;
extern unsigned int tdf_suspend_state;
extern unsigned int nr_run_hysteresis;
extern unsigned int tdf_cpu_load;

bool was_paused = false;

static unsigned int nr_run_last;

static unsigned int normal_thresholds[] = {
/* 	1,  2,  3,  4 - on-line cpus target */
	7,  9,  10, UINT_MAX
};

static unsigned int powersaving_thresholds[] = {
/*      1,  2, - on-line cpus target */
        5,  UINT_MAX
};

static unsigned int NwNs_Threshold[8] = {12, 0, 20, 7, 25, 10, 0, 18};
static unsigned int TwTs_Threshold[8] = {140, 0, 140, 190, 140, 190, 0, 190};

/* get cpu speed */
unsigned int cpu_getspeed(unsigned int cpu)
{
	unsigned long rate;

	if (cpu >= tripndroid_hp_config.max_cpus || !ACCESS_ONCE(cpu_clk))
		return 0;

	rate = clk_get_rate(cpu_clk) / 1000;
	return rate;
}

/* get the nr of the cpu with the lowest speed */
static int get_slowest_cpu(void)
{
        int i, cpu = 0;
        unsigned long rate, slow_rate = 0;

        for (i = 0; i < tripndroid_hp_config.max_cpus; i++) {

                if (!cpu_online(i))
			continue;

		rate = cpu_getspeed(i);

		if (slow_rate == 0) {
			slow_rate = rate;
		}
		if ((rate <= slow_rate) && (slow_rate != 0)) {
			if (i == 0)
				continue;

			cpu = i;
			slow_rate = rate;
		}
        }
        return cpu;
}

static unsigned int calculate_load(void)
{
	unsigned int avg_nr_run = avg_nr_running();
	unsigned int nr_run, nr_fshift;
	unsigned int select_threshold;

	if (!powersaving_active) {
		nr_fshift = 3;
		select_threshold =  ARRAY_SIZE(normal_thresholds);
	}
	else {
		nr_fshift = 1;
		select_threshold =  ARRAY_SIZE(powersaving_thresholds);
	}

	for (nr_run = 1; nr_run < select_threshold; nr_run++) {
		unsigned int nr_threshold;
		if (!powersaving_active)
			nr_threshold = normal_thresholds[nr_run - 1];
		else
			nr_threshold = powersaving_thresholds[nr_run - 1];

		if (nr_run_last <= nr_run)
			nr_threshold += (1 << nr_fshift) / nr_run_hysteresis;
		if (avg_nr_run <= (nr_threshold << (FSHIFT - nr_fshift)))
			break;
	}
	nr_run_last = nr_run;

	return nr_run;
}

static int mp_decision(void)
{
	static bool initial = true;

	int next_state = TRIPNDROID_HP_IDLE;
	int online_cpus;
	int index;
	int current_run;
	int req_cpus;

	static cputime64_t total_time = 0;
	static cputime64_t last_time;
	cputime64_t current_time;
	cputime64_t this_time = 0;

	if (state == TRIPNDROID_HP_DISABLED)
		return TRIPNDROID_HP_DISABLED;

	current_time = ktime_to_ms(ktime_get());

	if (current_time <= tripndroid_hp_config.sample_ms)
		return TRIPNDROID_HP_IDLE;

	if (initial) {
		initial = false;
	}
	else {
		this_time = current_time - last_time;
	}

	total_time += this_time;

	sched_get_nr_running_avg(&current_run);
	online_cpus = num_online_cpus();
	req_cpus = calculate_load();

	if (online_cpus) {
		index = (online_cpus - 1) * 2;
		if ((online_cpus < tripndroid_hp_config.max_cpus) && (current_run >= NwNs_Threshold[index])) {
			if (total_time >= TwTs_Threshold[index]) {
				if (online_cpus < req_cpus)
					next_state = TRIPNDROID_HP_UP;
			}
		}
		else if ((online_cpus > 1) && (current_run <= NwNs_Threshold[index+1])) {
			if (total_time >= TwTs_Threshold[index+1]) {
				if (online_cpus > req_cpus)
					next_state = TRIPNDROID_HP_DOWN;
			}
		}
		else {
			next_state = TRIPNDROID_HP_IDLE;
			total_time = 0;
		}
	}
	else {
		total_time = 0;
	}

	if (next_state != TRIPNDROID_HP_IDLE) {
		total_time = 0;
	}

	last_time = ktime_to_ms(ktime_get());

	return next_state;
}

static void tripndroid_hp_wt(struct work_struct *work)
{
	unsigned int cpu = nr_cpu_ids;

	cputime64_t on_time = 0;

	if (ktime_to_ms(ktime_get()) <= tripndroid_hp_config.sample_ms)
		goto out;

        if (tdf_suspend_state == 1)
		goto out;

	if (!mutex_trylock(&tripndroid_hp_cpu_lock))
		goto out;

	if (was_paused) {
		for_each_possible_cpu(cpu) {
			if (cpu_online(cpu))
				per_cpu(tripndroid_hp_cpudata, cpu).online = true;
			else if (!cpu_online(cpu))
				per_cpu(tripndroid_hp_cpudata, cpu).online = false;
		}
		was_paused = false;
	}

	state = mp_decision();
	switch (state) {
	case TRIPNDROID_HP_IDLE:
	case TRIPNDROID_HP_DISABLED:
		break;
	case TRIPNDROID_HP_DOWN:
                cpu = get_slowest_cpu();
                if (cpu < nr_cpu_ids) {
                        if ((per_cpu(tripndroid_hp_cpudata, cpu).online == true) && (cpu_online(cpu))) {
                                cpu_down(cpu);
                                per_cpu(tripndroid_hp_cpudata, cpu).online = false;
                                on_time = ktime_to_ms(ktime_get()) - per_cpu(tripndroid_hp_cpudata, cpu).on_time;
                        }
			else if (per_cpu(tripndroid_hp_cpudata, cpu).online != cpu_online(cpu)) {
				msleep(tripndroid_hp_config.pause);
				was_paused = true;
                        }
                }
		break;
	case TRIPNDROID_HP_UP:
                cpu = cpumask_next_zero(0, cpu_online_mask);
                if (cpu < nr_cpu_ids) {
                        if ((per_cpu(tripndroid_hp_cpudata, cpu).online == false) && (!cpu_online(cpu))) {
                                cpu_up(cpu);
                                per_cpu(tripndroid_hp_cpudata, cpu).online = true;
                                per_cpu(tripndroid_hp_cpudata, cpu).on_time = ktime_to_ms(ktime_get());
                        }
			else if (per_cpu(tripndroid_hp_cpudata, cpu).online != cpu_online(cpu)) {
                                msleep(tripndroid_hp_config.pause);
                                was_paused = true;
                        }
                }
		break;
	default:
		pr_info("TDF: oops! hit an invalid state %d\n", state);
	}
	mutex_unlock(&tripndroid_hp_cpu_lock);

out:
	if (state != TRIPNDROID_HP_DISABLED) {
		schedule_delayed_work_on(0, &tripndroid_hp_w, msecs_to_jiffies(100));
        }

	return;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void tripndroid_hp_early_suspend(struct early_suspend *handler)
{
	int i;

	cancel_delayed_work_sync(&tripndroid_hp_w);

	mutex_lock(&tripndroid_hp_cpu_lock);
	if (!tdf_suspend_state) {
	tdf_suspend_state = 1;
	}
	mutex_unlock(&tripndroid_hp_cpu_lock);

	for (i = 1; i < CONFIG_NR_CPUS; i++) {
		if (cpu_online(i))
			cpu_down(i);

	per_cpu(tripndroid_hp_cpudata, i).online = false;
	}
}

static void __cpuinit tripndroid_hp_late_resume(struct early_suspend *handler)
{
	int i;
	int max_cpus;

	if (powersaving_active == 1) {
	max_cpus = 2;
	}
	else {
	max_cpus = CONFIG_NR_CPUS;
	}

	mutex_lock(&tripndroid_hp_cpu_lock);
	if (tdf_suspend_state) {
	tdf_suspend_state = 0;
	}
	mutex_unlock(&tripndroid_hp_cpu_lock);

	for (i = 1; i < max_cpus; i++) {
		if (!cpu_online(i))
			cpu_up(i);

	per_cpu(tripndroid_hp_cpudata, i).online = true;
	per_cpu(tripndroid_hp_cpudata, i).on_time = ktime_to_ms(ktime_get());
	}

	schedule_delayed_work_on(0, &tripndroid_hp_w, msecs_to_jiffies(10));
}

static struct early_suspend tripndroid_hp_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = tripndroid_hp_early_suspend,
	.resume = tripndroid_hp_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

static int __init tripndroid_hp_init(void)
{
	int cpu, err = 0;
	int sample_ms = usecs_to_jiffies(TRIPNDROID_HP_SAMPLE_MS);

	if (num_online_cpus() > 1)
		sample_ms -= jiffies % sample_ms;

	cpu_clk = clk_get_sys(NULL, "cpu");

	if (IS_ERR(cpu_clk))
		return -ENOENT;

	for_each_possible_cpu(cpu) {
		per_cpu(tripndroid_hp_cpudata, cpu).online = true;
	}

        was_paused = true;

	if (state != TRIPNDROID_HP_DISABLED)
		INIT_DELAYED_WORK(&tripndroid_hp_w, tripndroid_hp_wt);
		schedule_delayed_work_on(0, &tripndroid_hp_w, msecs_to_jiffies(sample_ms));

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&tripndroid_hp_early_suspend_struct_driver);
#endif

	return err;
}
late_initcall(tripndroid_hp_init);
