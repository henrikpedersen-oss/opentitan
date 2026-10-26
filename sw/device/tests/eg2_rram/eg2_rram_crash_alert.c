// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_crash_alert (EG2 verification spec REQ_RRAM_23): a global
 * escalation or a local fatal fault blocks host and controller access to the
 * RRAM, the controller's registers stay readable, and the escalation reset
 * leaves a clean state.
 *
 * Documents: rram_ctrl theory_of_operation.md "RRAM Escalation & Disable"
 * (global escalation: all further accesses disabled; local escalation on any
 * STD_FAULT_STATUS or FAULT_STATUS bit, latched until reset; "RRAM controller
 * registers remain accessible after disable"), "Transmission Integrity Faults"
 * (a register-access integrity error: fatal_std_err), rram_ctrl.hjson sec_cm
 * GLOBAL_ESC and LOCAL_ESC; alert_handler documentation (classes, escalation
 * phases, the signal each phase drives: 1 lc_ctrl's escalation, which gives
 * lc_escalate_en, 3 pwrmgr's escalation reset); pwrmgr_reg_pkg's reset request
 * order (escalation = HW_REQ index 3, RESET_INFO bit 6).
 *
 * Boot 0, `fault` drawn:
 *   0 global: an alert (uart0_fatal_fault through ALERT_TEST) in class A, which
 * escalates at once: phase 0 drives lc_ctrl's escalation (lc_escalate_en
 * reaches rram_ctrl) for phase0_cycles, phase 1 the escalation reset. `EG2 RRAM
 * crash global` before the trigger: the vseq checks rram_ctrl's lc_escalate_en
 * input goes On and that every host_tl response from then on until the reset is
 * an error (the core's own fetches, if it still runs: lc_ctrl's documentation
 *     disagrees on whether the core stops, ticket F19).
 *   1 local: rram_ctrl_fatal_std_err in class A, escalating to the reset after
 * phase0_cycles. `EG2 RRAM crash local`, then a loop of SCRATCH reads: the vseq
 * corrupts one such read's command integrity on rram_ctrl's core_tl (one
 * cycle), so STD_FAULT_STATUS.reg_intg_err latches the local escalation; RRAM
 * is disabled and the core fetches nothing more. Over SBA the vseq checks a
 * host read of the RRAM is refused, a controller read returns no data, and
 *     STD_FAULT_STATUS and FAULT_STATUS still read (reg_intg_err set).
 * The alert handler resets the chip. Boot 1 is recognised by the escalation
 * reset in the reset reasons the boot stage kept in the retention SRAM (it
 * clears RESET_INFO itself); then STD_FAULT_STATUS, FAULT_STATUS and ERR_CODE
 * 0; DIS at its reset value; a controller write and read and a host read of a
 * scratch word work; no alert.
 *
 * Knobs:
 *   fault          0 global, 1 local, 2 drawn
 *   phase0_cycles  the escalation phase before the reset (alert-handler clock
 * cycles), 2000-40000 model_gaps     skips what the SystemC model does not
 * model:
 *
 * | Skipped with model_gaps = 1                    | Why |
 * |------------------------------------------------|-------------------------------------------|
 * | fault 1's corruption (no bench) and the RRAM   | eg2sim's rram_ctrl models
 * no escalation   | |   blocking; the model run makes fault 0: the   |
 * (ip/eg2_mem.h); its alert handler       | |   escalation and its reset, and
 * boot 1's checks|   escalates and resets (README)           |
 *
 * Built with EG2_RRAM_CRASH_ALERT_INJECT_FAIL the first value check expects bit
 * 0 flipped: the run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define CRASH_PARAMS(X)                \
  X(fault, 2, 0, 2)                    \
  X(phase0_cycles, 20000, 2000, 40000) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(CRASH_PARAMS)

enum {
  kPolls = 200000,
  kResetWait = 4000000,
  kEscResetInfo =
      1u << (RSTMGR_RESET_INFO_HW_REQ_OFFSET + 3),  // pwrmgr ResetEscIdx = 3
  kSigLc = 1,     // escalation signal 1: lc_ctrl
  kSigReset = 3,  // escalation signal 3: pwrmgr's escalation reset
  kSigNone = 0xff,
  kPage = kEg2RramScratchFirstPage + 28,
};

static void ah_wr_shadowed(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevAlertHandler, off, v);
  eg2_dev_write32(kEg2DevAlertHandler, off, v);
}

/**
 * Class A escalates at the first alert: signal `sig0` (kSigNone: none) in phase
 * 0, for phase0_cycles, then the reset (signal 3) in phase 1.
 */
static void class_a(size_t alert, uint32_t sig0) {
  ah_wr_shadowed(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * alert,
                 0);  // class A
  ah_wr_shadowed(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * alert, 1);
  ah_wr_shadowed(ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0);
  ah_wr_shadowed(ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0);
  ah_wr_shadowed(ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET,
                 EG2_PARAM(phase0_cycles));
  ah_wr_shadowed(ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET, 100);
  // EN_Ex enables escalation signal x, MAP_Ex is the phase it is asserted in
  // (alert_handler.hjson). The fields of signal x are EN_E0 + x and MAP_E0 +
  // 2x.
  uint32_t ctrl = 1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT;
  if (sig0 != kSigNone) {
    ctrl |=
        1u << (ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E0_BIT + sig0);  // phase 0
  }
  {
    ctrl |= (1u << (ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E0_BIT + kSigReset)) |
            1u << (ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E0_OFFSET +
                   2 * kSigReset);  // phase 1
  }
  ah_wr_shadowed(ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET, ctrl);
}

/**
 * The reset reasons of this boot. The boot stage (test ROM, ROM) reads
 * RESET_INFO, keeps it in the retention SRAM's creator area (retention_sram.h:
 * creator.reset_reasons, at offset 4) and clears the register.
 */
static uint32_t boot_reset_reasons(void) {
  return eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 4);
}

static bool clean_state(void) {
  bool ok = true;
  uint32_t info = boot_reset_reasons();
  eg2_chk_step("reset reasons", info);
  ok = eg2_chk_eq("reset reasons: the escalation reset", kEscResetInfo,
                  info & kEscResetInfo) &&
       ok;
  ok = eg2_chk_eq("STD_FAULT_STATUS after the reset", 0,
                  eg2_rram_reg(RRAM_CTRL_STD_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("FAULT_STATUS after the reset", 0,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("ERR_CODE after the reset", 0,
                  eg2_rram_reg(RRAM_CTRL_ERR_CODE_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("DIS after the reset", RRAM_CTRL_DIS_REG_RESVAL,
                  eg2_rram_reg(RRAM_CTRL_DIS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls)) && ok;
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  eg2_rram_region_set(region, kPage, 1,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
  uint32_t w[4], rd[4];
  for (uint32_t i = 0; i < 4; ++i) {
    w[i] = eg2_rand();
  }
  eg2_rram_result_t r;
  ok = eg2_chk_true("controller write done",
                    eg2_rram_write(kEg2RramPartData, eg2_rram_addr(kPage, 0), w,
                                   4, &r)) &&
       ok;
  ok = eg2_chk_eq("controller write error code", 0, r.err_code) && ok;
  ok = eg2_chk_true("controller read done",
                    eg2_rram_read(kEg2RramPartData, eg2_rram_addr(kPage, 0), rd,
                                  4, &r)) &&
       ok;
  for (uint32_t i = 0; i < 4; ++i) {
    ok = eg2_chk_eq("controller read back", w[i], rd[i]) && ok;
    ok = eg2_chk_eq("host read back", w[i],
                    *eg2_rram_host(eg2_rram_addr(kPage, i))) &&
         ok;
  }
  eg2_rram_region_off(region);
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  if (boot_reset_reasons() & kEscResetInfo) {
    eg2_chk_step("boot", 1);
    bool ok1 = clean_state();
    return eg2_chk_summary(10) && ok1;
  }
  eg2_chk_step("boot", 0);
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t fault =
      EG2_PARAM(fault) > 1 ? eg2_rand_range(0, 1) : EG2_PARAM(fault);
  if (gaps) {
    if (fault == 1) {
      eg2_chk_skip(
          "the local fault (no bench in the model): the global escalation "
          "instead");
    }
    fault = 0;
  }
  eg2_chk_step("fault (0 global, 1 local)", fault);
  bool ok = eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls));
  ok = eg2_chk_eq("no fault before the test", 0,
                  eg2_rram_reg(RRAM_CTRL_STD_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  if (!ok) {
    return eg2_chk_summary(2) && ok;
  }
  if (fault == 0) {
    int alert = eg2_status_alert_find("uart0_fatal_fault");
    if (!eg2_chk_true("uart0_fatal_fault known", alert >= 0)) {
      return false;
    }
    class_a((size_t)alert, kSigLc);
    EG2_CHK_SYNC("EG2 RRAM crash global");
    eg2_status_raise_alert((size_t)alert);
  } else {
    int alert = eg2_status_alert_find("rram_ctrl_fatal_std_err");
    if (!eg2_chk_true("rram_ctrl_fatal_std_err known", alert >= 0)) {
      return false;
    }
    class_a((size_t)alert, kSigNone);
    EG2_CHK_SYNC("EG2 RRAM crash local");
    // SCRATCH reads until the corrupted one disables RRAM (and with it this
    // loop's fetches).
    for (uint32_t i = 0; i < kResetWait / 8; ++i) {
      (void)eg2_rram_reg(RRAM_CTRL_SCRATCH_REG_OFFSET);
    }
  }
  eg2_chk_delay(kResetWait);
  return eg2_chk_true("the escalation reset the chip", false);
}
