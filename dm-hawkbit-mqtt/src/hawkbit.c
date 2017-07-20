/*
 * Copyright (c) 2016 Linaro Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <misc/byteorder.h>
#include <flash.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>

#include <soc.h>
#include <net/http_parser.h>

#include "ota_debug.h"
#include "jsmn.h"
#include "hawkbit.h"
#include "tcp.h"
#include "boot_utils.h"
#include "device.h"

uint8_t tcp_buf[TCP_RECV_BUF_SIZE];

struct json_data_t {
	char *data;
	size_t len;
};

struct http_download_t {
	size_t header_size;
	size_t content_length;
};

typedef enum {
	HAWKBIT_UPDATE_SKIP = 0,
	HAWKBIT_UPDATE_ATTEMPT,
	HAWKBIT_UPDATE_FORCED
} hawkbit_update_action_t;

typedef enum {
	HAWKBIT_RESULT_SUCCESS = 0,
	HAWKBIT_RESULT_FAILURE,
	HAWKBIT_RESULT_NONE,
} hawkbit_result_status_t;

typedef enum {
	HAWKBIT_EXEC_CLOSED = 0,
	HAWKBIT_EXEC_PROCEEDING,
	HAWKBIT_EXEC_CANCELED,
	HAWKBIT_EXEC_SCHEDULED,
	HAWKBIT_EXEC_REJECTED,
	HAWKBIT_EXEC_RESUMED,
} hawkbit_exec_status_t;

/* Utils */
static int atoi_n(const char *s, int len)
{
        int i, val = 0;

	for (i = 0; i < len; i++) {
		if (*s < '0' || *s > '9')
			return val;
		val = (val * 10) + (*s - '0');
		s++;
	}

        return val;
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
	if (tok->type == JSMN_STRING &&
		(int) strlen(s) == tok->end - tok->start &&
		strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 1;
	}
	return 0;
}

static int json_parser(struct json_data_t *json, jsmn_parser *parser,
		       jsmntok_t *tks, uint16_t num_tokens)
{
	int ret = 0;

	OTA_DBG("JSON: max tokens supported %d\n", num_tokens);

	jsmn_init(parser);
	ret = jsmn_parse(parser, json->data, json->len, tks, num_tokens);
	if (ret < 0) {
		switch (ret) {
		case JSMN_ERROR_NOMEM:
			OTA_ERR("JSON: Not enough tokens\n");
			break;
		case JSMN_ERROR_INVAL:
			OTA_ERR("JSON: Invalid character found\n");
			break;
		case JSMN_ERROR_PART:
			OTA_ERR("JSON: Incomplete JSON\n");
			break;
		}
		return ret;
	} else if (ret == 0 || tks[0].type != JSMN_OBJECT) {
		OTA_ERR("JSON: First token is not an object\n");
		return 0;
	}

	OTA_DBG("JSON: %d tokens found\n", ret);

	return ret;
}

static int hawkbit_time2sec(const char *s)
{
        int sec = 0;

	/* Time: HH:MM:SS */
	sec = atoi_n(s, 2) * 60 * 60;
	sec += atoi_n(s + 3, 2) * 60;
	sec += atoi_n(s + 6, 2);

	if (sec < 0) {
		return -1;
	} else {
		return sec;
	}
}

/* HTTP parser callbacks */
static int handle_headers_complete(struct http_parser *parser)
{
	/* Check if our buffer is enough for a valid body */
	if (parser->nread + parser->content_length >= TCP_RECV_BUF_SIZE) {
		OTA_ERR("header + body larger than buffer %d\n", TCP_RECV_BUF_SIZE);
		return -1;
	}
	return 0;
}

static int handle_http_body(struct http_parser* parser, const char *at, size_t len)
{
	struct json_data_t *json = parser->data;

	if (json) {
		json->data = (char *) at;
		json->len = len;
	}

	return 0;
}

static int handle_headers_complete_download(struct http_parser *parser)
{
	struct http_download_t *http_data = parser->data;

	if (parser->status_code == 200) {
		http_data->header_size = parser->nread;
		http_data->content_length = parser->content_length;
	}

	return 1;
}

static int hawkbit_install_update(uint8_t *tcp_buffer, size_t size,
				  const char *download_http,
				  size_t file_size)
{
	struct http_parser_settings http_settings;
	struct http_download_t http_data = { 0 };
	struct http_parser parser;
	int downloaded_size = 0;
	int ret, len;

	if (!tcp_buffer || !download_http || !file_size) {
		return -EINVAL;
	}

	if (flash_erase(flash_dev, FLASH_BANK1_OFFSET, FLASH_BANK_SIZE) != 0) {
		OTA_ERR("Failed to erase flash at offset %x, size %d\n",
					FLASH_BANK1_OFFSET, FLASH_BANK_SIZE);
		return -EIO;
	}

	http_parser_init(&parser, HTTP_RESPONSE);
	http_parser_settings_init(&http_settings);
	http_settings.on_headers_complete = handle_headers_complete_download;
	parser.data = &http_data;

	OTA_INFO("Starting the download and flash process\n");

	/* Here we just proceed with a normal HTTP Download process */
	memset(tcp_buffer, 0, size);
	snprintf(tcp_buffer, size, "GET %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Connection: close\r\n"
				"\r\n",
				download_http, HAWKBIT_HOST);

	ret = tcp_send((const char *) tcp_buffer, strlen(tcp_buffer));
	if (ret < 0) {
		OTA_ERR("Failed to send buffer, err %d\n", ret);
		return ret;
	}

	/* Receive is special for download, since it writes to flash */
	memset(tcp_buf, 0, TCP_RECV_BUF_SIZE);
	len = tcp_recv((char *) tcp_buffer, size, TCP_RX_TIMEOUT);
	if (len <= 0) {
		OTA_ERR("Unable to start the download process (ret=%d)\n", len);
		return -1;
	}

	/* Parse the HTTP headers available from the first buffer */
	http_parser_execute(&parser, &http_settings,
				(const char *) tcp_buffer, size);
	if (parser.status_code != 200) {
		OTA_ERR("Download: http error %d\n", parser.status_code);
		return -1;
	}
	if (http_data.content_length != file_size) {
		OTA_ERR("Download: file size not the same as reported, "
				"found %d, expecting %d\n",
				http_data.content_length, file_size);
		return -1;
	}

	/* Everything looks good, so fetch and flash */
	flash_write_protection_set(flash_dev, false);

	if (len > http_data.header_size) {
		len -= http_data.header_size;
		flash_write(flash_dev, FLASH_BANK1_OFFSET,
				tcp_buffer + http_data.header_size,
				len);
		downloaded_size += len;
	}

	while (true) {
		len = tcp_recv((char *) tcp_buffer, size, TCP_RX_TIMEOUT);
		if (len <= 0) {
			if (len < 0) {
				OTA_ERR("Download error recieved: %d", len);
				return -1;
			}
			break;
		}
		flash_write(flash_dev, FLASH_BANK1_OFFSET + downloaded_size,
							tcp_buffer, len);
		downloaded_size += len;
	}
	flash_write_protection_set(flash_dev, true);

	if (downloaded_size != file_size) {
		OTA_ERR("Download: downloaded image size mismatch, "
				"downloaded %d, expecting %d\n",
				downloaded_size, file_size);
		return -1;
	}

	OTA_INFO("Download: downloaded bytes %d\n", downloaded_size);

	return 0;
}

static int hawkbit_query(uint8_t *tcp_buffer, size_t size,
			 struct json_data_t *json)
{
	struct http_parser_settings http_settings;
	struct http_parser parser;
	int ret;

	if (!tcp_buffer) {
		return -EINVAL;
	}

	OTA_DBG("\n\n%s\n", tcp_buffer);

	ret = tcp_send((const char *) tcp_buffer, strlen(tcp_buffer));
	if (ret < 0) {
		OTA_ERR("Failed to send buffer, err %d\n", ret);
		return ret;
	}
	ret = tcp_recv((char *) tcp_buffer, size, K_FOREVER);
	if (ret <= 0) {
		OTA_ERR("No received data (ret=%d)\n", ret);
		return -1;
	}

	http_parser_init(&parser, HTTP_RESPONSE);
	http_parser_settings_init(&http_settings);
	http_settings.on_body = handle_http_body;
	http_settings.on_headers_complete = handle_headers_complete;
	parser.data = json;

	http_parser_execute(&parser, &http_settings,
				(const char *) tcp_buffer, ret);
	if (parser.status_code != 200) {
		OTA_ERR("Invalid HTTP status code %d\n",
						parser.status_code);
		return -1;
	}

	if (json) {
		if (json->data == NULL) {
			OTA_ERR("JSON data not found\n");
			return -1;
		}
		/* FIXME: Each poll needs a new connection, this saves
		 * us from using content from a previous package.
		 */
		json->data[json->len] = '\0';
		OTA_DBG("JSON DATA:\n%s\n", json->data);
	}

	OTA_DBG("Hawkbit query completed\n");

	return 0;
}

static int hawkbit_report_config_data(uint8_t *tcp_buffer, size_t size)
{
	char *helper;

	OTA_INFO("Reporting target config data to Hawkbit\n");

	/* Use half for the header and half for the json content */
	memset(tcp_buffer, 0, size);
	helper = tcp_buffer + size / 2;

	/* Start with JSON as we need to calculate the content length */
	snprintf(helper, size / 2, "{"
			"\"data\":{"
				"\"board\":\"%s\","
				"\"serial\":\"%x\"},"
			"\"status\":{"
				"\"result\":{\"finished\":\"success\"},"
				"\"execution\":\"closed\"}"
			"}", product_id.name, product_id.number);

	/* size / 2 should be enough for the header */
	snprintf(tcp_buffer, size, "PUT %s/%s-%x/configData HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n%s", HAWKBIT_JSON_URL,
			product_id.name, product_id.number,
			HAWKBIT_HOST, strlen(helper), helper);

	if (hawkbit_query(tcp_buffer, size, NULL) < 0) {
		OTA_ERR("Error when reporting config data to Hawkbit\n");
		return -1;
	}

	return 0;
}

static int hawkbit_report_update_status(int acid,
					uint8_t *tcp_buffer, size_t size,
					hawkbit_result_status_t status,
					hawkbit_exec_status_t exec)
{
	char finished[8];	/* 'success', 'failure', 'none' */
	char execution[11];
	char *helper;

	switch (status) {
	case HAWKBIT_RESULT_SUCCESS:
		snprintf(finished, sizeof(finished), "success");
		break;
	case HAWKBIT_RESULT_FAILURE:
		snprintf(finished, sizeof(finished), "failure");
		break;
	case HAWKBIT_RESULT_NONE:
		snprintf(finished, sizeof(finished), "none");
		break;
	}

	/* 'closed', 'proceeding', 'canceled', 'scheduled',
	 * 'rejected', 'resumed'
	 */
	switch (exec) {
	case HAWKBIT_EXEC_CLOSED:
		snprintf(execution, sizeof(execution), "closed");
		break;
	case HAWKBIT_EXEC_PROCEEDING:
		snprintf(execution, sizeof(execution), "proceeding");
		break;
	case HAWKBIT_EXEC_CANCELED:
		snprintf(execution, sizeof(execution), "canceled");
		break;
	case HAWKBIT_EXEC_SCHEDULED:
		snprintf(execution, sizeof(execution), "scheduled");
		break;
	case HAWKBIT_EXEC_REJECTED:
		snprintf(execution, sizeof(execution), "rejected");
		break;
	case HAWKBIT_EXEC_RESUMED:
		snprintf(execution, sizeof(execution), "resumed");
		break;
	}

	OTA_INFO("Reporting action ID feedback: %s\n", finished);

	/* Use half for the header and half for the json content */
	memset(tcp_buffer, 0, size);
	helper = tcp_buffer + size / 2;

	/* Start with JSON as we need to calculate the content length */
	snprintf(helper, size / 2, "{"
			"\"id\":\"%d\","
			"\"status\":{"
				"\"result\":{\"finished\":\"%s\"},"
				"\"execution\":\"%s\"}"
			"}", acid, finished, execution);

	/* size / 2 should be enough for the header */
	snprintf(tcp_buffer, size,
			"POST %s/%s-%x/deploymentBase/%d/feedback HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n%s", HAWKBIT_JSON_URL,
			product_id.name, product_id.number, acid,
			HAWKBIT_HOST, strlen(helper), helper);

	if (hawkbit_query(tcp_buffer, size, NULL) < 0) {
		OTA_ERR("Error when reporting acId feedback to Hawkbit\n");
		return -1;
	}

	return 0;
}

int hawkbit_ddi_poll(void)
{
	jsmn_parser jsmnp;
	jsmntok_t jtks[60];	/* Enough for one artifact per SM */
	int i, ret, len, ntk;
	static hawkbit_update_action_t hawkbit_update_action;
	static int hawkbit_acid = 0;
	struct json_data_t json = { NULL, 0 };
	char deployment_base[40];	/* TODO: Find a better value */
	char download_http[200];	/* TODO: Find a better value */
	bool update_config_data = false;
	int file_size = 0;
	char *helper;

	OTA_DBG("Polling target data from Hawkbit\n");

	memset(tcp_buf, 0, TCP_RECV_BUF_SIZE);
	snprintf(tcp_buf, TCP_RECV_BUF_SIZE, "GET %s/%s-%x HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Connection: close\r\n"
				"\r\n",
				HAWKBIT_JSON_URL,
				product_id.name, product_id.number,
				HAWKBIT_HOST);

	ret = hawkbit_query(tcp_buf, TCP_RECV_BUF_SIZE, &json);
	if (ret < 0) {
		OTA_ERR("Error when polling from Hawkbit\n");
		return ret;
	}

	ntk = json_parser(&json, &jsmnp, jtks,
			sizeof(jtks) / sizeof(jsmntok_t));
	if (ntk <= 0) {
		OTA_ERR("Error when parsing JSON from target\n");
		return -1;
	}

	/* Hawkbit DDI v1 targetid */
	memset(deployment_base, 0, sizeof(deployment_base));
	/* TODO: Implement cancel action logic */
	for (i = 1; i < ntk - 1; i++) {
		/* config -> polling -> sleep */
		if (jsoneq(json.data, &jtks[i], "config") &&
				(i + 5 < ntk) &&
				(jsoneq(json.data, &jtks[i + 4], "sleep"))) {
			/* Sleep format: HH:MM:SS */
			if (jtks[i + 5].end - jtks[i + 5].start > 8) {
				OTA_ERR("Invalid poll sleep string\n");
				continue;
			}
			len = hawkbit_time2sec(json.data + jtks[i + 5].start);
			if (len > 0 &&
				poll_sleep != K_SECONDS(len)) {
				OTA_INFO("New poll sleep %d seconds\n", len);
				poll_sleep = K_SECONDS(len);
				i += 5;
			}
		} else if (jsoneq(json.data, &jtks[i], "deploymentBase") &&
				(i + 3 < ntk) &&
				(jsoneq(json.data, &jtks[i + 2], "href"))) {
			/* Just extract the deploymentBase piece */
			helper = strstr(json.data + jtks[i + 3].start,
							"deploymentBase/");
			if (helper == NULL ||
					helper > json.data + jtks[i + 3].end) {
				continue;
			}
			len = json.data + jtks[i + 3].end - helper;
			memcpy(&deployment_base, helper, len);
			deployment_base[len] = '\0';
			OTA_DBG("Deployment base %s\n", deployment_base);
			i += 3;
		} else if (jsoneq(json.data, &jtks[i], "configData") &&
				(i + 3 < ntk) &&
				(jsoneq(json.data, &jtks[i + 2], "href"))) {
			update_config_data = true;
			i += 3;
		}
	}

	/* Update config data if the server asked for it */
	if (update_config_data) {
		hawkbit_report_config_data(tcp_buf, TCP_RECV_BUF_SIZE);
	}

	if (strlen(deployment_base) == 0) {
		OTA_DBG("No deployment base found, no actions to take\n");
		return 0;
	}

	/* Hawkbit DDI v1 deploymentBase */
	memset(tcp_buf, 0, TCP_RECV_BUF_SIZE);
	snprintf(tcp_buf, TCP_RECV_BUF_SIZE, "GET %s/%s-%x/%s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Connection: close\r\n"
				"\r\n",
				HAWKBIT_JSON_URL,
				product_id.name, product_id.number,
				deployment_base, HAWKBIT_HOST);

	memset(&json, 0, sizeof(struct json_data_t));
	if (hawkbit_query(tcp_buf, TCP_RECV_BUF_SIZE, &json) < 0) {
		OTA_ERR("Error when querying from Hawkbit\n");
		return -1;
	}

	/* We have our own limit here, which is directly affected by the
	 * number of artifacts available as part of the software module
	 * assigned, so needs coordination with the deployment process.
	 */
	ntk = json_parser(&json, &jsmnp, jtks,
			sizeof(jtks) / sizeof(jsmntok_t));
	if (ntk <= 0) {
		OTA_ERR("Error when parsing JSON from deploymentBase\n");
		return -1;
	}

	ret = 0;
	memset(download_http, 0, sizeof(download_http));
	for (i = 1; i < ntk - 1; i++) {
		if (jsoneq(json.data, &jtks[i], "id")) {
			/* id -> id */
			hawkbit_acid = atoi_n(json.data + jtks[i + 1].start,
					jtks[i + 1].end - jtks[i + 1].start);
			OTA_DBG("Hawkbit ACTION ID %d\n", hawkbit_acid);
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "deployment")) {
			/* deployment -> download, update or chunks */
			if (i + 5 >= ntk) {
				continue;
			}
			/* Check just the first 2 keys, since chunk is [] */
			if (jsoneq(json.data, &jtks[i + 2], "update")) {
				i += 3;
			} else if (jsoneq(json.data, &jtks[i + 4], "update")) {
				i += 5;
			} else {
				continue;
			}
			/* Now just find the update action */
			if (jsoneq(json.data, &jtks[i], "skip")) {
				hawkbit_update_action = HAWKBIT_UPDATE_SKIP;
				OTA_DBG("Hawkbit update action: SKIP\n");
			} else if (jsoneq(json.data, &jtks[i], "attempt")) {
				hawkbit_update_action = HAWKBIT_UPDATE_ATTEMPT;
				OTA_DBG("Hawkbit update action: ATTEMPT\n");
			} else if (jsoneq(json.data, &jtks[i], "forced")) {
				hawkbit_update_action = HAWKBIT_UPDATE_FORCED;
				OTA_DBG("Hawkbit update action: FORCED\n");
			}
		} else if (jsoneq(json.data, &jtks[i], "chunks")) {
			if (jtks[i + 1].type != JSMN_ARRAY) {
				continue;
			}
			if (jtks[i + 1].size != 1) {
				OTA_ERR("Only one chunk is supported, %d\n",
							jtks[i + 1].size);
				ret = -1;
				break;
			}
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "part")) {
			if (!jsoneq(json.data, &jtks[i + 1], "os")) {
				OTA_ERR("Only part 'os' is supported\n");
				ret = -1;
				break;
			}
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "size")) {
			file_size = atoi_n(json.data + jtks[i + 1].start,
					jtks[i + 1].end - jtks[i + 1].start);
			OTA_DBG("Artifact file size: %d\n", file_size);
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "download-http")) {
			/* We just support DEFAULT tenant on the same server */
			if (i + 3 >= ntk ||
				!jsoneq(json.data, &jtks[i + 2], "href")) {
				OTA_ERR("No href entry for download-http\n");
				ret = -1;
				continue;
			}
			/* Extracting everying after server address */
			helper = strstr(json.data + jtks[i + 3].start,
						"/DEFAULT/controller/v1");
			if (helper == NULL ||
					helper > json.data + jtks[i + 3].end) {
				continue;
			}
			len = json.data + jtks[i + 3].end - helper;
			if (len >= sizeof(download_http)) {
				OTA_ERR("Download HREF too big (%d)\n", len);
				ret = - 1;
				continue;
			}
			memcpy(&download_http, helper, len);
			download_http[len] = '\0';
			OTA_DBG("Artifact address: %s\n", download_http);
			i += 3;
		}
	}

	if (boot_acid_read(BOOT_ACID_CURRENT) == hawkbit_acid) {
		/* We are coming from a successful flash, update the server */
		hawkbit_report_update_status(hawkbit_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_SUCCESS,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	} else if (boot_acid_read(BOOT_ACID_UPDATE) == hawkbit_acid) {
		/* There was already an atempt, so announce a failure */
		hawkbit_report_update_status(hawkbit_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	}

	/* Perform the action */
	if (strlen(download_http) == 0) {
		OTA_DBG("No download http address found, no action\n");
		return 0;
	}
	/* Error detected when parsing the SM */
	if (ret == -1) {
		hawkbit_report_update_status(hawkbit_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return -1;
	}
	if (file_size > FLASH_BANK_SIZE) {
		OTA_ERR("Artifact file size too big (%d)\n", file_size);
		hawkbit_report_update_status(hawkbit_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return -1;
	}

	/* Here we should have everything we need to apply the action */
	OTA_INFO("Valid action ID %d found, proceeding with the update\n",
					hawkbit_acid);
	hawkbit_report_update_status(hawkbit_acid,
				     tcp_buf, TCP_RECV_BUF_SIZE,
				     HAWKBIT_RESULT_SUCCESS,
				     HAWKBIT_EXEC_PROCEEDING);
	ret = hawkbit_install_update(tcp_buf, TCP_RECV_BUF_SIZE, download_http, file_size);
	if (ret != 0) {
		OTA_ERR("Failed to install the update for action ID %d\n",
					hawkbit_acid);
		return -1;
	}

	boot_trigger_ota();
	boot_acid_update(BOOT_ACID_UPDATE, hawkbit_acid);
	OTA_INFO("Image id %d flashed successfuly, rebooting now\n",
					hawkbit_acid);

	/* Reboot and let the bootloader take care of the swap process */
	sys_arch_reboot(0);

	return 0;
}
