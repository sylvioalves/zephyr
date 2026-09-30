/*
 * Copyright (c) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "llext_kheap.h"

#ifdef CONFIG_LLEXT_HEAP_EXECUTABLE
#ifdef CONFIG_RISCV_PMP
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/arch/riscv/pmp.h>
#endif

#define LLEXT_HEAP_ALIGN __aligned(LLEXT_HEAP_EXEC_ALIGN)
#else
#define LLEXT_HEAP_ALIGN
#endif

#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
#ifdef CONFIG_HARVARD
struct k_heap llext_instr_heap;
struct k_heap llext_data_heap;
#else
struct k_heap llext_heap;
#endif
#else /* !CONFIG_LLEXT_HEAP_DYNAMIC */
#ifdef CONFIG_HARVARD
Z_HEAP_DEFINE_IN_SECT(llext_instr_heap, (CONFIG_LLEXT_INSTR_HEAP_SIZE * KB(1)),
		      __attribute__((section(".llext_instr_heap"))));
Z_HEAP_DEFINE_IN_SECT(llext_data_heap, (CONFIG_LLEXT_DATA_HEAP_SIZE * KB(1)),
		      __attribute__((section(".llext_data_heap"))));
#else /* !CONFIG_HARVARD */
Z_HEAP_DEFINE_IN_SECT(llext_heap, (CONFIG_LLEXT_HEAP_SIZE * KB(1)),
		      __attribute__((section(".llext_heap"))) LLEXT_HEAP_ALIGN);
#ifdef CONFIG_LLEXT_HEAP_EXECUTABLE
BUILD_ASSERT(sizeof(kheap_llext_heap) == LLEXT_HEAP_EXEC_SIZE &&
		     LLEXT_HEAP_EXEC_SIZE % LLEXT_HEAP_EXEC_ALIGN == 0,
	     "LLEXT heap size must be a multiple of the memory protection granularity");
BUILD_ASSERT(!IS_ENABLED(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT) ||
		     IS_POWER_OF_TWO(LLEXT_HEAP_EXEC_SIZE),
	     "LLEXT heap size must be a power of two for this MPU");
#ifdef CONFIG_RISCV_PMP
PMP_SOC_REGION_DEFINE(llext_heap_exec, kheap_llext_heap,
		      kheap_llext_heap + sizeof(kheap_llext_heap), PMP_R | PMP_W | PMP_X);
#endif
#endif /* CONFIG_LLEXT_HEAP_EXECUTABLE */
#endif /* CONFIG_HARVARD */
#endif /* CONFIG_LLEXT_HEAP_DYNAMIC */
