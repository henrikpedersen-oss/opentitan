// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_BASE_CHERIOT_MMIO_H_
#define OPENTITAN_SW_DEVICE_LIB_BASE_CHERIOT_MMIO_H_

/**
 * @file
 * @brief The capability behind an MMIO address in a CHERIoT (purecap) build.
 *
 * In purecap code an integer cast to a pointer carries no tag, so every access through it
 * traps. `mmio_region_from_addr()` and the `abs_mmio_*()` functions therefore get their pointer
 * from `ot_cheriot_mmio_cap()` in a CHERIoT build: a capability derived from the memory root,
 * bounded exactly to the bus window (register window or memory) that contains the address, with
 * the address as its cursor. An address in no window gets an untagged pointer, so any access
 * through it traps rather than reaching something by accident.
 *
 * The function is provided by the platform's start-up library (for the EG2 test framework,
 * `sw/device/lib/testing/eg2:dev`), which knows the windows of the top; a CHERIoT image that
 * uses MMIO without one does not link.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __CHERIOT__
volatile void *ot_cheriot_mmio_cap(uintptr_t address);

/** The pointer for an MMIO address: a bounded capability in a CHERIoT build. */
#define OT_MMIO_ADDR_TO_PTR(address) ot_cheriot_mmio_cap((uintptr_t)(address))
#else
/** The pointer for an MMIO address: the address itself outside CHERIoT. */
#define OT_MMIO_ADDR_TO_PTR(address) ((volatile void *)(address))
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_LIB_BASE_CHERIOT_MMIO_H_
