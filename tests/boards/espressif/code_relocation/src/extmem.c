/* SPDX-FileCopyrightText: Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/linker/section_tags.h>

#include "reloc.h"

__noinit uint32_t ext_buf[EXT_BUF_WORDS];
