/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The executable LLEXT heap, outside the kernel RAM area where the MPU
 * programs dynamic regions.
 */
SECTION_DATA_PROLOGUE(.llext_heap_exec,(NOLOAD),)
{
	*(.llext_heap)
	*(.llext_ext_heap)
} GROUP_NOLOAD_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)
