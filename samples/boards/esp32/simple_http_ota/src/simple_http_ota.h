/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SIMPLE_HTTP_OTA_H__
#define __SIMPLE_HTTP_OTA_H__

#define SIMPLE_HTTP_OTA_MAJOR_VERSION 1
#define SIMPLE_HTTP_OTA_MINOR_VERSION 0

/**
 * @brief Init application
 *
 * Performs MCUBoot image check
 *
 * @return 0 on success
 * @return other error code returned by MCUBoot initialization
 */
int simple_http_ota_init(void);

/**
 * @brief Init over-the-air update
 *
 * This call starts http client to retrieve file information, performs
 * download and proper flashing into slot 1.
 *
 * @return 0 on success
 * @return other error code returned by HTTP and Flash API.
 */
int simple_http_ota_run(void);

#endif /* __SIMPLE_HTTP_OTA_H__ */
