/*
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mbox.h>

typedef struct {
	uint32_t lp_wake_count;
	uint32_t hp_wake_count;
	int16_t  temp_c_x10;
	uint16_t rh_x10;
} lp_shared_data_t;

#if defined(CONFIG_MULTITHREADING)
static K_SEM_DEFINE(g_mbox_data_rx_sem, 0, 1);
#else
static volatile bool g_mbox_received_data_flag;
#endif

static lp_shared_data_t g_mbox_received_data;
static mbox_channel_id_t g_mbox_received_channel;

static void callback(const struct device *dev, mbox_channel_id_t channel_id, void *user_data,
		     struct mbox_msg *data)
{
	memcpy(&g_mbox_received_data, data->data, data->size);
	g_mbox_received_channel = channel_id;

#if defined(CONFIG_MULTITHREADING)
	k_sem_give(&g_mbox_data_rx_sem);
#else
	g_mbox_received_data_flag = true;
#endif
}

int main(void)
{
	const struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);
	const struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);
	struct mbox_msg msg = {0};
	lp_shared_data_t tx_data = {0};
	uint32_t count = 0;

	printk("mbox_data Client demo started\n");

	const int max_transfer_size_bytes = mbox_mtu_get_dt(&tx_channel);

	if (max_transfer_size_bytes < (int)sizeof(lp_shared_data_t)) {
		printk("mbox_mtu_get() error: mtu %d < %u\n",
		       max_transfer_size_bytes, (unsigned int)sizeof(lp_shared_data_t));
		return 0;
	}

	if (mbox_register_callback_dt(&rx_channel, callback, NULL)) {
		printk("mbox_register_callback() error\n");
		return 0;
	}

	if (mbox_set_enabled_dt(&rx_channel, 1)) {
		printk("mbox_set_enable() error\n");
		return 0;
	}

	while (count < 100) {
		tx_data.hp_wake_count = count;
		tx_data.lp_wake_count = 0;
		tx_data.temp_c_x10 = 200 + count;
		tx_data.rh_x10 = 400 + count;

		msg.data = &tx_data;
		msg.size = sizeof(lp_shared_data_t);

		printk("Client send (ch %d) hp_wake=%u temp=%d.%d rh=%u.%u\n",
		       tx_channel.channel_id,
		       tx_data.hp_wake_count,
		       tx_data.temp_c_x10 / 10, abs(tx_data.temp_c_x10 % 10),
		       tx_data.rh_x10 / 10, tx_data.rh_x10 % 10);

		if (mbox_send_dt(&tx_channel, &msg) < 0) {
			printk("mbox_send() error\n");
			return 0;
		}

#if defined(CONFIG_MULTITHREADING)
		k_sem_take(&g_mbox_data_rx_sem, K_FOREVER);
#else
		while (!g_mbox_received_data_flag) {
		}
		g_mbox_received_data_flag = false;
#endif

		printk("Client recv (ch %d) lp_wake=%u temp=%d.%d rh=%u.%u\n",
		       g_mbox_received_channel,
		       g_mbox_received_data.lp_wake_count,
		       g_mbox_received_data.temp_c_x10 / 10,
		       abs(g_mbox_received_data.temp_c_x10 % 10),
		       g_mbox_received_data.rh_x10 / 10,
		       g_mbox_received_data.rh_x10 % 10);
		count++;
	}

	printk("mbox_data Client demo ended\n");
	return 0;
}
