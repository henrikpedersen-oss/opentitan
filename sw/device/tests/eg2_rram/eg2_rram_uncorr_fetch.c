// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_uncorr_fetch (EG2 verification spec REQ_RRAM_10): an
 * uncorrectable error raises `fatal_err`; RRAM is disabled only once
 * DIS.LOCAL_ESC_RELBL_ERR is set, and FAULT_STATUS.PHY_RELBL_ERR is clearable
 * before that.
 *
 * Documents: RFC RRAM §4.6.2 p18; rram_ctrl theory_of_operation.md "Multi-Bit
 * Error (Uncorrectable)" and "RRAM Escalation & Disable" (the fatal alert
 * regardless of the setting; local escalation only with LOCAL_ESC_RELBL_ERR not
 * MuBi4False; once disabled, host and controller requests are blocked until
 * reset, the registers stay readable), programmers_guide.md "Error Encountered
 * by Host Direct Read" (an uncorrectable error is an in-band TL-UL error: a
 * load access fault, or an instruction access fault on a fetch),
 * rram_ctrl.hjson (FAULT_STATUS.phy_relbl_err rw0c; DIS rw1s).
 *
 * Boot 0, with DIS.LOCAL_ESC_RELBL_ERR at its reset value MuBi4False:
 *   for each access of the run (a load and a fetch, in the drawn order), a
 * fresh RRAM word in the scratch region (RD, WR, ECC) is written (with `addi
 * a0, a0, 1 ; c.jr ra` for the fetch), then `EG2 RRAM uncorr off=0x<o>`: the
 * vseq flips two data bits in one 64-bit half (uncorrectable), then
 *     - the load gets a load access fault (mcause 5, mtval the address), the
 * call an instruction access fault (mcause 1, mtval the target);
 *     - FAULT_STATUS.PHY_RELBL_ERR is set and the rram_ctrl_fatal_err alert
 * reached the alert handler;
 *     - writing 0 clears PHY_RELBL_ERR; a clean word in the same region still
 * reads (host) and a controller read and write still complete: RRAM is not
 * disabled. Then with `escalate`: DIS.LOCAL_ESC_RELBL_ERR set (rw1s, MuBi4True
 * written), read back not MuBi4False, and `EG2 RRAM uncorr esc off=0x<o>` with
 * a load of that word: the vseq flips two bits again, the load faults and RRAM
 * is disabled, so the core can fetch nothing more from it (the test itself runs
 * from RRAM). The vseq checks that rram_ctrl is disabled and that FAULT_STATUS
 * is still readable (backdoor), repairs the rows it corrupted, and resets the
 * chip (boot_phase = 1). Boot 1: a clean state: FAULT_STATUS, STD_FAULT_STATUS
 * and ERR_CODE are 0, DIS is at its reset value, the words corrupted in boot 0
 * read back as written and execute.
 *
 * The fatal alert keeps firing until reset (a fatal alert), so boot 0 never
 * passes on its own on the chip: the run's verdict is boot 1's. Without
 * `escalate` the vseq resets after the two accesses (`EG2 RRAM uncorr done`).
 *
 * Knobs:
 *   order       0 load first, 1 fetch first
 *   escalate    1: the local-escalation step (chip only)
 *   boot_phase  written by the vseq before the reset
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                     | Why |
 * |-------------------------------------------------|-----------------------------------------|
 * | the faults, PHY_RELBL_ERR, the alert, the second| no bench injects, and
 * eg2sim's rram_ctrl| |   boot; the model run checks the words read and |
 * models no ECC (ip/eg2_mem.h)          | |   execute, PHY_RELBL_ERR's rw0c
 * clear leaves 0, |                                         | |
 * DIS.LOCAL_ESC_RELBL_ERR reads back set        | |
 *
 * Built with EG2_RRAM_UNCORR_FETCH_INJECT_FAIL the first value check expects
 * bit 0 flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define UNCORR_PARAMS(X) \
  X(order, 0, 0, 1)      \
  X(escalate, 0, 0, 1)   \
  X(boot_phase, 0, 0, 1) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(UNCORR_PARAMS)

enum {
  kPolls = 200000,
  kWait = 2000,
  kResetWait = 4000000,
  kPage = kEg2RramScratchFirstPage + 16,
  kRelbl = 1u << 9,  // FAULT_STATUS.phy_relbl_err
};

/** The RRAM word `slot` of the test's page: its byte offset. */
static uint32_t slot_off(uint32_t slot) {
  return eg2_rram_addr(kPage, 4 * slot);
}

/** The words written at `slot`: code (`addi a0, a0, 1 ; c.jr ra`) then pattern
 * words. */
static void slot_words(uint32_t slot, uint32_t *w) {
  w[0] = eg2_rram_insn_addi_a0(1);
  w[1] = kEg2ChkStubRetNop;
  w[2] = eg2_rram_pattern(0x0dead + slot, 2);
  w[3] = eg2_rram_pattern(0x0dead + slot, 3);
}

static bool write_slot(uint32_t slot) {
  uint32_t w[4];
  slot_words(slot, w);
  eg2_rram_result_t r;
  bool ok =
      eg2_chk_true("word write done",
                   eg2_rram_write(kEg2RramPartData, slot_off(slot), w, 4, &r));
  return eg2_chk_eq("word write error code", 0, r.err_code) && ok;
}

/** The word still executes and reads back as written. */
static bool slot_intact(uint32_t slot) {
  uint32_t w[4];
  slot_words(slot, w);
  eg2_chk_fence_i();
  eg2_chk_trap_t t;
  bool ok = eg2_chk_eq("word executes: no trap", 0,
                       eg2_chk_call(kEg2RramHostBase + slot_off(slot), 41, &t));
  ok = eg2_chk_eq("word executes: a0 + 1", 42, t.value) && ok;
  return eg2_chk_eq("word data", w[3], *eg2_rram_host(slot_off(slot) + 12)) &&
         ok;
}

static bool access_uncorr(uint32_t slot, bool fetch, bool gaps, size_t alert) {
  bool ok = write_slot(slot);
  LOG_INFO("EG2 RRAM uncorr off=0x%x", slot_off(slot));
  eg2_chk_delay(kWait);
  eg2_chk_fence_i();
  eg2_chk_trap_t t;
  uint32_t addr = kEg2RramHostBase + slot_off(slot);
  uint32_t cause;
  if (fetch) {
    cause = eg2_chk_call(addr, 0, &t);
  } else {
    cause =
        eg2_chk_access(eg2_rram_host(slot_off(slot) + 12), kEg2ChkLw, 0, &t);
    addr += 12;
  }
  eg2_chk_step(fetch ? "uncorrectable fetch at" : "uncorrectable load at",
               addr);
  if (gaps) {
    eg2_chk_skip(
        "the fault, PHY_RELBL_ERR and fatal_err (no ECC in the model)");
    return ok;
  }
  ok =
      eg2_chk_eq(
          fetch ? "fetch: instruction access fault" : "load: load access fault",
          fetch ? kEg2ChkCauseFetchFault : kEg2ChkCauseLoadFault, cause) &&
      ok;
  ok = eg2_chk_eq("mtval", addr, t.mtval) && ok;
  ok = eg2_chk_eq("FAULT_STATUS.PHY_RELBL_ERR set", kRelbl,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET) & kRelbl) &&
       ok;
  ok = eg2_chk_true("rram_ctrl_fatal_err at the alert handler",
                    eg2_status_wait_alert(alert, 10000)) &&
       ok;
  // rw0c: writing 0 clears it.
  eg2_rram_reg_wr(RRAM_CTRL_FAULT_STATUS_REG_OFFSET, 0);
  ok = eg2_chk_eq("PHY_RELBL_ERR cleared", 0,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET) & kRelbl) &&
       ok;
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls));
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  eg2_rram_region_set(region, kPage, 1,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
  eg2_chk_step("boot phase", EG2_PARAM(boot_phase));

  if (EG2_PARAM(boot_phase) == 1) {
    // After the reset: a clean controller, and the repaired words intact.
    ok = eg2_chk_eq("FAULT_STATUS after the reset", 0,
                    eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
         ok;
    ok = eg2_chk_eq("STD_FAULT_STATUS after the reset", 0,
                    eg2_rram_reg(RRAM_CTRL_STD_FAULT_STATUS_REG_OFFSET)) &&
         ok;
    ok = eg2_chk_eq("ERR_CODE after the reset", 0,
                    eg2_rram_reg(RRAM_CTRL_ERR_CODE_REG_OFFSET)) &&
         ok;
    ok = eg2_chk_eq("DIS after the reset", RRAM_CTRL_DIS_REG_RESVAL,
                    eg2_rram_reg(RRAM_CTRL_DIS_REG_OFFSET)) &&
         ok;
    for (uint32_t s = 0; s < 3; ++s) {
      ok = slot_intact(s) && ok;
    }
    eg2_rram_region_off(region);
    return eg2_chk_summary(10) && ok;
  }

  int alert = eg2_status_alert_find("rram_ctrl_fatal_err");
  if (!eg2_chk_true("rram_ctrl_fatal_err known", alert >= 0)) {
    return false;
  }
  if (!gaps) {
    eg2_status_expect_alert((size_t)alert);
  }
  bool fetch_first = EG2_PARAM(order) == 1;
  ok = access_uncorr(0, fetch_first, gaps, (size_t)alert) && ok;
  ok = access_uncorr(1, !fetch_first, gaps, (size_t)alert) && ok;
  // Not disabled: a clean word still reads and executes, the controller still
  // works.
  ok = write_slot(2) && ok;
  ok = slot_intact(2) && ok;

  // DIS.LOCAL_ESC_RELBL_ERR: rw1s, MuBi4True written; any value but MuBi4False
  // enables it.
  if (EG2_PARAM(escalate) || gaps) {
    eg2_rram_reg_wr(
        RRAM_CTRL_DIS_REG_OFFSET,
        (uint32_t)kEg2RramMubiTrue << RRAM_CTRL_DIS_LOCAL_ESC_RELBL_ERR_OFFSET);
    uint32_t esc = (eg2_rram_reg(RRAM_CTRL_DIS_REG_OFFSET) >>
                    RRAM_CTRL_DIS_LOCAL_ESC_RELBL_ERR_OFFSET) &
                   RRAM_CTRL_DIS_LOCAL_ESC_RELBL_ERR_MASK;
    ok =
        eg2_chk_true("DIS.LOCAL_ESC_RELBL_ERR set", esc != kEg2RramMubiFalse) &&
        ok;
    uint32_t sw_dis =
        eg2_rram_reg(RRAM_CTRL_DIS_REG_OFFSET) & RRAM_CTRL_DIS_SW_DIS_MASK;
    ok = eg2_chk_eq("DIS.SW_DIS untouched", kEg2RramMubiFalse, sw_dis) && ok;
  }
  if (gaps) {
    eg2_chk_skip(
        "local escalation and the second boot (no ECC, no bench in the model)");
    eg2_rram_region_off(region);
    return eg2_chk_summary(10) && ok;
  }
  if (!ok) {
    // Report the failure in this boot rather than in the next one.
    return eg2_chk_summary(10) && ok;
  }
  if (EG2_PARAM(escalate)) {
    eg2_chk_summary(10);
    // From here RRAM is disabled: nothing below the load runs again before the
    // reset.
    LOG_INFO("EG2 RRAM uncorr esc off=0x%x", slot_off(0));
    eg2_chk_delay(kWait);
    eg2_chk_trap_t t;
    (void)eg2_chk_access(eg2_rram_host(slot_off(0) + 12), kEg2ChkLw, 0, &t);
  } else {
    eg2_chk_summary(10);
    EG2_CHK_SYNC("EG2 RRAM uncorr done");
  }
  eg2_chk_delay(kResetWait);
  return eg2_chk_true("the bench reset the chip", false);
}
