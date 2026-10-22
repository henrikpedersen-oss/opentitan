// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_DEV_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_DEV_H_

/**
 * @file
 * @brief The register windows of Earl Grey 2, reached the same way in both execution modes.
 *
 * `eg2_dev()` returns a pointer to a device's register window: in RISC-V mode an integer address
 * made into a pointer, in CHERIoT mode a capability derived from the memory root with exact
 * bounds on the window (EG2 verification spec REQ_PER_01). The devices are those of the
 * generated `eg2_tables.h`.
 */

#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum eg2_dev_id {
#define EG2_DEV_ENUM_(id, name, base, size) id,
  EG2_DEV_TABLE(EG2_DEV_ENUM_)
#undef EG2_DEV_ENUM_
  kEg2DevCount,
} eg2_dev_id_t;

/** The device's register window: base pointer, valid for the window's size. */
volatile uint32_t *eg2_dev(eg2_dev_id_t id);

/** The device's name in the top description, e.g. "uart0" or "otp_ctrl.core". */
const char *eg2_dev_name(eg2_dev_id_t id);

/** Reads the 32-bit register at `offset` bytes into the device's window. */
uint32_t eg2_dev_read32(eg2_dev_id_t id, uint32_t offset);

/** Writes the 32-bit register at `offset` bytes into the device's window. */
void eg2_dev_write32(eg2_dev_id_t id, uint32_t offset, uint32_t value);

/** Writes a shadowed register: the same value twice, as its update protocol requires. */
void eg2_dev_write32_shadowed(eg2_dev_id_t id, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_DEV_H_
