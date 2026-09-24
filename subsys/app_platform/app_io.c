/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared general-I/O services: status LEDs, button/input events and a per-app
 * checkpoint store. The platform owns the LED pins and the button; an
 * application drives only the LED pins it was granted and receives button
 * presses one at a time. The checkpoint store lives outside any application's
 * memory domain, so a value survives the application crashing and restarting.
 * Each hardware service builds to a safe stub when its devicetree node is
 * absent.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include "app_events.h"
#include "app_grants.h"

#ifdef CONFIG_APP_PLATFORM_INPUT_KEYS
#include <zephyr/input/input.h>
#endif

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>
#include <string.h>

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS
#define IO_NAME_MAX 24
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- status LEDs ---- */

/* A LED's devicetree node may carry a free-text "label"; fall back to the
 * alias name when it does not, so every LED is addressable by a stable name.
 */
#define LED_NAME(alias_node) \
	DT_PROP_OR(alias_node, label, DT_NODE_FULL_NAME(alias_node))

/* Per-pin grant: bit N set means an application may drive LED pin N. Kept
 * outside the alias guard so the manager can always drop a slot's grants,
 * even on a board that wires no LED at all.
 */
static uint32_t led_grant[MAX_APPS];
/* Bit N set means this application last drove LED pin N on. */
static uint32_t led_on[MAX_APPS];
static K_MUTEX_DEFINE(led_lock);

#if DT_HAS_ALIAS(led0)
static const struct gpio_dt_spec led[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
#if DT_HAS_ALIAS(led1)
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
#endif
#if DT_HAS_ALIAS(led2)
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
#endif
};
#define LED_COUNT ARRAY_SIZE(led)

static const char *const led_name[] = {
	LED_NAME(DT_ALIAS(led0)),
#if DT_HAS_ALIAS(led1)
	LED_NAME(DT_ALIAS(led1)),
#endif
#if DT_HAS_ALIAS(led2)
	LED_NAME(DT_ALIAS(led2)),
#endif
};

static int led_init(void)
{
	for (int i = 0; i < (int)LED_COUNT; i++) {
		if (gpio_is_ready_dt(&led[i])) {
			(void)gpio_pin_configure_dt(&led[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	return 0;
}
SYS_INIT(led_init, APPLICATION, 90);

void app_platform_grant_led(int id, uint32_t pin_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		led_grant[id] = pin_mask;
	}
}

int z_impl_svc_led_set(int pin, int on)
{
	/* The capability is checked before the grant, as everywhere else, so a
	 * caller without APP_CAP_LED gets -EPERM and cannot tell an ungranted
	 * pin from one it was never entitled to reach.
	 */
	if (!app_has_cap(APP_CAP_LED)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (pin < 0 || pin >= (int)LED_COUNT || id < 0 || id >= MAX_APPS ||
	    (led_grant[id] & BIT(pin)) == 0U) {
		return -EACCES;
	}
	if (!gpio_is_ready_dt(&led[pin])) {
		return -ENODEV;
	}
	if (app_svc_lock(&led_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}

	int rc = gpio_pin_set_dt(&led[pin], on ? 1 : 0);

	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			led_on[i] &= ~BIT(pin);
		}
		if (on != 0) {
			led_on[id] |= BIT(pin);
		}
	}
	(void)app_svc_unlock(&led_lock);
	return rc;
}

int z_impl_svc_led_count(void)
{
	return (int)LED_COUNT;
}

/* Resolve a LED name to its index, so an application can address a LED by a
 * stable devicetree label instead of a position that shifts if LEDs are added.
 */
static int led_find(const char *name)
{
	for (int i = 0; i < (int)LED_COUNT; i++) {
		if (strcmp(led_name[i], name) == 0) {
			return i;
		}
	}
	return -ENOENT;
}

int z_impl_svc_led_find(const char *name)
{
	return led_find(name);
}
#else  /* no led alias */
void app_platform_grant_led(int id, uint32_t pin_mask)
{
	ARG_UNUSED(id); ARG_UNUSED(pin_mask);
}

int z_impl_svc_led_set(int pin, int on)
{
	if (!app_has_cap(APP_CAP_LED)) {
		return -EPERM;
	}
	ARG_UNUSED(pin); ARG_UNUSED(on);
	return -ENODEV;
}

int z_impl_svc_led_count(void)
{
	return 0;
}

int z_impl_svc_led_find(const char *name)
{
	ARG_UNUSED(name);
	return -ENOENT;
}
#endif /* DT_HAS_ALIAS(led0) */

void app_platform_clear_led_grants(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	led_grant[id] = 0U;
	if (app_svc_lock(&led_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
#if DT_HAS_ALIAS(led0)
	for (int i = 0; i < (int)LED_COUNT; i++) {
		if ((led_on[id] & BIT(i)) != 0U) {
			(void)gpio_pin_set_dt(&led[i], 0);
		}
	}
#endif
	led_on[id] = 0U;
	(void)app_svc_unlock(&led_lock);
}

/* ---- button / input events ---- */

#ifdef CONFIG_APP_PLATFORM_INPUT_KEYS
/*
 * Key events come from the input subsystem, so the driver debounces and reports
 * which key moved: an application sees one event per press rather than every
 * raw edge. Every application holding APP_CAP_INPUT receives every key, and the
 * code travels with the event so an application can tell them apart. Posting to
 * an application queue never blocks, so this is safe in the reporting context.
 */
static K_SEM_DEFINE(press_sem, 0, 16);
static atomic_t press_count;

static void key_event(struct input_event *evt, void *user)
{
	ARG_UNUSED(user);

	/* Presses only: a release is the same key reported back at zero. */
	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return;
	}

	atomic_inc(&press_count);
	k_sem_give(&press_sem);

	struct app_event out = {
		.kind = APP_EVENT_BUTTON,
		.value = 1,
		.code = (uint16_t)evt->code,
	};

	app_events_broadcast(&out, NULL);
}
INPUT_CALLBACK_DEFINE(NULL, key_event, NULL);

#elif DT_HAS_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;
static K_SEM_DEFINE(press_sem, 0, 16);
static atomic_t press_count;

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev); ARG_UNUSED(cb); ARG_UNUSED(pins);
	atomic_inc(&press_count);
	k_sem_give(&press_sem);

	/* Also deliver to every per-application queue, so an application can
	 * wait on the button together with its other sources. svc_button_wait
	 * keeps working for applications that use only the button.
	 */
	if (IS_ENABLED(CONFIG_APP_PLATFORM_EVENTS)) {
		struct app_event evt = { .kind = APP_EVENT_BUTTON };

		app_events_broadcast(&evt, NULL);
	}
}

static int button_init(void)
{
	if (!gpio_is_ready_dt(&button)) {
		return 0;
	}
	(void)gpio_pin_configure_dt(&button, GPIO_INPUT);
	(void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
	(void)gpio_add_callback(button.port, &button_cb);
	return 0;
}
SYS_INIT(button_init, APPLICATION, 90);

#else  /* no key source */
#define APP_IO_NO_BUTTON 1
#endif

#ifndef APP_IO_NO_BUTTON
int z_impl_svc_button_wait(int32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_INPUT)) {
		return -EPERM;
	}

	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_sem_take(&press_sem, t) != 0) {
		return -EAGAIN;
	}
	return (int)atomic_get(&press_count);
}
#else
int z_impl_svc_button_wait(int32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_INPUT)) {
		return -EPERM;
	}
	ARG_UNUSED(timeout_ms);
	return -ENODEV;
}
#endif /* APP_IO_NO_BUTTON */

/* ---- per-app checkpoint store ---- */

/* One retained value per application name, kept outside every application's
 * memory domain so it survives a crash and restart. It needs no capability.
 */
struct checkpoint {
	char name[APP_PLATFORM_NAME_LEN];
	uint32_t value;
};

static struct checkpoint checkpoints[MAX_APPS];
static struct k_spinlock checkpoint_lock;

static bool current_name(char *out)
{
	struct app_platform_status st;

	if (app_platform_status(app_platform_id_current(), &st) != 0 || st.name == NULL) {
		return false;
	}
	strncpy(out, st.name, APP_PLATFORM_NAME_LEN - 1);
	out[APP_PLATFORM_NAME_LEN - 1] = '\0';
	return true;
}

static bool name_running(const char *name)
{
	struct app_platform_status st;

	for (int i = 0; i < MAX_APPS; i++) {
		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strncmp(st.name, name, APP_PLATFORM_NAME_LEN) == 0) {
			return true;
		}
	}
	return false;
}

void z_impl_svc_checkpoint_set(uint32_t value)
{
	char name[APP_PLATFORM_NAME_LEN];
	struct checkpoint *slot = NULL;

	if (!current_name(name)) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&checkpoint_lock);

	for (int i = 0; i < MAX_APPS && slot == NULL; i++) {
		if (strncmp(checkpoints[i].name, name, sizeof(name)) == 0) {
			slot = &checkpoints[i];
		}
	}
	for (int i = 0; i < MAX_APPS && slot == NULL; i++) {
		if (checkpoints[i].name[0] == '\0' || !name_running(checkpoints[i].name)) {
			slot = &checkpoints[i];
		}
	}
	if (slot != NULL) {
		memcpy(slot->name, name, sizeof(name));
		slot->value = value;
	}
	k_spin_unlock(&checkpoint_lock, key);
}

uint32_t z_impl_svc_checkpoint_get(void)
{
	char name[APP_PLATFORM_NAME_LEN];
	uint32_t value = 0U;

	if (!current_name(name)) {
		return 0U;
	}

	k_spinlock_key_t key = k_spin_lock(&checkpoint_lock);

	for (int i = 0; i < MAX_APPS; i++) {
		if (strncmp(checkpoints[i].name, name, sizeof(name)) == 0) {
			value = checkpoints[i].value;
			break;
		}
	}
	k_spin_unlock(&checkpoint_lock, key);
	return value;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_led_set(int pin, int on)
{
	return z_impl_svc_led_set(pin, on);
}
#include <zephyr/syscalls/svc_led_set_mrsh.c>

static inline int z_vrfy_svc_led_count(void)
{
	return z_impl_svc_led_count();
}
#include <zephyr/syscalls/svc_led_count_mrsh.c>

static inline int z_vrfy_svc_led_find(const char *name)
{
	char buf[IO_NAME_MAX];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_led_find(buf);
}
#include <zephyr/syscalls/svc_led_find_mrsh.c>

static inline int z_vrfy_svc_button_wait(int32_t timeout_ms)
{
	return z_impl_svc_button_wait(timeout_ms);
}
#include <zephyr/syscalls/svc_button_wait_mrsh.c>

static inline void z_vrfy_svc_checkpoint_set(uint32_t value)
{
	z_impl_svc_checkpoint_set(value);
}
#include <zephyr/syscalls/svc_checkpoint_set_mrsh.c>

static inline uint32_t z_vrfy_svc_checkpoint_get(void)
{
	return z_impl_svc_checkpoint_get();
}
#include <zephyr/syscalls/svc_checkpoint_get_mrsh.c>
#endif /* CONFIG_USERSPACE */
