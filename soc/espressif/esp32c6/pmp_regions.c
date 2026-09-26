/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/devicetree.h>
#include <zephyr/linker/linker-defs.h>
#include <pmp.h>

/*
 * ESP32-C6 SoC ROM region.
 *
 * The ESP32-C6 has a ROM at 0x40000000 containing libc and other utility
 * functions. This region needs to be accessible (R+X) from both kernel
 * and user mode for proper operation.
 *
 * The size is rounded up to the next power of two so the region encodes
 * as a single NAPOT entry instead of a two-slot TOR range. The space past
 * the ROM up to the rounded size is reserved and holds no memory.
 */
#define SOC_ROM_NODE DT_NODELABEL(soc_rom)
#define SOC_ROM_NAPOT_SIZE ((uintptr_t)NHPOT(DT_REG_SIZE(SOC_ROM_NODE)))

PMP_SOC_REGION_DEFINE(esp32c6_soc_rom, DT_REG_ADDR(SOC_ROM_NODE),
		      DT_REG_ADDR(SOC_ROM_NODE) + SOC_ROM_NAPOT_SIZE, PMP_R | PMP_X);

/*
 * ESP32-C6 IRAM text region.
 *
 * On ESP32-C6, IRAM and DRAM share the same 512KB physical memory space
 * (0x40800000-0x40880000). The split between code (IRAM) and data (DRAM)
 * is determined at link time. Only the IRAM text portion should be
 * executable to maintain security - making the entire region executable
 * would allow code execution from the data area.
 *
 * The linker symbols _iram_text_start and _iram_text_end define the
 * actual IRAM text boundaries.
 */
extern char _iram_text_start[];
extern char _iram_text_end[];

PMP_SOC_REGION_DEFINE(esp32c6_iram_text, _iram_text_start, _iram_text_end, PMP_R | PMP_X);

/*
 * Flash-mapped read-only data (DROM).
 *
 * Const data and string literals live in a separate MMU window from
 * executable flash text (__rom_region). Without an explicit PMP entry,
 * user mode cannot read that window. Use _image_rodata_end so sections
 * after __rodata_region_end that still map into DROM are covered.
 *
 * The entry starts at __rom_region_end so it shares its lower bound with
 * the flash text entry and takes a single PMP slot. The addresses between
 * the end of the text and the DROM mapping only hold image padding.
 */
extern char _image_rodata_end[];

PMP_SOC_REGION_DEFINE(esp32c6_flash_rodata, __rom_region_end, _image_rodata_end, PMP_R);
