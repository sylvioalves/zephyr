/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Standard-library functions a loadable application may call.
 *
 * CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_LIBC already exports what the firmware
 * itself links, but a function no other code uses is dropped by --gc-sections
 * and is then missing at load time, where the failure surfaces as -ENODATA on
 * an unresolved symbol rather than as a build error. Naming one here both
 * exports it and keeps it in the image.
 *
 * Only pure functions belong here: they touch nothing but the memory the
 * caller hands them, so under CONFIG_USERSPACE the worst a faulty application
 * achieves is faulting itself, and the platform reclaims it. Two categories are
 * deliberately absent. The libc allocators operate on an arena in
 * z_malloc_partition, which is in no application's memory domain; a module
 * allocates from its own heap through app_sdk.h. Functions carrying static
 * state between calls, such as strtok(), would let one application corrupt
 * another's parsing and are not pure in this sense.
 */

#include <zephyr/llext/symbol.h>
#include <stdio.h>

EXPORT_SYMBOL(snprintf);
