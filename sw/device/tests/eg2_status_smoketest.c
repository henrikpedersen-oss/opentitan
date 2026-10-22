// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The EG2 framework's status module and its random parameters, in either execution mode (one
 * source, two builds).
 *
 *  1. After boot, nothing is set: no alert, no error interrupt, no error flag, the right mode,
 *     a power-on reset.
 *  2. One alert (parameter `alert`, drawn by the bench's vseq under its constraints) is raised
 *     through ALERT_TEST, and `num_intrs` error interrupts, each picked from the error-interrupt
 *     table with the software's seed, through INTR_TEST. All are declared: the sweep must find
 *     exactly those.
 *  3. Cleared, the sweep must find nothing again.
 *
 * Built with `EG2_STATUS_INJECT_FAIL`, the alert is raised without being declared: the run must
 * then fail at step 2 with `STATUS unexpected alert <name>` (the sweep is live).
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"

EG2_TEST_CONFIG();

// The defaults (UART1's fatal_fault, one interrupt) are what runs without a bench.
#define STATUS_SMOKETEST_PARAMS(X)              \
  X(alert, 1, 0, EG2_ALERT_COUNT - 1)           \
  X(num_intrs, 1, 1, 4)
EG2_TEST_PARAMS(STATUS_SMOKETEST_PARAMS);

enum {
  // Alerts arrive over a handshake, from the always-on domain at its slow clock; this is far
  // more reads than that takes.
  kAlertPolls = 10000,
};

// The error interrupts, as the status module checks them.
typedef struct intr_row {
  eg2_dev_id_t dev;
  uint32_t mask;
} intr_row_t;

static const intr_row_t kIntrRows[] = {
#define INTR_ROW_(dev, state, test, mask) {dev, mask},
    EG2_INTR_TABLE(INTR_ROW_)
#undef INTR_ROW_
};

static bool step(const char *name, bool ok) {
  eg2_puts(ok ? "step ok: " : "step FAILED: ");
  eg2_puts(name);
  eg2_puts("\r\n");
  return ok;
}

// One of the row's error interrupts, chosen with the software's seed.
static uint32_t pick_bit(uint32_t mask) {
  uint32_t n = eg2_rand_range(0, (uint32_t)__builtin_popcount(mask) - 1);
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if ((mask >> bit) & 1) {
      if (n-- == 0) {
        return 1u << bit;
      }
    }
  }
  return 0;
}

bool eg2_test_main(void) {
  bool ok = step("clean after boot", eg2_status_check());

  size_t alert = EG2_PARAM(alert);
  eg2_puts("raising alert ");
  eg2_puts(eg2_status_alert_name(alert));
  eg2_puts("\r\n");
#ifndef EG2_STATUS_INJECT_FAIL
  eg2_status_expect_alert(alert);
#endif
  eg2_status_raise_alert(alert);

  for (uint32_t i = 0; i < EG2_PARAM(num_intrs); ++i) {
    const intr_row_t *row =
        &kIntrRows[eg2_rand_range(0, sizeof(kIntrRows) / sizeof(kIntrRows[0]) - 1)];
    uint32_t bit = pick_bit(row->mask);
    eg2_puts("raising interrupt ");
    eg2_puts(eg2_dev_name(row->dev));
    eg2_puts(" 0x");
    eg2_puthex(bit);
    eg2_puts("\r\n");
    eg2_status_expect_intr(row->dev, bit);
    eg2_status_raise_intr(row->dev, bit);
  }

  ok = step("alert arrived", eg2_status_wait_alert(alert, kAlertPolls)) && ok;
  ok = step("exactly the raised alert and interrupts", eg2_status_check()) && ok;

  eg2_status_clear_expected();
  ok = step("clean after clearing", eg2_status_check()) && ok;
  return ok;
}
