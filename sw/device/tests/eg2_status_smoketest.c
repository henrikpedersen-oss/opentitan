// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The EG2 framework's status module, in either execution mode (one source, two builds).
 *
 *  1. After boot, nothing is set: no alert, no error interrupt, no error flag, the right mode,
 *     a power-on reset.
 *  2. UART1's fatal_fault alert (through ALERT_TEST) and its rx_overflow interrupt (through
 *     INTR_TEST) are raised on purpose and declared: the sweep must find exactly those.
 *  3. Cleared, the sweep must find nothing again.
 *
 * Built with `EG2_STATUS_INJECT_FAIL`, the alert is raised without being declared: the run must
 * then fail at step 2 with `STATUS unexpected alert uart1_fatal_fault` (the sweep is live).
 */

#include "hw/top/uart_regs.h"  // Generated.
#include "sw/device/lib/testing/eg2/eg2_test.h"

EG2_TEST_CONFIG();

enum {
  // Alerts arrive over a handshake; this is far more reads than that takes.
  kAlertPolls = 1000,
};

static bool step(const char *name, bool ok) {
  eg2_puts(ok ? "step ok: " : "step FAILED: ");
  eg2_puts(name);
  eg2_puts("\r\n");
  return ok;
}

bool test_main(void) {
  eg2_status_init();
  bool ok = step("clean after boot", eg2_status_check());

  int alert = eg2_status_alert_find("uart1_fatal_fault");
  if (alert < 0) {
    return step("uart1_fatal_fault in the alert table", false);
  }
  const uint32_t rx_overflow = 1u << UART_INTR_COMMON_RX_OVERFLOW_BIT;
#ifndef EG2_STATUS_INJECT_FAIL
  eg2_status_expect_alert((size_t)alert);
#endif
  eg2_status_expect_intr(kEg2DevUart1, rx_overflow);
  eg2_status_raise_alert((size_t)alert);
  eg2_status_raise_intr(kEg2DevUart1, rx_overflow);
  ok = step("alert arrived", eg2_status_wait_alert((size_t)alert, kAlertPolls)) && ok;
  ok = step("exactly the raised alert and interrupt", eg2_status_check()) && ok;

  eg2_status_clear_expected();
  ok = step("clean after clearing", eg2_status_check()) && ok;
  return eg2_test_end(ok);
}
