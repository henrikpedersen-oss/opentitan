// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_RESULT_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_RESULT_H_

/**
 * @file
 * @brief The test's console and verdict, the same in both execution modes.
 *
 * The console has two channels, chosen by `kEg2ConsoleDv`, a constant the chip bench overwrites
 * before the CPU starts (chip_sw_eg2_base_vseq, +eg2_dv_console=0|1, default 1):
 *   - 1, the DV channel: each character is one write to the DV window's console word
 *     (`rv_core_ibex` DV_SIM_WINDOW + 8), with the line's verbosity in bits 11:8. The bench prints
 *     each line in run.log with that UVM verbosity (`EG2_SW`), so +UVM_VERBOSITY selects what is
 *     shown. It costs no simulated time;
 *   - 0, UART0's transmit FIFO, as on every other platform (Verilator, the SystemC model, silicon):
 *     the image's own value. Each line then takes the UART's time (about 350 us at 1 Mbaud).
 * `eg2_console_init` sets UART0 up: in CHERIoT mode the framework's start-up code calls it, in
 * RISC-V mode `test_main` does on sim_dv (OTTF sets the console up on every other platform, not on
 * sim_dv).
 *
 * The verdict goes three ways (EG2 framework, part 6):
 *   - UART0, whichever channel the console uses: `PASS!`, or `FAIL! ` followed by the first console
 *     line that reported a failure (one containing `FAIL`, `MISMATCH`, `STATUS ` or `TRAP`, cut
 *     to 40 characters), so the UART alone says why (OTTF prints its own verdict in RISC-V mode, `eg2_result_report` in CHERIoT mode);
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
/** The UVM verbosity the bench prints a console line with (DV channel only). */
typedef enum eg2_verbosity {
  kEg2VerbosityNone = 0,
  kEg2VerbosityLow = 1,
  kEg2VerbosityMedium = 2,
  kEg2VerbosityHigh = 3,
  kEg2VerbosityDebug = 4,
} eg2_verbosity_t;
/**
 * Sets the verbosity of the console lines that follow; the default is `kEg2VerbosityLow`. A line
 * reporting a failure (`FAIL`, `MISMATCH`, `STATUS `, `TRAP`) is always printed, whatever its
 * verbosity.
 */
void eg2_console_verbosity(eg2_verbosity_t verbosity);

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
 * CHERIoT mode, and tests without OTTF (the boot-ROM test eg2_boot_epmp_reset): in RISC-V mode
 * OTTF does this.
 */
void eg2_result_report(bool passed) __attribute__((noreturn));

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_RESULT_H_
