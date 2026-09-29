/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Semaphores the threads of one application share. The platform owns the
 * kernel objects and an application names them by index, so no kernel object
 * is granted to application threads.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#include "app_grants.h"

#define APP_SEMS CONFIG_APP_PLATFORM_APP_SEMS

#if APP_SEMS > 0
struct app_sem {
	struct k_sem sem;
	bool used;
};

static struct app_sem sems[MAX_APPS][APP_SEMS];
static struct k_spinlock sems_lock;

static struct app_sem *sem_of(int handle)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || handle < 0 || handle >= APP_SEMS ||
	    !sems[id][handle].used) {
		return NULL;
	}
	return &sems[id][handle];
}
#endif

void app_platform_clear_sems(int id)
{
#if APP_SEMS > 0
	k_spinlock_key_t key = k_spin_lock(&sems_lock);

	for (int i = 0; i < APP_SEMS; i++) {
		sems[id][i].used = false;
	}
	k_spin_unlock(&sems_lock, key);
#else
	ARG_UNUSED(id);
#endif
}

int z_impl_app_svc_sem_init(uint32_t initial, uint32_t limit)
{
#if APP_SEMS > 0
	int id = app_platform_id_current();
	k_spinlock_key_t key;
	int rc = -ENOMEM;

	if (id < 0 || id >= MAX_APPS || !app_has_cap(APP_CAP_THREAD)) {
		return -EPERM;
	}
	if (limit == 0U || initial > limit) {
		return -EINVAL;
	}

	key = k_spin_lock(&sems_lock);
	for (int i = 0; i < APP_SEMS; i++) {
		if (!sems[id][i].used) {
			sems[id][i].used = true;
			rc = i;
			break;
		}
	}
	k_spin_unlock(&sems_lock, key);
	if (rc >= 0) {
		(void)k_sem_init(&sems[id][rc].sem, initial, limit);
	}
	return rc;
#else
	ARG_UNUSED(initial);
	ARG_UNUSED(limit);
	return -ENOTSUP;
#endif
}

int z_impl_app_svc_sem_give(int handle)
{
#if APP_SEMS > 0
	struct app_sem *s;

	if (!app_has_cap(APP_CAP_THREAD)) {
		return -EPERM;
	}
	s = sem_of(handle);
	if (s == NULL) {
		return -EINVAL;
	}
	k_sem_give(&s->sem);
	return 0;
#else
	ARG_UNUSED(handle);
	return -ENOTSUP;
#endif
}

int z_impl_app_svc_sem_take(int handle, int32_t timeout_ms)
{
#if APP_SEMS > 0
	struct app_sem *s;

	if (!app_has_cap(APP_CAP_THREAD)) {
		return -EPERM;
	}
	s = sem_of(handle);
	if (s == NULL) {
		return -EINVAL;
	}
	return k_sem_take(&s->sem, timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms));
#else
	ARG_UNUSED(handle);
	ARG_UNUSED(timeout_ms);
	return -ENOTSUP;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_sem_init(uint32_t initial, uint32_t limit)
{
	return z_impl_app_svc_sem_init(initial, limit);
}
#include <zephyr/syscalls/app_svc_sem_init_mrsh.c>

static inline int z_vrfy_app_svc_sem_give(int handle)
{
	return z_impl_app_svc_sem_give(handle);
}
#include <zephyr/syscalls/app_svc_sem_give_mrsh.c>

static inline int z_vrfy_app_svc_sem_take(int handle, int32_t timeout_ms)
{
	return z_impl_app_svc_sem_take(handle, timeout_ms);
}
#include <zephyr/syscalls/app_svc_sem_take_mrsh.c>
#endif
