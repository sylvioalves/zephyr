/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES10_SVC_H_
#define APP_MORE_SERVICES10_SVC_H_

#include <zephyr/kernel.h>

/*
 * A tenth batch: an I2S audio-output service. The platform owns the I2S
 * controller, its DMA, and the transmit memory slab; the app simply hands raw
 * PCM samples to be played. The supervisor copies the app bytes into a slab
 * block and streams them, so none of the I2S kernel objects are exposed to user
 * mode.
 *
 *   svc_audio_play - queue @len bytes of PCM (16-bit stereo) for playback.
 */

/* Scoped: queue @len bytes of PCM for I2S playback. Returns bytes accepted. */
__syscall int svc_audio_play(const uint8_t *pcm, size_t len);

/* Supervisor-only: configure I2S TX before launching the app. */
int svc_audio_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES10_SVC_H_ */
