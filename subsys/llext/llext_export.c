/*
 * Copyright (c) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/llext/symbol.h>

EXPORT_GROUP_SYMBOL(LIBC, strcpy);
EXPORT_GROUP_SYMBOL(LIBC, strncpy);
EXPORT_GROUP_SYMBOL(LIBC, strlen);
EXPORT_GROUP_SYMBOL(LIBC, strcmp);
EXPORT_GROUP_SYMBOL(LIBC, strncmp);
EXPORT_GROUP_SYMBOL(LIBC, memcmp);
EXPORT_GROUP_SYMBOL(LIBC, memcpy);
EXPORT_GROUP_SYMBOL(LIBC, memset);
EXPORT_GROUP_SYMBOL(LIBC, memmove);
EXPORT_GROUP_SYMBOL(LIBC, strtoul);
EXPORT_GROUP_SYMBOL(LIBC, sprintf);

#if defined(CONFIG_ARM) && !defined(CONFIG_CPU_AARCH32_CORTEX_R)
/*
 * On AArch32, compilers may lower memcpy()/memmove()/memset()/zero-init to the
 * ARM EABI run-time helpers instead of the C library functions. These helpers
 * are provided by the compiler run-time (libgcc/compiler-rt); export them so
 * that extensions which reference them can be linked. The exact set emitted
 * depends on the compiler (e.g. Clang uses the aligned __aeabi_memset4 /
 * __aeabi_memclr4 variants where GCC may emit direct memset() calls).
 */
extern void __aeabi_memcpy(void);
extern void __aeabi_memcpy4(void);
extern void __aeabi_memcpy8(void);
extern void __aeabi_memmove(void);
extern void __aeabi_memmove4(void);
extern void __aeabi_memmove8(void);
extern void __aeabi_memset(void);
extern void __aeabi_memset4(void);
extern void __aeabi_memset8(void);
extern void __aeabi_memclr(void);
extern void __aeabi_memclr4(void);
extern void __aeabi_memclr8(void);

EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memcpy);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memcpy4);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memcpy8);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memmove);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memmove4);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memmove8);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memset);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memset4);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memset8);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memclr);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memclr4);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_memclr8);

/* Integer division on AArch32 is lowered to these run-time helpers when the
 * core has no divide instruction, and always for 64-bit operands.
 */
extern void __aeabi_idiv(void);
extern void __aeabi_uidiv(void);
extern void __aeabi_idivmod(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_uldivmod(void);

EXPORT_GROUP_SYMBOL(LIBC, __aeabi_idiv);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_uidiv);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_idivmod);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_uidivmod);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_ldivmod);
EXPORT_GROUP_SYMBOL(LIBC, __aeabi_uldivmod);
#endif /* CONFIG_ARM && !CONFIG_CPU_AARCH32_CORTEX_R */

#if defined(CONFIG_RISCV) && !defined(CONFIG_64BIT)
/* RV32 lowers 64-bit division and modulo to these run-time helpers. */
extern void __divdi3(void);
extern void __udivdi3(void);
extern void __moddi3(void);
extern void __umoddi3(void);

EXPORT_GROUP_SYMBOL(LIBC, __divdi3);
EXPORT_GROUP_SYMBOL(LIBC, __udivdi3);
EXPORT_GROUP_SYMBOL(LIBC, __moddi3);
EXPORT_GROUP_SYMBOL(LIBC, __umoddi3);

#if !defined(CONFIG_RISCV_ISA_EXT_M)
/* Without the M extension, 32-bit multiplication and division, and
 * 64-bit multiplication, are lowered to run-time helpers as well.
 */
extern void __mulsi3(void);
extern void __muldi3(void);
extern void __divsi3(void);
extern void __udivsi3(void);
extern void __modsi3(void);
extern void __umodsi3(void);

EXPORT_GROUP_SYMBOL(LIBC, __mulsi3);
EXPORT_GROUP_SYMBOL(LIBC, __muldi3);
EXPORT_GROUP_SYMBOL(LIBC, __divsi3);
EXPORT_GROUP_SYMBOL(LIBC, __udivsi3);
EXPORT_GROUP_SYMBOL(LIBC, __modsi3);
EXPORT_GROUP_SYMBOL(LIBC, __umodsi3);
#endif /* !CONFIG_RISCV_ISA_EXT_M */
#endif /* CONFIG_RISCV && !CONFIG_64BIT */

/* These symbols are used if CCAC is given the flag -Os */
#ifdef __CCAC__
extern void __ac_mc_va(void);
extern void __ac_push_13_to_13(void);
extern void __ac_push_13_to_14(void);
extern void __ac_push_13_to_15(void);
extern void __ac_push_13_to_16(void);
extern void __ac_push_13_to_17(void);
extern void __ac_push_13_to_18(void);
extern void __ac_push_13_to_19(void);
extern void __ac_push_13_to_20(void);
extern void __ac_push_13_to_21(void);
extern void __ac_push_13_to_22(void);
extern void __ac_push_13_to_23(void);
extern void __ac_push_13_to_24(void);
extern void __ac_push_13_to_25(void);
extern void __ac_push_13_to_26(void);
extern void __ac_push_none(void);
extern void __ac_pop_13_to_13(void);
extern void __ac_pop_13_to_13v(void);
extern void __ac_pop_13_to_14(void);
extern void __ac_pop_13_to_14v(void);
extern void __ac_pop_13_to_15(void);
extern void __ac_pop_13_to_15v(void);
extern void __ac_pop_13_to_16(void);
extern void __ac_pop_13_to_16v(void);
extern void __ac_pop_13_to_17(void);
extern void __ac_pop_13_to_17v(void);
extern void __ac_pop_13_to_18(void);
extern void __ac_pop_13_to_18v(void);
extern void __ac_pop_13_to_19(void);
extern void __ac_pop_13_to_19v(void);
extern void __ac_pop_13_to_20(void);
extern void __ac_pop_13_to_20v(void);
extern void __ac_pop_13_to_21(void);
extern void __ac_pop_13_to_21v(void);
extern void __ac_pop_13_to_22(void);
extern void __ac_pop_13_to_22v(void);
extern void __ac_pop_13_to_23(void);
extern void __ac_pop_13_to_23v(void);
extern void __ac_pop_13_to_24(void);
extern void __ac_pop_13_to_24v(void);
extern void __ac_pop_13_to_25(void);
extern void __ac_pop_13_to_25v(void);
extern void __ac_pop_13_to_26(void);
extern void __ac_pop_13_to_26v(void);
extern void __ac_pop_none(void);
extern void __ac_pop_nonev(void);

EXPORT_SYMBOL(__ac_mc_va);
EXPORT_SYMBOL(__ac_push_13_to_13);
EXPORT_SYMBOL(__ac_push_13_to_14);
EXPORT_SYMBOL(__ac_push_13_to_15);
EXPORT_SYMBOL(__ac_push_13_to_16);
EXPORT_SYMBOL(__ac_push_13_to_17);
EXPORT_SYMBOL(__ac_push_13_to_18);
EXPORT_SYMBOL(__ac_push_13_to_19);
EXPORT_SYMBOL(__ac_push_13_to_20);
EXPORT_SYMBOL(__ac_push_13_to_21);
EXPORT_SYMBOL(__ac_push_13_to_22);
EXPORT_SYMBOL(__ac_push_13_to_23);
EXPORT_SYMBOL(__ac_push_13_to_24);
EXPORT_SYMBOL(__ac_push_13_to_25);
EXPORT_SYMBOL(__ac_push_13_to_26);
EXPORT_SYMBOL(__ac_push_none);
EXPORT_SYMBOL(__ac_pop_13_to_13);
EXPORT_SYMBOL(__ac_pop_13_to_13v);
EXPORT_SYMBOL(__ac_pop_13_to_14);
EXPORT_SYMBOL(__ac_pop_13_to_14v);
EXPORT_SYMBOL(__ac_pop_13_to_15);
EXPORT_SYMBOL(__ac_pop_13_to_15v);
EXPORT_SYMBOL(__ac_pop_13_to_16);
EXPORT_SYMBOL(__ac_pop_13_to_16v);
EXPORT_SYMBOL(__ac_pop_13_to_17);
EXPORT_SYMBOL(__ac_pop_13_to_17v);
EXPORT_SYMBOL(__ac_pop_13_to_18);
EXPORT_SYMBOL(__ac_pop_13_to_18v);
EXPORT_SYMBOL(__ac_pop_13_to_19);
EXPORT_SYMBOL(__ac_pop_13_to_19v);
EXPORT_SYMBOL(__ac_pop_13_to_20);
EXPORT_SYMBOL(__ac_pop_13_to_20v);
EXPORT_SYMBOL(__ac_pop_13_to_21);
EXPORT_SYMBOL(__ac_pop_13_to_21v);
EXPORT_SYMBOL(__ac_pop_13_to_22);
EXPORT_SYMBOL(__ac_pop_13_to_22v);
EXPORT_SYMBOL(__ac_pop_13_to_23);
EXPORT_SYMBOL(__ac_pop_13_to_23v);
EXPORT_SYMBOL(__ac_pop_13_to_24);
EXPORT_SYMBOL(__ac_pop_13_to_24v);
EXPORT_SYMBOL(__ac_pop_13_to_25);
EXPORT_SYMBOL(__ac_pop_13_to_25v);
EXPORT_SYMBOL(__ac_pop_13_to_26);
EXPORT_SYMBOL(__ac_pop_13_to_26v);
EXPORT_SYMBOL(__ac_pop_none);
EXPORT_SYMBOL(__ac_pop_nonev);
#endif

#include <zephyr/syscall_exports_llext.c>
