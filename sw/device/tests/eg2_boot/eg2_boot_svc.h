// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_BOOT_EG2_BOOT_SVC_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_BOOT_EG2_BOOT_SVC_H_

/**
 * @file
 * @brief Boot services requests from owner code, in both modes (EG2 spec REQ_BOOT_12).
 *
 * A request is built with OpenTitan's boot_svc library (its digest computed by the HMAC block),
 * then copied word by word into the retention SRAM's boot services area through the retention
 * SRAM's pointer, which in CHERIoT mode is a capability bounded to the retention SRAM's window.
 * After a reset, ROM_EXT leaves its response there (silicon creator "Boot Services").
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_msg.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Copies `msg` into the retention SRAM's boot services area. */
void eg2_boot_svc_put(const boot_svc_msg_t *msg);

/** Copies the retention SRAM's boot services area into `msg`. */
void eg2_boot_svc_get(boot_svc_msg_t *msg);

/**
 * Checks the response in the retention SRAM: a valid header (digest, length) of type `type`.
 * Copies it into `msg`. Returns whether both held.
 */
bool eg2_boot_svc_check_response(uint32_t type, boot_svc_msg_t *msg);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_BOOT_EG2_BOOT_SVC_H_
