/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

/* The platform owns the RTC device (a counter-backed wall clock). */
static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(counter_rtc));

int z_impl_svc_rtc_set(int year, int mon, int mday, int hour, int min, int sec)
{
	if (!device_is_ready(rtc) || mon < 1 || mon > 12) {
		return -EINVAL;
	}

	struct rtc_time t = {
		.tm_year = year - 1900,
		.tm_mon = mon - 1,
		.tm_mday = mday,
		.tm_hour = hour,
		.tm_min = min,
		.tm_sec = sec,
		.tm_wday = -1,
		.tm_yday = -1,
		.tm_isdst = -1,
	};

	return rtc_set_time(rtc, &t);
}

int z_impl_svc_rtc_get(int *year, int *mon, int *mday, int *hour, int *min, int *sec)
{
	if (!device_is_ready(rtc)) {
		return -ENODEV;
	}

	struct rtc_time t;
	int rc = rtc_get_time(rtc, &t);

	if (rc != 0) {
		return rc;
	}

	*year = t.tm_year + 1900;
	*mon = t.tm_mon + 1;
	*mday = t.tm_mday;
	*hour = t.tm_hour;
	*min = t.tm_min;
	*sec = t.tm_sec;
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_rtc_set(int year, int mon, int mday, int hour, int min, int sec)
{
	return z_impl_svc_rtc_set(year, mon, mday, hour, min, sec);
}
#include <zephyr/syscalls/svc_rtc_set_mrsh.c>

static inline int z_vrfy_svc_rtc_get(int *year, int *mon, int *mday, int *hour, int *min, int *sec)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(year, sizeof(*year)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(mon, sizeof(*mon)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(mday, sizeof(*mday)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(hour, sizeof(*hour)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(min, sizeof(*min)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(sec, sizeof(*sec)));
	return z_impl_svc_rtc_get(year, mon, mday, hour, min, sec);
}
#include <zephyr/syscalls/svc_rtc_get_mrsh.c>
#endif
