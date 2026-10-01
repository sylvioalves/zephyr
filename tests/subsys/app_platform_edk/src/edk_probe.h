/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EDK_PROBE_H_
#define EDK_PROBE_H_

/* A syscall sorting before every application service, which moves their
 * syscall numbers in the firmware built with it.
 */
__syscall int aaa_edk_probe(int value);

#include <zephyr/syscalls/edk_probe.h>

#endif
