/*
 * Copyright (c) 2021-2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/zephyr.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/sys/byteorder.h>
#include <logging/log.h>
LOG_MODULE_REGISTER(main);

#define DEVICE_NAME_PEER_L "BT_HEADSET_L"
#define DEVICE_NAME_PEER_L_LEN (sizeof(DEVICE_NAME_PEER_L) - 1)

#define DEVICE_NAME_PEER_R "BT_HEADSET_R"
#define DEVICE_NAME_PEER_R_LEN (sizeof(DEVICE_NAME_PEER_R) - 1)

static void start_scan(void);

enum {
	HEADSET_L = 0,
	HEADSET_R = 1,
};

static struct bt_conn *headset_conn[2];

static struct k_work_delayable audio_send_work;
static struct bt_audio_unicast_group *unicast_group;
static struct bt_codec *remote_codec_capabilities[CONFIG_BT_AUDIO_UNICAST_CLIENT_PAC_COUNT];
static struct bt_audio_ep *sinks[CONFIG_BT_AUDIO_UNICAST_CLIENT_ASE_SNK_COUNT];
NET_BUF_POOL_FIXED_DEFINE(tx_pool, CONFIG_BT_AUDIO_UNICAST_CLIENT_ASE_SNK_COUNT,
			  CONFIG_BT_ISO_TX_MTU + BT_ISO_CHAN_SEND_RESERVE,
			  8, NULL);

static struct bt_audio_stream streams[CONFIG_BT_AUDIO_UNICAST_CLIENT_ASE_SNK_COUNT +
				      CONFIG_BT_AUDIO_UNICAST_CLIENT_ASE_SRC_COUNT];
static size_t configured_stream_count = 2;


/* Select a codec configuration to apply that is mandatory to support by both client and server.
 * Allows this sample application to work without logic to parse the codec capabilities of the
 * server and selection of an appropriate codec configuration.
 */
static struct bt_audio_lc3_preset codec_configuration =
	BT_AUDIO_LC3_UNICAST_PRESET_16_2_1(BT_AUDIO_LOCATION_FRONT_LEFT,
					   BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED);



void init_lc3(void)
{
	printk("init lc3\n");
}

/**
 * @brief Send audio data on timeout
 *
 * This will send an increasing amount of audio data, starting from 1 octet.
 * The data is just mock data, and does not actually represent any audio.
 *
 * First iteration : 0x00
 * Second iteration: 0x00 0x01
 * Third iteration : 0x00 0x01 0x02
 *
 * And so on, until it wraps around the configured MTU (CONFIG_BT_ISO_TX_MTU)
 *
 * @param work Pointer to the work structure
 */
static void audio_timer_timeout(struct k_work *work)
{
	int ret;
	static uint8_t buf_data[40];
	static bool data_initialized;
	struct net_buf *buf_l, *buf_r;
	static size_t len_to_send = 1;
	static uint32_t seq_r, seq_l;

	if (!data_initialized) {
		/* TODO: Actually encode some audio data */
		for (int i = 0; i < ARRAY_SIZE(buf_data); i++) {
			buf_data[i] = (uint8_t)i;
		}

		data_initialized = true;
	}

	if(streams[0].iso->state == BT_ISO_STATE_CONNECTED) {
			buf_l = net_buf_alloc(&tx_pool, K_FOREVER);
			net_buf_reserve(buf_l, BT_ISO_CHAN_SEND_RESERVE);

			net_buf_add_mem(buf_l, buf_data, len_to_send);
			ret = bt_audio_stream_send(&streams[0], buf_l, seq_l++, BT_ISO_TIMESTAMP_NONE);
			if (ret < 0) {
				LOG_ERR("Failed to send audio data (%d)", ret);
				net_buf_unref(buf_l);
			} else {
				LOG_INF("Sending mock data with len %zu", len_to_send);
			}
	}

	if(streams[1].iso->state == BT_ISO_STATE_CONNECTED) {
			buf_r = net_buf_alloc(&tx_pool, K_FOREVER);
			net_buf_reserve(buf_r, BT_ISO_CHAN_SEND_RESERVE);

			net_buf_add_mem(buf_r, buf_data, len_to_send);
			ret = bt_audio_stream_send(&streams[1], buf_r, seq_r++, BT_ISO_TIMESTAMP_NONE);
			if (ret < 0) {
				LOG_ERR("Failed to send audio data (%d)", ret);
				net_buf_unref(buf_r);
			} else {
				LOG_INF("Sending mock data with len %zu", len_to_send);
			}
	}


	k_work_schedule(&audio_send_work, K_MSEC(1000));

	len_to_send++;
	if (len_to_send > ARRAY_SIZE(buf_data)) {
		len_to_send = 1;
	}
}

static void print_hex(const uint8_t *ptr, size_t len)
{
	while (len-- != 0) {
		printk("%02x", *ptr++);
	}
}

static void print_codec_capabilities(const struct bt_codec *codec)
{
#if 1
	printk("codec 0x%02x cid 0x%04x vid 0x%04x count %u\n",
	       codec->id, codec->cid, codec->vid, codec->data_count);

	for (size_t i = 0; i < codec->data_count; i++) {
		printk("data #%zu: type 0x%02x len %u\n",
		       i, codec->data[i].data.type,
		       codec->data[i].data.data_len);
		print_hex(codec->data[i].data.data,
			  codec->data[i].data.data_len -
			  sizeof(codec->data[i].data.type));
		printk("\n");
	}

	for (size_t i = 0; i < codec->meta_count; i++) {
		printk("meta #%zu: type 0x%02x len %u\n",
		       i, codec->meta[i].data.type,
		       codec->meta[i].data.data_len);
		print_hex(codec->meta[i].data.data,
			  codec->meta[i].data.data_len -
			  sizeof(codec->meta[i].data.type));
		printk("\n");
	}
#endif
}

#define BT_LE_CONN_PARAM_TWS BT_LE_CONN_PARAM(100, 100, 0, 400)

static bool check_audio_support_and_connect(struct bt_data *data, void *user_data)
{
	bt_addr_le_t *addr = user_data;
	int ret;
	//printk("[AD]: %u data_len %u\n", data->type, data->data_len);
	struct bt_conn *conn;
	switch (data->type) {
	case BT_DATA_NAME_COMPLETE:
		if ((data->data_len == DEVICE_NAME_PEER_L_LEN) &&
		    (strncmp(DEVICE_NAME_PEER_L, data->data, DEVICE_NAME_PEER_L_LEN) == 0)) {
			bt_le_scan_stop();

			ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_TWS,
						&conn);
			if (ret) {
				LOG_ERR("Could not init connection %d", ret);
				return ret;
			}
			headset_conn[HEADSET_L] = conn;
			return 0; //stop parsing
		} else if ((data->data_len == DEVICE_NAME_PEER_R_LEN) &&
			   (strncmp(DEVICE_NAME_PEER_R, data->data, DEVICE_NAME_PEER_R_LEN) == 0)) {
			bt_le_scan_stop();

			ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_TWS,
						&conn);
			if (ret) {
				LOG_ERR("Could not init connection %d", ret);
				return ret;
			}
			headset_conn[HEADSET_R] = conn;
			return 0; //stop parsing
		}
	}

	return true;
}

static void stream_configured(struct bt_audio_stream *stream,
			      const struct bt_codec_qos_pref *pref)
{
	int err;
	LOG_INF("Audio Stream %p configured, conn %p", (void *)stream, (void *)stream->conn);

	if(stream->conn == headset_conn[HEADSET_L]) {
		err = bt_audio_stream_qos(headset_conn[HEADSET_L], unicast_group);
		if (err != 0) {
			LOG_ERR("Unable to setup QoS for conn %p: %d", (void *)headset_conn[HEADSET_L], err);
		} else {
			LOG_INF("qos set");
		}
	}
	if(stream->conn == headset_conn[HEADSET_R]) {
		err = bt_audio_stream_qos(headset_conn[HEADSET_R], unicast_group);
		if (err != 0) {
			LOG_ERR("Unable to setup QoS for conn %p: %d", (void *)headset_conn[HEADSET_L], err);
		} else {
			LOG_INF("qos set");
		}
	}

}

static void stream_qos_set(struct bt_audio_stream *stream)
{
	int err;
	printk("Audio Stream %p QoS set\n", stream);
	LOG_INF("Audio Stream %p QoS set", (void *)stream);
	err = bt_audio_stream_enable(stream, codec_configuration.codec.meta,
				     codec_configuration.codec.meta_count);
	if (err != 0) {
		LOG_ERR("Unable to enable stream: %d", err);
	} else {
		LOG_INF("enable stream");
	}

}

static void stream_enabled(struct bt_audio_stream *stream)
{
	int err;

	LOG_INF("Audio Stream %p enabled", (void *)stream);
	if(stream->conn == headset_conn[HEADSET_L]) {
		err = bt_audio_stream_start(&streams[0]);
		while(err != 0) {
			k_sleep(K_MSEC(1000));
			LOG_ERR("Unable to start stream: %d", err);
			err = bt_audio_stream_start(&streams[0]);
		}
	}
	if(stream->conn == headset_conn[HEADSET_R]) {
		err = bt_audio_stream_start(&streams[1]);
		while(err != 0) {
			k_sleep(K_MSEC(1000));
			LOG_ERR("Unable to start stream: %d", err);
			err = bt_audio_stream_start(&streams[1]);
		}
	}

}

static void stream_started(struct bt_audio_stream *stream)
{
	printk("Audio Stream %p started\n", stream);
	k_work_reschedule(&audio_send_work, K_MSEC(10));
}

static void stream_metadata_updated(struct bt_audio_stream *stream)
{
	printk("Audio Stream %p metadata updated\n", stream);
}

static void stream_disabled(struct bt_audio_stream *stream)
{
	printk("Audio Stream %p disabled\n", stream);
}

static void stream_stopped(struct bt_audio_stream *stream)
{
	printk("Audio Stream %p stopped\n", stream);

	/* Stop send timer */
	//k_work_cancel_delayable(&audio_send_work);
}

static void stream_released(struct bt_audio_stream *stream)
{
	printk("Audio Stream %p released\n", stream);
}

static void stream_recv(struct bt_audio_stream *stream,
			const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	printk("Incoming audio on stream %p len %u\n", stream, buf->len);
}

static struct bt_audio_stream_ops stream_ops = {
	.configured = stream_configured,
	.qos_set = stream_qos_set,
	.enabled = stream_enabled,
	.started = stream_started,
	.metadata_updated = stream_metadata_updated,
	.disabled = stream_disabled,
	.stopped = stream_stopped,
	.released = stream_released,
	.recv = stream_recv
};

static void add_remote_sink(struct bt_audio_ep *ep, uint8_t index)
{
	printk("Sink #%u: ep %p\n", index, ep);

	if (index > ARRAY_SIZE(sinks)) {
		printk("Could not add sink ep[%u]\n", index);
		return;
	}

	sinks[index] = ep;
}

static void add_remote_codec(struct bt_codec *codec_capabilities, int index,
			     enum bt_audio_dir dir)
{
	printk("#%u: codec_capabilities %p dir 0x%02x\n",
	       index, codec_capabilities, dir);

	print_codec_capabilities(codec_capabilities);

	if (dir != BT_AUDIO_DIR_SINK && dir != BT_AUDIO_DIR_SOURCE) {
		return;
	}

	if (index < CONFIG_BT_AUDIO_UNICAST_CLIENT_PAC_COUNT) {
		remote_codec_capabilities[index] = codec_capabilities;
	}
}

static void discover_sink_cb(struct bt_conn *conn, struct bt_codec *codec, struct bt_audio_ep *ep,
			     struct bt_audio_discover_params *params)
{
	int ep_index = 0;
	int err;
	if (conn == headset_conn[HEADSET_L]) {
		LOG_INF("discover sink cb for left");
		ep_index = HEADSET_L;
	} else if (conn == headset_conn[HEADSET_R]) {
		LOG_INF("discover sink cb for right");
		ep_index = HEADSET_R;
	}

	if (params->err != 0) {
		LOG_INF("Discovery failed: %d", params->err);
		return;
	}

	if (codec != NULL) {
		add_remote_codec(codec, params->num_caps, params->dir);
		return;
	}

	if (ep != NULL) {
		if (params->dir == BT_AUDIO_DIR_SINK) {
			add_remote_sink(ep, ep_index);
		} else {
			LOG_INF("Invalid param type: %u", params->dir);
		}

		return;
	}

	LOG_INF("Discover complete: err %d", params->err);

	(void)memset(params, 0, sizeof(*params));

	if (conn == headset_conn[HEADSET_L]) {
		err = bt_audio_stream_config(conn, &streams[HEADSET_L], sinks[HEADSET_L],
					     &codec_configuration.codec);
		LOG_INF("configure stream for sink[HEADSET_L], err = %d", err);
	} else if (conn == headset_conn[HEADSET_R]) {
		err = bt_audio_stream_config(conn, &streams[HEADSET_R], sinks[HEADSET_R],
					     &codec_configuration.codec);
		LOG_INF("configure stream for sink[HEADSET_R], err = %d", err);
	}
}

bool all_headset_connected(void)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (headset_conn[i] == NULL) {
			//LOG_ERR("device %d not connected", i);
			return false;
		}
	}
	LOG_INF("all devices connected");
	return true;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		LOG_INF("Failed to connect to %s (%u)", addr, err);

		if (conn == headset_conn[HEADSET_L]) {
			bt_conn_unref(headset_conn[HEADSET_L]);
			headset_conn[HEADSET_L] = NULL;
		} else if (conn == headset_conn[HEADSET_R]) {
			bt_conn_unref(headset_conn[HEADSET_R]);
			headset_conn[HEADSET_R] = NULL;
		}
		
		start_scan();
		return;
	}

	if (conn != headset_conn[HEADSET_L] && conn != headset_conn[HEADSET_R]) {
		return;
	}
	LOG_INF("bt_conn_set_security");
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err != 0) {
		LOG_ERR("SMP failed, err = %d", err);
	}
	LOG_INF("Connected: %s", addr);
	if (!all_headset_connected()) {
		start_scan();
	} else {
		LOG_INF("stop scan");
		bt_le_scan_stop();
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	if (conn != headset_conn[HEADSET_L] && conn != headset_conn[HEADSET_R]) {
		return;
	}

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

	if (conn == headset_conn[HEADSET_L]) {
		bt_conn_unref(headset_conn[HEADSET_L]);
		headset_conn[HEADSET_L] = NULL;
	} else if (conn == headset_conn[HEADSET_R]) {
		bt_conn_unref(headset_conn[HEADSET_R]);
		headset_conn[HEADSET_R] = NULL;
	}

	start_scan();
}

static int discover_sink(struct bt_conn *conn)
{
	int err = 0;
	static struct bt_audio_discover_params params_l;
	static struct bt_audio_discover_params params_r;

	params_l.func = discover_sink_cb;
	params_l.dir = BT_AUDIO_DIR_SINK;
	params_r.func = discover_sink_cb;
	params_r.dir = BT_AUDIO_DIR_SINK;

	if (conn == headset_conn[HEADSET_L]) {
		LOG_INF("bt_audio_discover for left");
		err = bt_audio_discover(conn, &params_l);
	} else if (conn == headset_conn[HEADSET_R]) {
		LOG_INF("bt_audio_discover for right");
		err = bt_audio_discover(conn, &params_r);
	}
	if (err != 0) {
		LOG_ERR("Failed to discover sink: %d", err);
		return err;
	}


	return 0;
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	int ret;

	if (err) {
		LOG_ERR("Security failed: level %u err %d", level, err);
		ret = bt_conn_disconnect(conn, err);
		if (ret) {
			LOG_ERR("Failed to disconnect %d", ret);
		}
	} else {
		LOG_INF("Security changed: level %u", level);
		discover_sink(conn);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed_cb,
};

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	printk("MTU exchanged: %u/%u\n", tx, rx);
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = att_mtu_updated,
};

static int init(void)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		printk("Bluetooth enable failed (err %d)\n", err);
		return err;
	}

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		streams[i].ops = &stream_ops;
	}

	bt_gatt_cb_register(&gatt_callbacks);


	k_work_init_delayable(&audio_send_work, audio_timer_timeout);

	return 0;
}



static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];

	/* We're only interested in connectable events 	*/
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	(void)bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	//printk("Device found: %s (RSSI %d)\n", addr_str, rssi);

	/* connect only to devices in close proximity */
	if (rssi < -90) {
		return;
	}

	bt_data_parse(ad, check_audio_support_and_connect, (void *)addr);
}

static void start_scan(void)
{
	int err;

	/* This demo doesn't require active scan */
	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found);
	if (err != 0) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return;
	}

	LOG_INF("Scanning successfully started");
}





static int create_group(void)
{
	struct bt_audio_unicast_group_param params[ARRAY_SIZE(streams)];
	int err;

	for (size_t i = 0U; i < configured_stream_count; i++) {
		params[i].stream = &streams[i];
		params[i].qos = &codec_configuration.qos;
		params[i].dir = BT_AUDIO_DIR_SINK;
	}

	err = bt_audio_unicast_group_create(params, configured_stream_count,
					    &unicast_group);
	if (err != 0) {
		printk("Could not create unicast group (err %d)\n", err);
		return err;
	}

	return 0;
}

void main(void)
{
	int err;

	printk("Initializing\n");
	err = init();
	if (err != 0) {
		return;
	}
	printk("Initialized\n");
	create_group();
	LOG_INF("Waiting for connection");

	start_scan();
	while (1) {
		k_sleep(K_MSEC(1000));
	}
}
