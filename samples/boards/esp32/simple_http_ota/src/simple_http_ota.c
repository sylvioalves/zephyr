/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr.h>
#include <net/net_ip.h>
#include <net/socket.h>
#include <net/net_mgmt.h>
#include <drivers/flash.h>
#include <storage/flash_map.h>
#include <dfu/mcuboot.h>
#include <dfu/flash_img.h>
#include <net/http_client.h>
#include <net/http_parser_url.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(simple_http_ota);

#define SLOT_SIZE FLASH_AREA_SIZE(image_1)
#define MAX_RECV_BUF_LEN 512

#define HTTP_TIMEOUT (CONFIG_SIMPLE_HTTP_OTA_DOWNLOAD_TIMEOUT * MSEC_PER_SEC)

/* Copy URL locally so we can parse it */
static char download_url[] = CONFIG_SIMPLE_HTTP_OTA_FILE_URL;

enum simple_http_ota_response {
	SIMPLE_HTTP_OTA_OK,
	SIMPLE_HTTP_OTA_ERROR,
};

static struct simple_http_ota_context {
	int sock;
	struct flash_img_context flash_ctx;
	uint8_t recv_buf[MAX_RECV_BUF_LEN];
	enum simple_http_ota_response status;
	size_t content_length;
} ota_context;

static int setup_socket(sa_family_t family, const char *server, int port,
			int *sock, struct sockaddr *addr, socklen_t addr_len)
{
	int ret = 0;

	memset(addr, 0, addr_len);

	if (family == AF_INET) {
		net_sin(addr)->sin_family = AF_INET;
		net_sin(addr)->sin_port = htons(port);
		inet_pton(family, server, &net_sin(addr)->sin_addr);
	}

	*sock = socket(family, SOCK_STREAM, IPPROTO_TCP);

	if (*sock < 0) {
		LOG_ERR("Failed to create HTTP socket (%d)", -errno);
	}

	return ret;
}

static int connect_socket(sa_family_t family, const char *server, int port,
			  int *sock, struct sockaddr *addr, socklen_t addr_len)
{
	int ret;

	ret = setup_socket(family, server, port, sock, addr, addr_len);
	if (ret < 0 || *sock < 0) {
		LOG_ERR("could create socket (%d)", ret);
		return -1;
	}

	ret = connect(*sock, addr, addr_len);
	if (ret < 0) {
		LOG_ERR("Cannot connect to remote (%d)", -errno);
		ret = -errno;
	}

	return ret;
}

static void response_cb(struct http_response *rsp,
			enum http_final_call final_data,
			void *user_data)
{
	static size_t body_len;
	uint8_t *body_data = NULL;
	int ret = 0;

	/* check if file exists and its size */
	if (rsp->http_status_code != 200) {
		ota_context.status = SIMPLE_HTTP_OTA_ERROR;
		LOG_ERR("Could not download file: HTTP error %d", rsp->http_status_code);
		return;
	}

	if (rsp->content_length > SLOT_SIZE) {
		ota_context.status = SIMPLE_HTTP_OTA_ERROR;
		LOG_ERR("File size too big (got %d, max is %d)",
				rsp->content_length, SLOT_SIZE);
		return;
	}

	if (ota_context.content_length == 0) {
		body_data = rsp->body_frag_start;
		body_len = rsp->data_len;
		body_len -= (rsp->body_frag_start - rsp->recv_buf);

		ota_context.content_length = rsp->content_length;
	} else {
		body_data = rsp->body_frag_start;
		body_len = rsp->data_len;
	}

	if ((rsp->body_found == 1) && (body_data == NULL)) {
		body_data = rsp->recv_buf;
		body_len = rsp->data_len;
	}

	if (body_data != NULL) {
		ret = flash_img_buffered_write(&ota_context.flash_ctx, body_data, body_len, false);
		if (ret < 0) {
			ota_context.status = SIMPLE_HTTP_OTA_ERROR;
			LOG_ERR("Flash write error %d", ret);
			return;
		}
	}
}

int simple_http_ota_run(void)
{
	struct sockaddr_in addr4;
	int ret = 0;
	char uri[64];
	char host[64];
	int port = 80;

	struct http_parser_url parser;
	uint16_t off, len;

	flash_img_init(&ota_context.flash_ctx);

	http_parser_url_init(&parser);
	ret = http_parser_parse_url(download_url, strlen(download_url), 0, &parser);
	if (ret < 0) {
		LOG_ERR("Invalid url: %s", download_url);
		return -ENOTSUP;
	}

	/* parse URL schema address */
	off = parser.field_data[UF_SCHEMA].off;
	len = parser.field_data[UF_SCHEMA].len;

	/* check for supported protocol */
	if (strncmp(download_url + off, "http://", len) != 0) {
		LOG_ERR("Only http URL is supported");
		return -EPROTONOSUPPORT;
	}

	/* parse host address */
	off = parser.field_data[UF_HOST].off;
	len = parser.field_data[UF_HOST].len;
	strncpy(host, download_url+off, len);
	host[len] = '\0';

	/* parse uri address */
	off = parser.field_data[UF_PATH].off;
	len = parser.field_data[UF_PATH].len;
	strncpy(uri, download_url+off, len);
	uri[len] = '\0';

	/* set custom port */
	if ((parser.field_set & (1 << UF_PORT))) {
		port = parser.port;
	}

	LOG_INF("URL: http://%s:%d%s", host, port, uri);

	connect_socket(AF_INET, host, port, &ota_context.sock,
					(struct sockaddr *)&addr4, sizeof(addr4));

	if (ota_context.sock < 0) {
		LOG_ERR("Cannot create HTTP connection.");
		return -ECONNABORTED;
	}

	if (ota_context.sock >= 0) {
		struct http_request req;

		memset(&req, 0, sizeof(req));

		req.method = HTTP_GET;
		req.url = uri;
		req.host = host;
		req.protocol = "HTTP/1.1";
		req.response = response_cb;
		req.recv_buf = ota_context.recv_buf;
		req.recv_buf_len = sizeof(ota_context.recv_buf);

		ota_context.content_length = 0;
		ota_context.status = SIMPLE_HTTP_OTA_OK;

		ret = http_client_req(ota_context.sock, &req, HTTP_TIMEOUT, NULL);
		if (ret < 0 || ota_context.status != SIMPLE_HTTP_OTA_OK) {
			LOG_ERR("Error downloading file ret = %d", ret);
		} else {
			/* finish image flashing in here */
			int ret = flash_img_buffered_write(&ota_context.flash_ctx, NULL, 0, true);

			if (ret < 0) {
				LOG_ERR("Flash write error %d", ret);
			}

			/* perform boot upgrade check */
			if (boot_request_upgrade(BOOT_UPGRADE_TEST) == 0) {
				LOG_INF("Update installed. Restart the board to complete.");
			} else {
				LOG_ERR("Update not installed. File is corrupted or invalid.");
			}
		}
	}

	if (ota_context.sock >= 0) {
		close(ota_context.sock);
	}

	return ret;
}

int simple_http_ota_init(void)
{
	bool image_ok = false;
	int ret = 0;

	image_ok = boot_is_img_confirmed();
	LOG_INF("Image is%s confirmed OK", image_ok ? "" : " not");
	if (!image_ok) {
		ret = boot_write_img_confirmed();
		if (ret < 0) {
			LOG_ERR("Couldn't confirm this image: %d", ret);
			return ret;
		}

		LOG_INF("Erasing bank in slot 1...");
		ret = boot_erase_img_bank(FLASH_AREA_ID(image_1));
		if (ret) {
			LOG_ERR("Failed to erase second slot");
			return ret;
		}
	}

	return ret;
}
