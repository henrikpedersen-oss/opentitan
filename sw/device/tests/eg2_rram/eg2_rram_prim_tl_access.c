// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_prim_tl_access (EG2 verification spec REQ_RRAM_25): the macro's
 * register window `rram_macro.prim` is reachable, and a macro error or a `prim`
 * integrity fault reaches rram_ctrl's fatal_macro_err / recov_macro_err alerts.
 *
 * Documents: RFC RRAM §3.1 p7; rram_macro.hjson (the `prim` device port and its
 * registers: CSR0_REGWEN rw0c reset 1, CSR1 rw [12:0] reset 0 locked by
 * CSR0_REGWEN, CSR2 rw1c / rw bits); rram_ctrl.hjson (the alerts
 * fatal_macro_err and recov_macro_err from the macro); README "rram_macro" (the
 * macro's own errors); the TL-UL bus integrity scheme (a command integrity
 * error at a device is a fatal fault of that device).
 *
 * Boot 0:
 *   1. reset values: CSR0_REGWEN 1, CSR1 0, CSR2 0, each at the drawn width
 * order;
 *   2. CSR1 written with a drawn value, read back (its 13 bits); CSR2's rw1c
 * bits written with 1s stay 0 (no hardware sets them);
 *   3. with `lock`: CSR0_REGWEN written 0 (rw0c); a new CSR1 write leaves the
 * old value; a 1 written to CSR0_REGWEN does not reopen it;
 *   4. with `fault` (chip only): `EG2 RRAM prim intg`, then CSR1 reads: the
 * vseq flips the command integrity of one at the register block's input;
 * rram_ctrl_fatal_macro_err must reach the alert handler; then `EG2 RRAM prim
 * done` and the vseq resets the chip (boot_phase = 1) since a fatal alert fires
 * until reset. Boot 1: the reset values again (the lock gone), and no alert.
 *
 * recov_macro_err has no stimulus at chip level: the open-source emulation
 * macro ties its recoverable error to 0 (rram_macro.sv `recov_err = 1'b0`), so
 * the requirement's recoverable half cannot pass on this RTL (proposed ticket:
 * the emulation macro raises no recoverable error).
 *
 * Knobs:
 *   lock        1: step 3
 *   fault       1: step 4 and the second boot (chip only)
 *   boot_phase  written by the vseq before the reset
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1 | Why |
 * |-----------------------------|--------------------------------------------------------------|
 * | (none; fault stays 0)       | eg2sim models rram_macro.prim as its
 * generated register block |
 *
 * Built with EG2_RRAM_PRIM_TL_ACCESS_INJECT_FAIL the first value check expects
 * bit 0 flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/rram_macro_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define PRIM_PARAMS(X)   \
  X(lock, 1, 0, 1)       \
  X(fault, 0, 0, 1)      \
  X(boot_phase, 0, 0, 1) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(PRIM_PARAMS)

enum {
  kCsr1Mask = 0x1fff,
  kCsr2Rw1c = 0x77,  // fields 0-2 and 4-6 (rw1c); 3 and 7 are rw
  kResetWait = 4000000,
};

static uint32_t prim_rd(uint32_t off) {
  return eg2_chk_rd32(kEg2RramPrimBase + off);
}
static void prim_wr(uint32_t off, uint32_t v) {
  eg2_chk_wr32(kEg2RramPrimBase + off, v);
}

static bool reset_values(void) {
  bool ok = eg2_chk_eq("CSR0_REGWEN reset", RRAM_MACRO_CSR0_REGWEN_REG_RESVAL,
                       prim_rd(RRAM_MACRO_CSR0_REGWEN_REG_OFFSET));
  ok = eg2_chk_eq("CSR1 reset", RRAM_MACRO_CSR1_REG_RESVAL,
                  prim_rd(RRAM_MACRO_CSR1_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("CSR2 reset", RRAM_MACRO_CSR2_REG_RESVAL,
                  prim_rd(RRAM_MACRO_CSR2_REG_OFFSET)) &&
       ok;
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  eg2_chk_step("boot phase", EG2_PARAM(boot_phase));
  bool ok = reset_values();
  if (EG2_PARAM(boot_phase) == 1) {
    return eg2_chk_summary(3) && ok;
  }
  uint32_t v = eg2_rand() & kCsr1Mask;
  if (v == 0) {
    v = 1;
  }
  prim_wr(RRAM_MACRO_CSR1_REG_OFFSET, v | ~(uint32_t)kCsr1Mask);
  ok = eg2_chk_eq("CSR1 write and read back", v,
                  prim_rd(RRAM_MACRO_CSR1_REG_OFFSET)) &&
       ok;
  prim_wr(RRAM_MACRO_CSR2_REG_OFFSET, kCsr2Rw1c);
  ok = eg2_chk_eq("CSR2 rw1c bits written 1 stay 0", 0,
                  prim_rd(RRAM_MACRO_CSR2_REG_OFFSET) & kCsr2Rw1c) &&
       ok;
  if (EG2_PARAM(lock)) {
    prim_wr(RRAM_MACRO_CSR0_REGWEN_REG_OFFSET, 0);
    ok = eg2_chk_eq("CSR0_REGWEN cleared", 0,
                    prim_rd(RRAM_MACRO_CSR0_REGWEN_REG_OFFSET)) &&
         ok;
    prim_wr(RRAM_MACRO_CSR1_REG_OFFSET, ~v & kCsr1Mask);
    ok =
        eg2_chk_eq("CSR1 locked", v, prim_rd(RRAM_MACRO_CSR1_REG_OFFSET)) && ok;
    prim_wr(RRAM_MACRO_CSR0_REGWEN_REG_OFFSET, 1);
    ok = eg2_chk_eq("CSR0_REGWEN not reopened (rw0c)", 0,
                    prim_rd(RRAM_MACRO_CSR0_REGWEN_REG_OFFSET)) &&
         ok;
  }
  if (!EG2_PARAM(fault) || EG2_PARAM(model_gaps)) {
    return eg2_chk_summary(5) && ok;
  }
  int alert = eg2_status_alert_find("rram_ctrl_fatal_macro_err");
  if (!eg2_chk_true("rram_ctrl_fatal_macro_err known", alert >= 0)) {
    return false;
  }
  eg2_status_expect_alert((size_t)alert);
  eg2_chk_irq_off();
  EG2_CHK_SYNC("EG2 RRAM prim intg");
  // CSR1 reads until the alert: one of them reaches the macro with a bad
  // command integrity.
  bool seen = false;
  for (uint32_t i = 0; i < 2000 && !seen; ++i) {
    eg2_chk_trap_t t;
    uint32_t cause = eg2_chk_access(
        eg2_chk_ptr(kEg2RramPrimBase + RRAM_MACRO_CSR1_REG_OFFSET), kEg2ChkLw,
        0, &t);
    if (cause != 0) {
      eg2_chk_step("the corrupted read trapped, mcause", cause);
    }
    seen = eg2_status_wait_alert((size_t)alert, 1);
  }
  ok = eg2_chk_true("rram_ctrl_fatal_macro_err at the alert handler",
                    seen || eg2_status_wait_alert((size_t)alert, 10000)) &&
       ok;
  ok = eg2_chk_summary(6) && ok;
  if (!ok) {
    return false;
  }
  EG2_CHK_SYNC("EG2 RRAM prim done");
  eg2_chk_delay(kResetWait);
  return eg2_chk_true("the bench reset the chip", false);
}
