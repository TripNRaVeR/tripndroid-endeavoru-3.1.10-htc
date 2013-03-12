/*
 * drivers/misc/tripndroid_vibrator.c
 *
 * Copyright (c) 2012-2013, TripNDroid Mobile Engineering
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

#include <linux/regulator/consumer.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/pwm.h>
#include <linux/tripndroid_vibrator.h>

#include "../staging/android/timed_output.h"

#define PLAYBACK_PERIOD_US 		50000
#define PLAYBACK_DUTY_US 		36000
#define ZERO_DUTY_US 			25000

struct vibrator {
	struct timed_output_dev timed_dev;
	struct vibrator_platform_data *pdata;
	int pwm_duty;
	struct work_struct work;
	struct hrtimer vib_timer;
};

struct vibrator *g_vib;

static int timeout;
static struct workqueue_struct *vib_work_queue;

static enum hrtimer_restart vib_timer_func(struct hrtimer *timer)
{
	struct vibrator *vib = container_of(timer, struct vibrator, vib_timer);

	if(vib->pdata->ena_gpio >= 0)
		gpio_direction_output(vib->pdata->ena_gpio, 0);

	if(vib->pdata->pwr_gpio >= 0)
		gpio_direction_output(vib->pdata->pwr_gpio, 0);

	queue_work(vib_work_queue, &vib->work);

	return HRTIMER_NORESTART;
}

static void vibrator_start(struct vibrator *vib)
{
	pwm_config(vib->pdata->pwm_data.pwm_dev, g_vib->pwm_duty, PLAYBACK_PERIOD_US);
	pwm_enable(vib->pdata->pwm_data.pwm_dev);

	if(vib->pdata->ena_gpio >= 0)
		gpio_direction_output(vib->pdata->ena_gpio, 1);

	if(vib->pdata->pwr_gpio >= 0)
		gpio_direction_output(vib->pdata->pwr_gpio, 1);
}

static void vibrator_stop(struct vibrator *vib)
{
	pwm_config(vib->pdata->pwm_data.pwm_dev, ZERO_DUTY_US, PLAYBACK_PERIOD_US);

	pwm_enable(vib->pdata->pwm_data.pwm_dev);
	pwm_disable(vib->pdata->pwm_data.pwm_dev);

	if(vib->pdata->ena_gpio >= 0)
		gpio_direction_output(vib->pdata->ena_gpio, 0);

	if(vib->pdata->pwr_gpio >= 0)
		gpio_direction_output(vib->pdata->pwr_gpio, 0);
}

/*
 * Timeout value can be changed from sysfs entry
 * created by timed_output_dev.
 * echo 100 > /sys/class/timed_output/vibrator/enable
 */
static void vibrator_enable(struct timed_output_dev *dev, int value)
{
	struct vibrator *vib = container_of(dev, struct vibrator, timed_dev);
	timeout = value;

	if (value < 0)
		value = 0;

	if (value) {
		vibrator_start(vib);
		hrtimer_start(&vib->vib_timer, ktime_set(value / 1000, (value % 1000) * 1000000), HRTIMER_MODE_REL);
	}
	else {
		vibrator_stop(vib);
	}
}

static void vib_work_func(struct work_struct *work)
{
	struct vibrator *vib = container_of(work, struct vibrator, work);
	vibrator_stop(vib);
}

/*
 * Timeout value can be read from sysfs entry
 * created by timed_output_dev.
 * cat /sys/class/timed_output/vibrator/enable
 */
static int vibrator_get_time(struct timed_output_dev *dev)
{
	struct vibrator *vib = container_of(dev, struct vibrator, timed_dev);
	if (hrtimer_active(&vib->vib_timer)) {
		ktime_t r = hrtimer_get_remaining(&vib->vib_timer);
		return (int)ktime_to_us(r);
	} else
		return 0;
}

static ssize_t show_dutycycle(struct device *dev, struct device_attribute *attr,char *buf)
{
	return sprintf(buf, "%d\n",(int)g_vib->pwm_duty);
}

static ssize_t set_dutycycle(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	long val = simple_strtol(buf, NULL, 10);

		if(val>PLAYBACK_PERIOD_US)
			val = PLAYBACK_PERIOD_US;
		if(val < 0)
			val = 0;
		g_vib->pwm_duty = (int)val;

	return count;
}

static DEVICE_ATTR(dutycycle, 0644, show_dutycycle, set_dutycycle);

static int vibrator_probe(struct platform_device *pdev)
{
	struct vibrator_platform_data *pdata = pdev->dev.platform_data;
	struct vibrator *vib;
	int rc = 0;

	if (!pdata)
		return -EINVAL;
	vib = kzalloc(sizeof(struct vibrator), GFP_KERNEL);

	if (!vib)
		return -ENOMEM;
	vib->pdata=kzalloc(sizeof(struct vibrator_platform_data), GFP_KERNEL);

	if(!(vib->pdata)){
		rc = -ENOMEM;
		goto err_platform_data_allocate;
	}

	vib->pdata->pwm_gpio = pdata->pwm_gpio;
	vib->pdata->ena_gpio = pdata->ena_gpio;
	vib->pdata->pwr_gpio = pdata->pwr_gpio;
	vib->pdata->pwm_data.name = pdata->pwm_data.name;
	vib->pdata->pwm_data.bank = pdata->pwm_data.bank;

	vib->pwm_duty = PLAYBACK_DUTY_US;

	INIT_WORK(&vib->work, vib_work_func);

	hrtimer_init(&vib->vib_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->vib_timer.function = vib_timer_func;

	vib->pdata->pwm_data.pwm_dev = pwm_request(vib->pdata->pwm_data.bank, "vibrator");

	rc = gpio_request(vib->pdata->pwm_gpio, "vibrator_pwm");
	if (rc) {
		rc = -ENOMEM;
	}

	rc = gpio_request(vib->pdata->ena_gpio, "vibrator_ena");
	if (rc) {
		rc = -ENOMEM;
	}

	rc = gpio_direction_output(vib->pdata->ena_gpio,0);
	if(rc < 0){
		goto err_ena_output;
	}

	tegra_gpio_enable(vib->pdata->ena_gpio);
	gpio_free(vib->pdata->pwr_gpio);

	rc = gpio_request(vib->pdata->pwr_gpio, "vibrator_pwr");
	if (rc) {
		rc = -ENOMEM;
	}
	rc = gpio_direction_output(vib->pdata->pwr_gpio,0);
	if(rc < 0){
		goto err_pwr_output;
	}

	tegra_gpio_enable(vib->pdata->pwr_gpio);

	vib->timed_dev.name = "vibrator";
	vib->timed_dev.get_time = vibrator_get_time;
	vib->timed_dev.enable = vibrator_enable;

	timed_output_dev_register(&vib->timed_dev);

	rc = device_create_file(vib->timed_dev.dev, &dev_attr_dutycycle);
	if (rc){
		goto err_create_file;
	}

	vib_work_queue = create_workqueue("vib");
	if (!vib_work_queue)
		goto err_create_file;

	g_vib = vib;
	g_vib->pwm_duty = PLAYBACK_DUTY_US;

	platform_set_drvdata(pdev, vib);

	return 0;

err_create_file:
	timed_output_dev_unregister(&vib->timed_dev);
err_pwr_output:
	gpio_free(vib->pdata->pwr_gpio);
err_ena_output:
	gpio_free(vib->pdata->ena_gpio);
	gpio_free(vib->pdata->pwm_gpio);
err_platform_data_allocate:
	kfree(vib);

	return rc;
}

static int vibrator_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct vibrator *vib = platform_get_drvdata(pdev);

	vibrator_stop(vib);
	hrtimer_cancel(&vib->vib_timer);
	cancel_work_sync(&vib->work);
	gpio_direction_output(vib->pdata->pwm_gpio, 0);
	tegra_gpio_enable(vib->pdata->pwm_gpio);

	return 0;
}

static int vibrator_resume(struct platform_device *pdev)
{
	struct vibrator *vib = platform_get_drvdata(pdev);

	tegra_gpio_disable(vib->pdata->pwm_gpio);
	vib->pwm_duty = PLAYBACK_DUTY_US;

	return 0;
}

static int __devexit vibrator_remove(struct platform_device *pdev)
{
	struct vibrator *vib = platform_get_drvdata(pdev);

	cancel_work_sync(&vib->work);

	hrtimer_cancel(&vib->vib_timer);

	gpio_free(vib->pdata->pwm_gpio);
	gpio_free(vib->pdata->ena_gpio);
	gpio_free(vib->pdata->pwr_gpio);

	kfree(vib->pdata);
	kfree(vib);

	timed_output_dev_unregister(&vib->timed_dev);

	destroy_workqueue(vib_work_queue);

	return 0;
}

static struct platform_driver vibrator_driver = {
	.probe		= vibrator_probe,
	.remove		= __devexit_p(vibrator_remove),
	.suspend	= vibrator_suspend,
	.resume		= vibrator_resume,
	.driver		= {
		.name	= VIBRATOR_NAME,
		.owner	= THIS_MODULE,
	},
};

static int __init vibrator_init(void)
{
	return platform_driver_register(&vibrator_driver);
}
module_init(vibrator_init);

static void __exit vibrator_exit(void)
{
	platform_driver_unregister(&vibrator_driver);
}
module_exit(vibrator_exit);

MODULE_DESCRIPTION("tripndroid vibrator driver");
