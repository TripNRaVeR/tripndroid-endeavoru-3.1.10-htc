/*
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

#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/td_framework.h>

/* not for userspace */
unsigned int tdf_suspend_state = 0;
unsigned int nr_run_hysteresis = 4;
unsigned int tdf_cpu_load = 0;

/* make available to userspace */
unsigned int powersaving_active = 0;
unsigned int tdf_fast_charge = 0;


/* create sysfs structure start */
struct kobject *tdf_kobject;

#define show_one(file_name, value)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)               \
{									\
	return sprintf(buf, "%u\n", value);				\
}
show_one(powersave_active, powersaving_active);
show_one(fast_charge, tdf_fast_charge);

static ssize_t store_powersave_active(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int value;
	int ret;
	ret = sscanf(buf, "%u", &value);
	if (ret != 1)
		return -EINVAL;

	powersaving_active = value;

	return count;
}
define_one_global_rw(powersave_active);

static ssize_t store_fast_charge(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int value;
	int ret;
	ret = sscanf(buf, "%u", &value);
	if (ret != 1)
		return -EINVAL;

	tdf_fast_charge = value;

	return count;
}
define_one_global_rw(fast_charge);

static struct attribute *tdf_attributes[] = {
	&powersave_active.attr,
	&fast_charge.attr,
	NULL
};

static struct attribute_group tdf_attr_group = {
	.attrs = tdf_attributes,
};
/* create sysfs structure end */

static int __init td_framework_init(void)
{
	int rc = 0;

	tdf_kobject = kobject_create_and_add("td_framework", NULL);

		if (tdf_kobject)
			rc = sysfs_create_group(tdf_kobject, &tdf_attr_group);

	return rc;

}
late_initcall(td_framework_init);
