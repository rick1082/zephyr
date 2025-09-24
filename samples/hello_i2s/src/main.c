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

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/sys_clock.h>
#include <zephyr/toolchain.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>

#include <sample_usbd.h>

#define USB_SAMPLE_RATE_HZ 48000U
#define IN_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))
#define USB_ENQUEUE_COUNT        30U
#define USB_FRAME_DURATION_US    125U
#define USB_SAMPLE_CNT           ((USB_FRAME_DURATION_US * USB_SAMPLE_RATE_HZ) / USEC_PER_SEC)
#define USB_BYTES_PER_SAMPLE     sizeof(int16_t)
#define USB_MONO_FRAME_SIZE      (USB_SAMPLE_CNT * USB_BYTES_PER_SAMPLE)
#define USB_CHANNELS             2U
#define USB_STEREO_FRAME_SIZE    (USB_MONO_FRAME_SIZE * USB_CHANNELS)
#define USB_OUT_RING_BUF_SIZE    (10 * 960)
#define USB_IN_RING_BUF_SIZE     (USB_MONO_FRAME_SIZE * USB_ENQUEUE_COUNT)

RING_BUF_DECLARE(usb_out_ring_buf, USB_OUT_RING_BUF_SIZE);
K_MEM_SLAB_DEFINE_STATIC(usb_out_buf_pool, ROUND_UP(USB_STEREO_FRAME_SIZE, UDC_BUF_GRANULARITY),
			 USB_ENQUEUE_COUNT, UDC_BUF_ALIGN);
static volatile bool terminal_enabled;

static void uac2_sof_cb(const struct device *dev, void *user_data)
{
	void *pcm_buf;
	uint32_t size;
	int err;

	if (!terminal_enabled) {
		/* Simply discard the data then */
		(void)ring_buf_get(&usb_out_ring_buf, NULL, USB_STEREO_FRAME_SIZE);
		return;
	}

	err = k_mem_slab_alloc(&usb_out_buf_pool, &pcm_buf, K_NO_WAIT);
	if (err != 0) {
		printk("Could not allocate pcm_buf\n");
		return;
	}

	size = ring_buf_get(&usb_out_ring_buf, pcm_buf, USB_STEREO_FRAME_SIZE);
	if (size != USB_STEREO_FRAME_SIZE) {
		/* If we could not fill the buffer, zero-fill the rest (possibly all) */
		memset(((uint8_t *)pcm_buf) + size, 100, USB_STEREO_FRAME_SIZE - size);
	}

	err = usbd_uac2_send(dev, IN_TERMINAL_ID, pcm_buf, USB_STEREO_FRAME_SIZE);
	if (err != 0) {
		k_mem_slab_free(&usb_out_buf_pool, pcm_buf);
	} /* USB owns the buffer which will be released in uac2_buf_release_cb */
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal, void *buf,
				void *user_data)
{
	k_mem_slab_free(&usb_out_buf_pool, buf);
}

static void terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
			       bool microframes, void *user_data)
{
	terminal_enabled = enabled;
}


int usb_init(void)
{
	const struct device *mic_dev = DEVICE_DT_GET(DT_NODELABEL(uac2_microphone));
	static struct uac2_ops usb_audio_ops = {
		.sof_cb = uac2_sof_cb,
		.buf_release_cb = uac2_buf_release_cb,
		.terminal_update_cb = terminal_update_cb,
	};
	struct usbd_context *sample_usbd;
	static bool initialized;
	int err;

	if (initialized) {
		return -EALREADY;
	}

	if (!device_is_ready(mic_dev)) {
		printk("Cannot get USB Microphone Device\n");
		return -EIO;
	}

	usbd_uac2_set_ops(mic_dev, &usb_audio_ops, NULL);

	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		return -ENODEV;
	}

	err = usbd_enable(sample_usbd);
	if (err != 0) {
		return err;
	}

	printk("USB initialized\n");
	initialized = true;

	return 0;
}

int main(void)
{
	printf("Hello I2S! %s\n", CONFIG_BOARD_TARGET);
	usb_init();
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

	uint32_t block_size;
	while (1) {

		int ret;

		ret = i2s_buf_read(i2s_dev_rx, rx_buffer, &block_size);
		if (ret < 0) {
			printk("Failed to read data: %d\n", ret);
		}

		/* quick workaround since I'm using mono mic from
		 *		     https://www.adafruit.com/product/3421
		 */
		for (int i = 0; i < block_size; i++) {
			if (i % 2 != 0) {
				rx_buffer[i] = 0;
			}
		}

		ret = ring_buf_put(&usb_out_ring_buf, (uint8_t *)rx_buffer, block_size);
		if (ret < 0) {
			printk("Failed to put data to ring buffer: %d\n", ret);
		}
	}

	return 0;
}
