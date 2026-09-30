/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RENUMBER_PROBE_H_
#define RENUMBER_PROBE_H_

/* A syscall sorting before every application service, which moves their
 * numbers away from the ones of the producer firmware.
 */
__syscall int aaa_renumber_probe(int value);

#include <zephyr/syscalls/renumber_probe.h>

#endif
