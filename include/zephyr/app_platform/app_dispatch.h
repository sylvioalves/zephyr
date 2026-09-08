/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_APP_DISPATCH_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_APP_DISPATCH_H_

#include <zephyr/app_platform/app_services.h>

/**
 * @file
 * @brief Callback dispatch for application input events.
 *
 * Registered handlers run on the application's own thread, in user mode and in
 * its own memory domain: this is a loop around @ref svc_event_wait, not a call
 * from the platform into the application. A handler that faults is an ordinary
 * application fault the platform reclaims, and no platform thread is ever
 * parked inside application code.
 *
 * Handlers are expected to return promptly. Dispatch is cooperative, so a
 * handler that blocks delays that application's other events.
 */

/**
 * @brief Handler for one event.
 *
 * @param evt  The event.
 * @param user Value passed to @ref app_dispatch_on.
 */
typedef void (*app_event_handler_t)(const struct app_event *evt, void *user);

/** @brief Maximum handlers one application may register. */
#define APP_DISPATCH_MAX 4

/** @brief Dispatch table. Zero-initialize before use. */
struct app_dispatch {
	struct {
		uint8_t             kind;
		app_event_handler_t fn;
		void               *user;
	} slot[APP_DISPATCH_MAX];
	int count;
	bool stop;
};

/**
 * @brief Register a handler for one kind of event.
 *
 * @param d    Dispatch table.
 * @param kind Event kind from @ref app_event_kind.
 * @param fn   Handler.
 * @param user Passed through to the handler.
 * @retval 0 on success, -ENOSPC when the table is full.
 */
static inline int app_dispatch_on(struct app_dispatch *d, uint8_t kind,
				  app_event_handler_t fn, void *user)
{
	if (d->count >= APP_DISPATCH_MAX) {
		return -ENOSPC;
	}
	d->slot[d->count].kind = kind;
	d->slot[d->count].fn = fn;
	d->slot[d->count].user = user;
	d->count++;
	return 0;
}

/**
 * @brief Ask a running dispatch loop to return.
 *
 * Safe to call from inside a handler.
 *
 * @param d Dispatch table.
 */
static inline void app_dispatch_stop(struct app_dispatch *d)
{
	d->stop = true;
}

/**
 * @brief Wait for events and run the matching handlers.
 *
 * Returns when @ref app_dispatch_stop was called, or when @p timeout_ms passes
 * with no event. A negative @p timeout_ms waits for ever.
 *
 * @param d          Dispatch table.
 * @param timeout_ms Idle timeout in milliseconds.
 * @retval 0 when stopped, -EAGAIN on the idle timeout, or a negative errno
 *         from @ref svc_event_wait.
 */
static inline int app_dispatch_run(struct app_dispatch *d, int32_t timeout_ms)
{
	d->stop = false;

	while (!d->stop) {
		struct app_event evt;
		int rc = svc_event_wait(&evt, timeout_ms);

		if (rc != 0) {
			return rc;
		}
		for (int i = 0; i < d->count; i++) {
			if (d->slot[i].kind == evt.kind &&
			    d->slot[i].fn != NULL) {
				d->slot[i].fn(&evt, d->slot[i].user);
			}
		}
	}
	return 0;
}

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_APP_DISPATCH_H_ */
