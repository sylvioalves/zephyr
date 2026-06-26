/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES12_SVC_H_
#define APP_MORE_SERVICES12_SVC_H_

#include <zephyr/kernel.h>

/*
 * A twelfth batch: a real-time-clock wall-clock service, built on a counter that
 * keeps running across the device. The platform owns the RTC; the app sets and
 * reads the calendar time as plain scalars, so the driver time struct never
 * crosses the user/kernel boundary.
 *
 *   svc_rtc_set - set the wall-clock to the given calendar fields.
 *   svc_rtc_get - read the current wall-clock into the given fields.
 */

/* Scoped: set the RTC to @year-@mon-@mday @hour:@min:@sec (mon 1..12). */
__syscall int svc_rtc_set(int year, int mon, int mday, int hour, int min, int sec);

/* Open: read the current wall-clock into the six output fields. */
__syscall int svc_rtc_get(int *year, int *mon, int *mday, int *hour, int *min, int *sec);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES12_SVC_H_ */
