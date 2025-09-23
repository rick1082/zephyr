/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/linker/devicetree_regions.h>
#define SAMPLE_FREQUENCY    48000
#define SAMPLE_BIT_WIDTH    16
#define BYTES_PER_SAMPLE    sizeof(int16_t)
#define NUMBER_OF_CHANNELS  2

/* Such block length provides an echo with the delay of 10 ms. */
#define SAMPLES_PER_BLOCK   ((SAMPLE_FREQUENCY / 100) * NUMBER_OF_CHANNELS)
#define TIMEOUT             10

#define BLOCK_SIZE  (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (4)

#include <dmm.h>
struct k_mem_slab mem_slab;
char __aligned(WB_UP(4)) mem_slab_buffer[BLOCK_COUNT * WB_UP(BLOCK_SIZE)]
					 DMM_MEMORY_SECTION(DT_ALIAS(i2s_node0));


static int16_t rx_buffer[BLOCK_SIZE];


int main(void)
{
	printf("Hello I2S! %s\n", CONFIG_BOARD_TARGET);
	const struct device *const i2s_dev_rx = DEVICE_DT_GET(DT_ALIAS(i2s_node0));
	struct i2s_config config;
	int ret;
	ret = k_mem_slab_init(&mem_slab, mem_slab_buffer, WB_UP(BLOCK_SIZE), BLOCK_COUNT);
	config.word_size = SAMPLE_BIT_WIDTH;
	config.channels = NUMBER_OF_CHANNELS;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER;
	config.frame_clk_freq = SAMPLE_FREQUENCY;
	config.mem_slab = &mem_slab;
	config.block_size = BLOCK_SIZE;
	config.timeout = TIMEOUT;
	printk("BLOCK_SIZE %d\n", BLOCK_SIZE);

	if (!device_is_ready(i2s_dev_rx)) {
		printk("%s is not ready\n", i2s_dev_rx->name);
		return 0;
	}
	ret = i2s_configure(i2s_dev_rx, I2S_DIR_RX, &config);
	if (ret < 0) {
		printk("Failed to configure RX stream: %d\n", ret);
		return 0;
	}

	ret = i2s_trigger(i2s_dev_rx, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("Failed to trigger command I2S_TRIGGER_START on RX: %d\n", ret);
		return false;
	}

	//static int16_t rx_block[BLOCK_SIZE];
	uint32_t block_size;
	while (1)
	{

		int ret;

		ret = i2s_buf_read(i2s_dev_rx, rx_buffer, &block_size);
		if (ret < 0) {
			printk("Failed to read data: %d\n", ret);
		}
		printk("%d %d %d %d %d\n", block_size, rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3]);
	}
	

	return 0;
}
