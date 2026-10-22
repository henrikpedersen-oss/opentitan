// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_STATUS_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_STATUS_H_

/**
 * @file
 * @brief The status module: is the chip still healthy? (EG2 framework, part 5)
 *
 * `eg2_status_init` gives the module the alert handler: every alert and local alert enabled, all
 * in class D, which never escalates (its CTRL keeps EN clear), so an alert is recorded and the
 * test goes on. In RISC-V mode the framework's test configuration sets OTTF's `ignore_alerts`,
 * so OTTF's own alert catcher stays out of the way.
 *
 * `eg2_status_check` sweeps, and reports on the console each thing that is not as it should be:
 *   - every alert cause and local alert cause, and each class's accumulator and state;
 *   - every device's error interrupts (INTR_STATE, event-type error interrupts only);
 *   - every device's error flags in its status and error registers;
 *   - the execution mode (`misa`), against the mode the test was built for;
 *   - the reset reason: anything but a power-on reset.
 * The registers and fields come from `eg2_tables.h`, generated from the IPs' register
 * descriptions. What a test causes on purpose it declares first (`eg2_status_expect_*`); an
 * expected alert or interrupt that is missing is a failure too.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of alerts, in alert-handler order (the alert IDs of the top description). */
enum { kEg2AlertCount = EG2_ALERT_COUNT };

/** Configures the alert handler: all alerts and local alerts enabled into class D. */
void eg2_status_init(void);

/** Sweeps the chip; returns true if nothing unexpected is set and nothing expected is missing. */
bool eg2_status_check(void);

/** The alert's index, from its name in the top description ("uart1_fatal_fault"); -1 if none. */
int eg2_status_alert_find(const char *name);

/** The alert's name. */
const char *eg2_status_alert_name(size_t alert);

/** Declares that the test raises `alert` on purpose. */
void eg2_status_expect_alert(size_t alert);

/** Declares that the test raises the error interrupts `mask` of device `dev` on purpose. */
void eg2_status_expect_intr(eg2_dev_id_t dev, uint32_t mask);

/** Raises `alert` through its device's ALERT_TEST register. */
void eg2_status_raise_alert(size_t alert);

/** Raises interrupts `mask` of device `dev` through its INTR_TEST register. */
void eg2_status_raise_intr(eg2_dev_id_t dev, uint32_t mask);

/**
 * Waits until `alert` has reached the alert handler (its cause is set). Alerts travel over a
 * differential handshake and arrive some cycles after the write that raised them. Returns false
 * if it has not arrived after `max_polls` reads.
 */
bool eg2_status_wait_alert(size_t alert, uint32_t max_polls);

/**
 * Clears what the test declared and caused: the expected alerts' causes, class D's accumulator
 * and interrupt, the expected interrupts. The expectations are then forgotten.
 */
void eg2_status_clear_expected(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_STATUS_H_
