// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_RESULT_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_RESULT_H_

/**
 * @file
 * @brief The test's console and verdict, the same in both execution modes.
 *
 * The console writes straight to UART0's transmit FIFO. In RISC-V mode OTTF has set UART0 up
 * before `test_main`; in CHERIoT mode the framework's start-up code does (`eg2_console_init`).
 *
 * The verdict goes three ways (EG2 framework, part 6):
 *   - the UART: `PASS!` or `FAIL!` (OTTF prints it in RISC-V mode, `eg2_result_report` in
 *     CHERIoT mode);
 *   - the DV status window of `rv_core_ibex`: 0x900d or 0xbaad, which the bench watches;
 *   - optionally a GPIO pin, for runs without a UART reader: built with
 *     `EG2_RESULT_GPIO_PIN=<n>`, the pin goes high on a pass and toggles on a failure. Routing
 *     the GPIO to a pad is the platform's (pinmux), not the test's.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Enables UART0's transmitter at the platform's baud rate (CHERIoT mode only needs it). */
void eg2_console_init(void);

/** Writes a NUL-terminated string to the console. */
void eg2_puts(const char *s);

/** Writes `value` as eight hex digits. */
void eg2_puthex(uint32_t value);

/** Writes `value` in decimal. */
void eg2_putdec(uint32_t value);

/** Marks the start of the test in the DV status window. */
void eg2_result_in_test(void);

/**
 * The end of a test: the final status sweep (`eg2_status_check`), then the GPIO verdict.
 * Returns `passed` and the sweep's verdict. The framework's `test_main` calls it after the
 * test's body.
 */
bool eg2_test_end(bool passed);

/**
 * Reports the verdict on the console and in the DV status window, then parks the core.
 * CHERIoT mode only: in RISC-V mode OTTF does this.
 */
void eg2_result_report(bool passed) __attribute__((noreturn));

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_RESULT_H_
