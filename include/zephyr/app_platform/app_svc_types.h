/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_SVC_TYPES_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_SVC_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zephyr/app_platform/app_caps.h>

/**
 * @addtogroup app_platform_services
 * @{
 */

/** Entry point of an application worker thread. */
typedef void (*app_thread_entry_t)(void *arg);

/** Metadata for one announced sensor service. */
struct app_svc_sensor_info {
	char name[16];   /**< service name, for example "temperature" */
	char unit[10];   /**< unit of the value, for example "m.degC" */
	bool present;    /**< a device on this board backs the kind    */
};

/** Timer slots an application may hold at once. */
#define APP_TIMER_SLOTS 4

/** Snapshot of the shared Wi-Fi link, filled by @ref app_svc_wifi_status. */
struct app_svc_wifi_status {
	uint8_t  connected;  /**< Non-zero when the link has an IPv4 address. */
	char     ssid[33];   /**< SSID the link is joined to (NUL-terminated). */
	char     ipv4[16];   /**< Dotted IPv4 address, empty when none. */
	int8_t   rssi;       /**< Cached signal strength in dBm, 0 if unknown. */
};

/**
 * @brief Kinds of event delivered through @ref app_svc_event_wait.
 */
enum app_event_kind {
	APP_EVENT_NONE = 0,   /**< no event (timeout) */
	APP_EVENT_BUTTON,     /**< the board button was pressed */
	APP_EVENT_GPIO,       /**< a granted input line changed */
	APP_EVENT_GATT_WRITE, /**< a peer wrote a served characteristic */
	APP_EVENT_BLE_CONNECTED,    /**< a peer connected to the served app */
	APP_EVENT_BLE_DISCONNECTED, /**< the connected peer went away */
	APP_EVENT_STOP,       /**< the platform asked the application to exit */
};

/**
 * @brief One input event delivered to an application.
 */
struct app_event {
	uint8_t  kind;   /**< one of @ref app_event_kind */
	uint8_t  line;   /**< GPIO line index, or served characteristic index */
	uint8_t  value;  /**< line level, or pressed state for a key */
	uint16_t code;   /**< key code, for @ref APP_EVENT_BUTTON */
	uint32_t seq;    /**< per-application counter, gaps mean dropped events */
};

/** A user-mode-safe snapshot of one application group. */
struct app_svc_app_info {
	char     name[16];
	uint8_t  active;
	uint8_t  monitored;
	uint8_t  faulted;
	uint32_t stack_size;
	uint32_t stack_used;
	uint32_t reserved;
	uint64_t cpu_cycles;
	uint64_t cpu_total;
};

/** A user-mode-safe wall-clock time. */
struct app_svc_rtc_time {
	uint16_t year;    /**< full year, e.g. 2026 */
	uint8_t  month;   /**< month [1, 12] */
	uint8_t  day;     /**< day of the month [1, 31] */
	uint8_t  hour;    /**< hour [0, 23] */
	uint8_t  minute;  /**< minute [0, 59] */
	uint8_t  second;  /**< second [0, 59] */
};

/** A rectangle on the display, in panel coordinates. */
struct app_display_rect {
	uint16_t x;      /**< left column */
	uint16_t y;      /**< top row */
	uint16_t width;  /**< width in pixels */
	uint16_t height; /**< height in pixels */
};

/** @brief Pixel formats reported in @ref app_svc_display_info.pixel_format. */
enum app_pixel_format {
	APP_PIXEL_FORMAT_RGB_888 = BIT(0),    /**< 24-bit RGB */
	APP_PIXEL_FORMAT_MONO01 = BIT(1),     /**< monochrome, 0 is black */
	APP_PIXEL_FORMAT_MONO10 = BIT(2),     /**< monochrome, 1 is black */
	APP_PIXEL_FORMAT_ARGB_8888 = BIT(3),  /**< 32-bit ARGB */
	APP_PIXEL_FORMAT_RGB_565 = BIT(4),    /**< 16-bit RGB */
	APP_PIXEL_FORMAT_RGB_565X = BIT(5),   /**< 16-bit RGB, bytes swapped */
	APP_PIXEL_FORMAT_L_8 = BIT(6),        /**< 8-bit luminance */
	APP_PIXEL_FORMAT_AL_88 = BIT(7),      /**< 8-bit luminance with alpha */
	APP_PIXEL_FORMAT_XRGB_8888 = BIT(8),  /**< 32-bit XRGB */
	APP_PIXEL_FORMAT_BGR_888 = BIT(9),    /**< 24-bit BGR */
	APP_PIXEL_FORMAT_ABGR_8888 = BIT(10), /**< 32-bit ABGR */
	APP_PIXEL_FORMAT_RGBA_8888 = BIT(11), /**< 32-bit RGBA */
	APP_PIXEL_FORMAT_BGRA_8888 = BIT(12), /**< 32-bit BGRA */
};

/** What an application sees of the display: its viewport and the pixel layout. */
struct app_svc_display_info {
	uint16_t width;          /**< viewport width in pixels */
	uint16_t height;         /**< viewport height in pixels */
	uint32_t pixel_format;   /**< @ref app_pixel_format of the panel */
	uint8_t  bytes_per_pixel; /**< bytes per pixel in a write buffer */
};

/** @brief Parity for @ref app_svc_uart_configure. */
enum app_uart_parity {
	APP_UART_PARITY_NONE = 0,
	APP_UART_PARITY_ODD,
	APP_UART_PARITY_EVEN,
};

/**
 * @brief Header at the front of a ring bound with @ref app_svc_ring_bind.
 *
 * The application writes at @c head and the platform consumes from @c tail
 * when sending; the platform writes at @c head and the application consumes
 * from @c tail when receiving. Both live in the application's own memory, so
 * the platform re-reads and range-checks them on every use and reports
 * @c -EBADMSG rather than trusting a value that cannot be right.
 */
struct app_ring_hdr {
	volatile uint32_t head;  /**< index one past the newest byte */
	volatile uint32_t tail;  /**< index of the oldest unread byte */
	uint32_t reserved[2];    /**< pad the data to an aligned offset */
};

/**
 * @brief Maximum payload one generic GATT read or write may carry.
 */
#define APP_BLE_GATT_MAX 64

/**
 * @brief Maximum characteristics one application may serve.
 */
#define APP_BLE_SERVE_MAX_CHRC 4

/** @brief A characteristic the application asks the platform to serve. */
struct app_ble_chrc {
	uint8_t uuid[16];   /**< characteristic UUID bytes, little-endian */
	uint8_t uuid_len;   /**< 2 or 16 */
	uint8_t readable;   /**< non-zero to allow reads */
	uint8_t writable;   /**< non-zero to allow writes */
	uint8_t notifiable; /**< non-zero to allow @ref app_svc_ble_notify */
};

/** @} */

/** @ref app_can_frame::flags bit: 29-bit extended identifier. */
#define APP_CAN_FRAME_EXT 0x01U
/** @ref app_can_frame::flags bit: remote transmission request, no payload. */
#define APP_CAN_FRAME_RTR 0x02U

/** One classic CAN frame, for @ref app_svc_can_send_frame and
 *  @ref app_svc_can_recv_frame.
 */
struct app_can_frame {
	uint32_t id;      /**< 11-bit identifier, or 29-bit with APP_CAN_FRAME_EXT */
	uint8_t  flags;   /**< APP_CAN_FRAME_* bits */
	uint8_t  dlc;     /**< payload length, 0 to 8 */
	uint8_t  data[8]; /**< payload */
};

/** Controller states reported by @ref app_svc_can_status. */
enum app_can_state {
	APP_CAN_ERROR_ACTIVE = 0, /**< normal operation */
	APP_CAN_ERROR_WARNING,    /**< an error counter passed the warning level */
	APP_CAN_ERROR_PASSIVE,    /**< an error counter passed 127 */
	APP_CAN_BUS_OFF,          /**< the controller left the bus */
	APP_CAN_STOPPED,          /**< the controller is stopped */
};

/** Bus and per-application receive status, filled by @ref app_svc_can_status. */
struct app_can_status {
	uint8_t  state;         /**< one of @ref app_can_state */
	uint8_t  tx_err_cnt;    /**< controller transmit error counter */
	uint8_t  rx_err_cnt;    /**< controller receive error counter */
	int32_t  last_tx_error; /**< errno of the last failed send since the last call, or 0 */
	uint32_t rx_dropped;    /**< frames dropped because the queue was full */
};

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_SVC_TYPES_H_ */
