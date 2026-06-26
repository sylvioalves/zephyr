/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLATFORM_SVC_H_
#define PLATFORM_SVC_H_

#include <zephyr/kernel.h>

/*
 * The platform service syscalls apps call from user mode. Each brokered service
 * checks the calling app's capabilities and returns -EPERM if the app was not
 * granted it. svc_log and svc_heartbeat are always available.
 */
__syscall int  svc_sensor_read(int32_t *milli_c);   /* CAP_SENSOR */
__syscall void svc_log(const char *msg);
__syscall void svc_heartbeat(void);

__syscall int  svc_led_set(int on);                 /* CAP_GPIO   */
__syscall int  svc_pwm_set(uint8_t percent);        /* CAP_PWM    */
__syscall int  svc_flash_write(uint32_t off, const uint8_t *buf, size_t len); /* CAP_FLASH */
__syscall int  svc_flash_read(uint32_t off, uint8_t *buf, size_t len);        /* CAP_FLASH */
__syscall int  svc_rtc_now(uint32_t *epoch_sec);    /* CAP_RTC    */

/* Supervisor-only: bring up the brokered devices before launching apps. */
void svc_devices_init(void);

#include <zephyr/syscalls/svc.h>

#endif /* PLATFORM_SVC_H_ */
