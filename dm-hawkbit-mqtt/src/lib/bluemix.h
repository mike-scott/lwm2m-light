/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef	__FOTA_BLUEMIX_H__
#define	__FOTA_BLUEMIX_H__

#include <net/mqtt.h>
#include <kernel.h>
#include <toolchain.h>

#define BLUEMIX_PORT	1883

#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS		K_MSEC(500)
#define APP_TX_RX_TIMEOUT       K_MSEC(300)

#define CONFIG_FOTA_BLUEMIX_DEVICE_TYPE	CONFIG_BOARD

struct bluemix_ctx;

/**
 * Return codes for Bluemix user callbacks.
 */
enum {
	/** Continue normally. */
	BLUEMIX_CB_OK = 0,
	/** Re-establish Bluemix connection, then proceed. */
	BLUEMIX_CB_RECONNECT = -1,
	/** Halt background thread. */
	BLUEMIX_CB_HALT = -2,
};

/**
 * User callback from Bluemix thread.
 *
 * The return value must be one of the BLUEMIX_CB_XXX values.
 */
typedef int (*bluemix_cb)(struct bluemix_ctx *ctx, void *data);

/**
 * @brief bluemix_ctx	Context structure for Bluemix
 *
 * All of this state is internal. Clients should interact using the
 * API functions defined below only.
 */
struct bluemix_ctx {
	struct mqtt_connect_msg connect_msg;
	struct mqtt_publish_msg pub_msg;

	struct mqtt_ctx mqtt_ctx;

	u8_t bm_id[30];		/* Bluemix device ID */
	u8_t bm_topic[255];		/* Buffer for topic names */
	u8_t bm_message[1024];		/* Buffer for message data */
	u8_t bm_auth_token[20];	/* Bluemix authentication token */
	u8_t client_id[50];		/* MQTT client ID */

	/* For waiting for a callback from the MQTT stack. */
	struct k_sem wait_sem;
};

/**
 * @brief Start a background Bluemix thread
 *
 * The background thread will periodically invoke the user callback,
 * as long as a connection is established. It is safe to publish MQTT
 * messages when this callback is invoked; for example, it's safe to
 * call bluemix_pub_status_json(). The callback will not be invoked if
 * no connection is available.
 *
 * @param cb        User callback; this may be NULL if unused.
 * @param cb_data   Passed to cb along with a bluemix context.
 * @return Zero if the thread started successfully, negative errno
 *         on error.
 */
int bluemix_init(bluemix_cb cb, void *cb_data);

/**
 * @brief Publish device status reading in JSON format.
 *
 * The format string and arguments should correspond to the JSON
 * value of the "d" field in a Bluemix JSON status publication.
 *
 * For example, to publish an "mcutemp" field with value 23, you could
 * write:
 *
 *    bluemix_pub_status_json(ctx, "{\"mcutemp\":%d}", 23);
 *
 * Do *NOT* write:
 *
 *    bluemix_pub_status_json(ctx, "{ \"d\": { \"mcutemp\":%d } }", 23);
 *
 * @param ctx Bluemix context to publish status for.
 * @param fmt printf()-like format for JSON sub-string to publish as
 *            status message's data field ("d"). Remaining arguments
 *            are used to build the JSON string to publish with fmt.
 * @return 0 on success, negative errno on failure.
 */
int __printf_like(2, 3) bluemix_pub_status_json(struct bluemix_ctx *ctx,
						const char *fmt, ...);

#endif	/* __FOTA_BLUEMIX_H__ */
