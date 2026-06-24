/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BEHAVIOR_SANDBOX_POLICY_H_
#define BEHAVIOR_SANDBOX_POLICY_H_

/*
 * A behavior policy is the dynamic payload the sandbox executes. In a full
 * framework this is what an LLM would produce (as Wasm or a DSL); here it is
 * compiled C that runs in user mode and may only reach hardware through the
 * capability surface.
 */
enum policy_kind {
	/* Happy path: blink the pin and report die temperature. */
	POLICY_GOOD,
	/* Containment: attempt a raw access the cap-gate does not grant. */
	POLICY_ROGUE,
	/* Supervision: spin forever so the task watchdog must intervene. */
	POLICY_RUNAWAY,
};

/* Entry point executed inside the user-mode sandbox thread. */
void policy_run(void *kind, void *unused1, void *unused2);

#endif /* BEHAVIOR_SANDBOX_POLICY_H_ */
