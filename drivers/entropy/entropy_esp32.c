/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_trng

#include <string.h>
#include <drivers/entropy.h>
#include <soc/dport_reg.h>
#include <soc/rtc.h>
#include <soc/wdev_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/apb_ctrl_reg.h>
#include <esp_system.h>
#include <soc.h>

extern int esp_clk_cpu_freq(void);
extern int esp_clk_apb_freq(void);

static inline uint32_t entropy_esp32_get_u32(void)
{
	/* The PRNG which implements WDEV_RANDOM register gets 2 bits
	 * of extra entropy from a hardware randomness source every APB clock cycle
	 * (provided WiFi or BT are enabled). To make sure entropy is not drained
	 * faster than it is added, this function needs to wait for at least 16 APB
	 * clock cycles after reading previous word. This implementation may actually
	 * wait a bit longer due to extra time spent in arithmetic and branch statements.
	 *
	 * As a (probably unncessary) precaution to avoid returning the
	 * RNG state as-is, the result is XORed with additional
	 * WDEV_RND_REG reads while waiting.
	 */

	/* This code does not run in a critical section, so CPU frequency switch may
	 * happens while this code runs (this will not happen in the current
	 * implementation, but possible in the future). However if that happens,
	 * the number of cycles spent on frequency switching will certainly be more
	 * than the number of cycles we need to wait here.
	 */
	uint32_t cpu_to_apb_freq_ratio =
		esp_clk_cpu_freq() / esp_clk_apb_freq();

	static uint32_t last_ccount;
	uint32_t ccount;
	uint32_t result = 0;

	do {
		ccount = XTHAL_GET_CCOUNT();
		result ^= REG_READ(WDEV_RND_REG);
	} while (ccount - last_ccount < cpu_to_apb_freq_ratio * 16);
	last_ccount = ccount;
	return result ^ REG_READ(WDEV_RND_REG);
}

static int entropy_esp32_get_entropy(const struct device *device, uint8_t *buf,
				     uint16_t len)
{
	assert(buf != NULL);
	uint8_t *buf_bytes = (uint8_t *)buf;

	while (len > 0) {
		uint32_t word = entropy_esp32_get_u32();
		uint32_t to_copy = MIN(sizeof(word), len);

		memcpy(buf_bytes, &word, to_copy);
		buf_bytes += to_copy;
		len -= to_copy;
	}

	return 0;
}

static int entropy_esp32_init(const struct device *device)
{
	/* clock initialization handled by clock manager */
	return 0;
}

static const struct entropy_driver_api entropy_esp32_api_funcs = {
	.get_entropy = entropy_esp32_get_entropy
};

DEVICE_AND_API_INIT(entropy_esp32, DT_INST_LABEL(0), &entropy_esp32_init, NULL,
		    NULL, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &entropy_esp32_api_funcs);
