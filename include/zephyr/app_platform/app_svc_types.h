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
	APP_EVENT_TIMER,      /**< a timer slot expired; line is the slot */
	APP_EVENT_IPC,        /**< a message arrived in the IPC mailbox */
	APP_EVENT_CAN,        /**< a CAN frame is queued */
	APP_EVENT_UART_RX,    /**< bytes arrived on the secondary UART */
	APP_EVENT_NET,        /**< data is queued; line 0 MQTT, line 1 socket */
	APP_EVENT_COUNTER,    /**< a counter alarm fired; line is the counter */
	APP_EVENT_RADIO,      /**< an 802.15.4 datagram is queued */
};

/** @ref app_svc_gpio_configure flag: the line is an input. */
#define APP_GPIO_INPUT      0x01U
/** @ref app_svc_gpio_configure flag: the line is an output, driven inactive. */
#define APP_GPIO_OUTPUT     0x02U
/** @ref app_svc_gpio_configure flag: enable the pull-up. */
#define APP_GPIO_PULL_UP    0x04U
/** @ref app_svc_gpio_configure flag: enable the pull-down. */
#define APP_GPIO_PULL_DOWN  0x08U
/** @ref app_svc_gpio_configure flag: drive the output open-drain. */
#define APP_GPIO_OPEN_DRAIN 0x10U

/** Edges reported by @ref app_svc_gpio_watch_edge. */
enum app_gpio_edge {
	APP_GPIO_EDGE_BOTH = 0, /**< every change */
	APP_GPIO_EDGE_RISING,   /**< changes to the active level */
	APP_GPIO_EDGE_FALLING,  /**< changes to the inactive level */
};

/** @ref app_svc_spi_configure flag: clock idles high. */
#define APP_SPI_CPOL      0x01U
/** @ref app_svc_spi_configure flag: data sampled on the second clock edge. */
#define APP_SPI_CPHA      0x02U
/** @ref app_svc_spi_configure flag: least significant bit first. */
#define APP_SPI_LSB_FIRST 0x04U
/** @ref app_svc_spi_configure chip select value: drive no chip select. */
#define APP_SPI_CS_NONE   0xffU

/** @ref app_svc_i2c_configure speed: the devicetree setting. */
#define APP_I2C_SPEED_DEFAULT   0U
/** @ref app_svc_i2c_configure speed: 100 kHz. */
#define APP_I2C_SPEED_STANDARD  1U
/** @ref app_svc_i2c_configure speed: 400 kHz. */
#define APP_I2C_SPEED_FAST      2U
/** @ref app_svc_i2c_configure speed: 1 MHz. */
#define APP_I2C_SPEED_FAST_PLUS 3U

/** @ref app_i2c_msg::flags bit: read into the buffer instead of writing it. */
#define APP_I2C_MSG_READ    0x01U
/** @ref app_i2c_msg::flags bit: send a stop after the message. */
#define APP_I2C_MSG_STOP    0x02U
/** @ref app_i2c_msg::flags bit: send a repeated start before the message. */
#define APP_I2C_MSG_RESTART 0x04U
/** Messages one @ref app_svc_i2c_transfer call may carry. */
#define APP_I2C_MSGS_MAX    8

/** One message of an @ref app_svc_i2c_transfer, taking the next @p len bytes of
 *  the call's buffer.
 */
struct app_i2c_msg {
	uint16_t len;   /**< bytes to read or write */
	uint8_t  flags; /**< APP_I2C_MSG_* bits */
};

/** @ref app_svc_pwm_set_ns flag: invert the channel's devicetree polarity. */
#define APP_PWM_INVERTED 0x01U

/** Bit of an @ref app_event_kind in an @ref app_svc_event_subscribe mask. */
#define APP_EVENT_BIT(kind) (1UL << (kind))

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

/** @ref app_svc_uart_errors bit: the receiver overran. */
#define APP_UART_ERR_OVERRUN    0x01U
/** @ref app_svc_uart_errors bit: a byte had a parity error. */
#define APP_UART_ERR_PARITY     0x02U
/** @ref app_svc_uart_errors bit: a byte had a framing error. */
#define APP_UART_ERR_FRAMING    0x04U
/** @ref app_svc_uart_errors bit: a break was received. */
#define APP_UART_ERR_BREAK      0x08U
/** @ref app_svc_uart_errors bit: the receive buffer was full and bytes were lost. */
#define APP_UART_ERR_RX_DROPPED 0x10U

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
