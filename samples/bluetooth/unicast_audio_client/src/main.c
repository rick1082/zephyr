/*
 * Copyright (c) 2021-2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>
#include <sys/printk.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/audio/audio.h>
#include <sys/byteorder.h>
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
static struct bt_audio_stream audio_stream[2];
static struct bt_audio_unicast_group *unicast_group;
static struct bt_codec *remote_codec_capabilities[CONFIG_BT_AUDIO_UNICAST_CLIENT_PAC_COUNT];
static struct bt_audio_ep *sinks[CONFIG_BT_AUDIO_UNICAST_CLIENT_ASE_SNK_COUNT];
NET_BUF_POOL_FIXED_DEFINE(tx_pool, 1, CONFIG_BT_ISO_TX_MTU + BT_ISO_CHAN_SEND_RESERVE, 8, NULL);

/* Select a codec configuration to apply that is mandatory to support by both client and server.
 * Allows this sample application to work without logic to parse the codec capabilities of the
 * server and selection of an appropriate codec configuration.
 */
static struct bt_audio_lc3_preset codec_configuration = BT_AUDIO_LC3_UNICAST_PRESET_16_2_1;

static int discover_sink(struct bt_conn *conn);

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
	static uint8_t buf_data[CONFIG_BT_ISO_TX_MTU];
	static bool data_initialized;
	struct net_buf *buf;
	static size_t len_to_send = 1;

	if (!data_initialized) {
		/* TODO: Actually encode some audio data */
		for (int i = 0; i < ARRAY_SIZE(buf_data); i++) {
			buf_data[i] = (uint8_t)i;
		}

		data_initialized = true;
	}

	buf = net_buf_alloc(&tx_pool, K_FOREVER);
	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);

	net_buf_add_mem(buf, buf_data, len_to_send);

	ret = bt_audio_stream_send(&audio_stream, buf);
	if (ret < 0) {
		LOG_ERR("Failed to send audio data (%d)", ret);
		net_buf_unref(buf);
	} else {
		LOG_INF("Sending mock data with len %zu", len_to_send);
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
	printk("codec 0x%02x cid 0x%04x vid 0x%04x count %u\n", codec->id, codec->cid, codec->vid,
	       codec->data_count);

	for (size_t i = 0; i < codec->data_count; i++) {
		printk("data #%zu: type 0x%02x len %u\n", i, codec->data[i].data.type,
		       codec->data[i].data.data_len);
		print_hex(codec->data[i].data.data,
			  codec->data[i].data.data_len - sizeof(codec->data[i].data.type));
		printk("\n");
	}

	for (size_t i = 0; i < codec->meta_count; i++) {
		printk("meta #%zu: type 0x%02x len %u\n", i, codec->meta[i].data.type,
		       codec->meta[i].data.data_len);
		print_hex(codec->meta[i].data.data,
			  codec->meta[i].data.data_len - sizeof(codec->meta[i].data.type));
		printk("\n");
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

#define BT_LE_CONN_PARAM_TWS BT_LE_CONN_PARAM(100, \
						  100, \
						  0, 400)

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

			ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
						BT_LE_CONN_PARAM_TWS, &conn);
			if (ret) {
				LOG_ERR("Could not init connection");
				return ret;
			}
			headset_conn[HEADSET_L] = conn;
			return 0; //stop parsing
		} else if ((data->data_len == DEVICE_NAME_PEER_R_LEN) &&
			   (strncmp(DEVICE_NAME_PEER_R, data->data, DEVICE_NAME_PEER_R_LEN) == 0)) {
			bt_le_scan_stop();

			ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
						BT_LE_CONN_PARAM_TWS, &conn);
			if (ret) {
				LOG_ERR("Could not init connection");
				return ret;
			}
			headset_conn[HEADSET_R] = conn;
			return 0; //stop parsing
		}
	}

	return true;
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

static void stream_configured(struct bt_audio_stream *stream, const struct bt_codec_qos_pref *pref)
{
	int err;
	LOG_INF("Audio Stream %p configured, conn %p", (void *)stream, (void *)stream->conn);
	err = bt_audio_stream_qos(stream->conn, unicast_group, &codec_configuration.qos);
	if (err != 0) {
		LOG_ERR("Unable to setup QoS for conn %p: %d", (void *)stream->conn, err);
	}else {
		LOG_INF("qos set");
	}
}

static void stream_qos_set(struct bt_audio_stream *stream)
{
	int err;

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
	err = bt_audio_stream_start(stream);
	if (err != 0) {
		LOG_ERR("Unable to start stream: %d", err);
	}
}

static void stream_started(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p started", stream);
	/* Start send timer */
	//k_work_schedule(&audio_send_work, K_MSEC(0));
}

static void stream_metadata_updated(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p metadata updated", (void *)stream);
}

static void stream_disabled(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p disabled", (void *)stream);
}

static void stream_stopped(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p stopped", (void *)stream);

	/* Stop send timer */
	k_work_cancel_delayable(&audio_send_work);
}

static void stream_released(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p released", (void *)stream);
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
};

static void add_remote_sink(struct bt_audio_ep *ep, uint8_t index)
{
	LOG_INF("Sink #%u: ep %p", index, (void *)ep);
	sinks[index] = ep;
}

static void add_remote_codec(struct bt_codec *codec_capabilities, int index, uint8_t type)
{
	//printk("#%u: codec %p type 0x%02x\n", index, codec_capabilities, type);

	//print_codec_capabilities(codec_capabilities);

	if (type != BT_AUDIO_SINK && type != BT_AUDIO_SOURCE) {
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
		add_remote_codec(codec, params->num_caps, params->type);
		return;
	}

	if (ep != NULL) {
		if (params->type == BT_AUDIO_SINK) {
			//add_remote_sink(ep, params->num_eps);
			add_remote_sink(ep, ep_index);
		} else {
			LOG_INF("Invalid param type: %u", params->type);
		}

		return;
	}

	LOG_INF("Discover complete: err %d", params->err);

	(void)memset(params, 0, sizeof(*params));

	if (conn == headset_conn[HEADSET_L]) {
		err = bt_audio_stream_config(conn, &audio_stream[HEADSET_L], sinks[HEADSET_L], &codec_configuration.codec);
		LOG_INF("configure stream for sink[HEADSET_L], err = %d", err);
	} else if (conn == headset_conn[HEADSET_R]) {
		err = bt_audio_stream_config(conn, &audio_stream[HEADSET_R], sinks[HEADSET_R], &codec_configuration.codec);
		LOG_INF("configure stream for sink[HEADSET_R], err = %d", err);
	}

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
		k_sleep(K_MSEC(500));
		start_scan();
		return;
	}

	if (conn != headset_conn[HEADSET_L] && conn != headset_conn[HEADSET_R]) {
		return;
	}
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err != 0) {
		LOG_ERR("SMP failed, err = %d", err);
	}
	LOG_INF("Connected: %s", addr);
	if (!all_headset_connected()) {
		k_sleep(K_MSEC(500));
		start_scan();
	} else {
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

static bool conn_param_req_cb(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	/* Connection between two nRF5340 Audio DKs are fixed 
	if ((param->interval_min != CONFIG_BLE_ACL_CONN_INTERVAL) ||
	    (param->interval_max != CONFIG_BLE_ACL_CONN_INTERVAL)) {
		return false;
	}
	*/
	return true;
}

static void conn_param_updated_cb(struct bt_conn *conn, uint16_t interval, uint16_t latency,
				  uint16_t timeout)
{
}

#if (CONFIG_BT_SMP)
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
#endif /* (CONFIG_BT_SMP) */

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = conn_param_req_cb,
	.le_param_updated = conn_param_updated_cb,
#if (CONFIG_BT_SMP)
	.security_changed = security_changed_cb,
#endif /* (CONFIG_BT_SMP) */
};

static int init(void)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth enable failed (err %d)", err);
		return err;
	}

	audio_stream[0].ops = &stream_ops;
	audio_stream[1].ops = &stream_ops;
	err = bt_audio_unicast_group_create(audio_stream, 2, &unicast_group);
	if (err != 0) {
		LOG_ERR("bt_audio_unicast_group_create failed, err = %d", err);
	}else {
		LOG_INF("bt_audio_unicast_group_create finished");
	}

	k_work_init_delayable(&audio_send_work, audio_timer_timeout);

	return 0;
}

static int discover_sink(struct bt_conn *conn)
{
	int err = 0;
	static struct bt_audio_discover_params params_l;
	static struct bt_audio_discover_params params_r;

	params_l.func = discover_sink_cb;
	params_l.type = BT_AUDIO_SINK;
	params_r.func = discover_sink_cb;
	params_r.type = BT_AUDIO_SINK;

	if (conn == headset_conn[HEADSET_L]) {
		err = bt_audio_discover(conn, &params_l);
	} else if (conn == headset_conn[HEADSET_R]) {
		err = bt_audio_discover(conn, &params_r);	
	}
	if (err != 0) {
		LOG_ERR("Failed to discover sink: %d", err);
		return err;
	}

	return 0;
}

void main(void)
{
	int err;

	LOG_INF("Initializing");
	err = init();
	if (err != 0) {
		return;
	}
	LOG_INF("Initialized");

	LOG_INF("Waiting for connection");

	start_scan();

	while (1) {
		k_sleep(K_MSEC(1000));
	}
}
