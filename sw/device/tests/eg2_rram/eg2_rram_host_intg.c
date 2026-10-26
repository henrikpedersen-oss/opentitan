// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_host_intg (EG2 verification spec REQ_RRAM_12): bus integrity
 * runs from the RRAM read buffer to the core unchanged, so a bit corrupted in
 * the read buffer is detected by the core, on the data path and on the fetch
 * path.
 *
 * Documents: RFC RRAM §4.5 p12; rram_ctrl theory_of_operation.md "RRAM Data
 * Path" ("ECC-protected read-buffer data ... end-to-end protected with no ECC
 * recomputation along the way"), "RRAM Read Buffer" (an entry holds four 39-bit
 * words: data and its bus integrity); Ibex security.rst "Bus integrity
 * checking" (a mismatch: an internal interrupt and a bus major alert; a load's
 * destination register is not written) and exception_interrupts.rst "Internal
 * Interrupts" (mcause 0xFFFFFFE0, mtval the address, for a load);
 * rv_core_ibex.hjson (the bus major alert is fatal_hw_err,
 * ERR_STATUS.FATAL_INTG_ERR: "detected a response integrity error").
 *
 * The data path: a word of the scratch region is loaded once (into the read
 * buffer), then `EG2 RRAM host_intg data off=0x<o>`: the vseq flips one data
 * bit of that word in the read-buffer entry that holds it (the entry verified,
 * so the shadow read is done); the word is loaded again (a buffer hit): the
 * load ends in the internal interrupt (mcause 0xFFFFFFE0, mtval the address).
 * The fetch path: the RRAM word of a routine never executed is loaded as data
 * (into the buffer, not the iCache), `EG2 RRAM host_intg fetch off=0x<o>`, the
 * vseq flips a bit of its first instruction in the buffer, and the routine is
 * called: the fetch traps (an instruction access fault, or the internal
 * interrupt: Ibex's documentation lists the internal interrupt for loads only;
 * either is accepted and the cause is logged). Both: rv_core_ibex_fatal_hw_err
 * at the alert handler, ERR_STATUS.FATAL_INTG_ERR set, and rram_ctrl reports no
 * fault of its own (FAULT_STATUS 0: the corruption is after its checks).
 *
 * The iCache is on during both, and the code between the sync line and the
 * access has run once before (the warm-up pass), so the core's own fetches do
 * not evict the buffer entry.
 *
 * fatal_hw_err keeps firing until reset, so after `EG2 RRAM host_intg done` the
 * vseq resets the chip (boot_phase = 1); boot 1 checks a clean state:
 * ERR_STATUS 0, no rram_ctrl fault, both words read and execute.
 *
 * Knobs:
 *   paths       1 data, 2 fetch, 3 both
 *   boot_phase  written by the vseq before the reset
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                     | Why |
 * |-------------------------------------------------|-----------------------------------------|
 * | the traps, the alert, ERR_STATUS and the second | no bench corrupts the
 * buffer, and       | |   boot; the model run checks the loads and the  |
 * eg2sim has no read buffer or bus      | |   call give the data and the code's
 * result      |   integrity                             |
 *
 * Built with EG2_RRAM_HOST_INTG_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/rv_core_ibex_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define HOST_INTG_PARAMS(X) \
  X(paths, 3, 1, 3)         \
  X(boot_phase, 0, 0, 1)    \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(HOST_INTG_PARAMS)

enum {
  kPolls = 200000,
  kPage = kEg2RramScratchFirstPage + 20,
  kVerifyWait = 400,  // cycles: the shadow read of a fresh buffer entry
  kResetWait = 4000000,
  kNmiLoadIntg = 0xFFFFFFE0,
  kFatalIntg = 1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT,
};

// RRAM words of the page: 0 and 1 the warm-up's data and routine, 2 and 3 the
// real ones.
static uint32_t woff(uint32_t slot) { return eg2_rram_addr(kPage, 4 * slot); }

static bool write_page(void) {
  uint32_t w[16];
  for (uint32_t s = 0; s < 4; ++s) {
    w[4 * s] =
        (s & 1) ? eg2_rram_insn_addi_a0(5) : eg2_rram_pattern(0x1a7, 4 * s);
    w[4 * s + 1] =
        (s & 1) ? kEg2ChkStubRetNop : eg2_rram_pattern(0x1a7, 4 * s + 1);
    w[4 * s + 2] = eg2_rram_pattern(0x1a7, 4 * s + 2);
    w[4 * s + 3] = eg2_rram_pattern(0x1a7, 4 * s + 3);
  }
  eg2_rram_result_t r;
  bool ok = eg2_chk_true("page write done",
                         eg2_rram_write(kEg2RramPartData, woff(0), w, 16, &r));
  return eg2_chk_eq("page write error code", 0, r.err_code) && ok;
}

/**
 * The data access: load, wait for the shadow read, sync, load again. With
 * `sync` false it is the warm-up of the same code. Returns the second load's
 * mcause; `*val` its value.
 */
static uint32_t data_access(uint32_t slot, bool sync, uint32_t *val) {
  eg2_chk_trap_t t;
  (void)eg2_chk_access(eg2_rram_host(woff(slot)), kEg2ChkLw, 0, &t);
  eg2_chk_delay(kVerifyWait);
  if (sync) {
    LOG_INFO("EG2 RRAM host_intg data off=0x%x", woff(slot));
  } else {
    LOG_INFO("EG2 RRAM host_intg warm off=0x%x", woff(slot));
  }
  uint32_t cause = eg2_chk_access(eg2_rram_host(woff(slot)), kEg2ChkLw, 0, &t);
  *val = t.value;
  return cause;
}

/** The fetch: the routine's word loaded as data, sync, the routine called. */
static uint32_t fetch_access(uint32_t slot, bool sync, uint32_t *val) {
  eg2_chk_trap_t t;
  (void)eg2_chk_access(eg2_rram_host(woff(slot)), kEg2ChkLw, 0, &t);
  eg2_chk_delay(kVerifyWait);
  if (sync) {
    LOG_INFO("EG2 RRAM host_intg fetch off=0x%x", woff(slot));
  } else {
    LOG_INFO("EG2 RRAM host_intg warm off=0x%x", woff(slot));
  }
  uint32_t cause = eg2_chk_call(kEg2RramHostBase + woff(slot), 10, &t);
  *val = t.value;
  return cause;
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
  uint32_t paths = EG2_PARAM(paths);
  eg2_chk_step("boot phase", EG2_PARAM(boot_phase));

  if (EG2_PARAM(boot_phase) == 1) {
    ok = eg2_chk_eq("ERR_STATUS after the reset", 0,
                    eg2_dev_read32(kEg2DevRvCoreIbexCfg,
                                   RV_CORE_IBEX_ERR_STATUS_REG_OFFSET)) &&
         ok;
    ok = eg2_chk_eq("rram FAULT_STATUS after the reset", 0,
                    eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
         ok;
    ok = eg2_chk_eq("data word", eg2_rram_pattern(0x1a7, 8),
                    *eg2_rram_host(woff(2))) &&
         ok;
    eg2_chk_fence_i();
    eg2_chk_trap_t t;
    ok = eg2_chk_eq("routine: no trap", 0,
                    eg2_chk_call(kEg2RramHostBase + woff(3), 10, &t)) &&
         ok;
    ok = eg2_chk_eq("routine result", 15, t.value) && ok;
    eg2_rram_region_off(region);
    return eg2_chk_summary(6) && ok;
  }

  ok = write_page() && ok;
  bool was_on = eg2_rram_icache_set(true);
  int alert = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  if (!eg2_chk_true("rv_core_ibex_fatal_hw_err known", alert >= 0)) {
    return false;
  }
  uint32_t val;
  // Warm-up passes: the same code, on the warm-up words, no corruption.
  if (paths & 1) {
    ok = eg2_chk_eq("warm-up load: no trap", 0, data_access(0, false, &val)) &&
         ok;
    ok =
        eg2_chk_eq("warm-up load value", eg2_rram_pattern(0x1a7, 0), val) && ok;
  }
  if (paths & 2) {
    ok = eg2_chk_eq("warm-up call: no trap", 0, fetch_access(1, false, &val)) &&
         ok;
    ok = eg2_chk_eq("warm-up call result", 15, val) && ok;
  }
  if (!gaps) {
    eg2_status_expect_alert((size_t)alert);
  }
  if (paths & 1) {
    uint32_t cause = data_access(2, true, &val);
    eg2_chk_step("corrupted load mcause", cause);
    if (gaps) {
      eg2_chk_skip(
          "the load integrity trap (no read buffer, no bus integrity in the "
          "model)");
      ok = eg2_chk_eq("load value", eg2_rram_pattern(0x1a7, 8), val) && ok;
    } else {
      ok = eg2_chk_eq("corrupted load: internal interrupt", kNmiLoadIntg,
                      cause) &&
           ok;
    }
  }
  if (paths & 2) {
    uint32_t cause = fetch_access(3, true, &val);
    eg2_chk_step("corrupted fetch mcause", cause);
    if (gaps) {
      eg2_chk_skip(
          "the fetch integrity trap (no read buffer, no bus integrity in the "
          "model)");
      ok = eg2_chk_eq("routine result", 15, val) && ok;
    } else {
      ok =
          eg2_chk_true(
              "corrupted fetch: instruction access fault or internal interrupt",
              cause == kEg2ChkCauseFetchFault || cause == kNmiLoadIntg) &&
          ok;
    }
  }
  eg2_rram_icache_set(was_on);
  if (gaps) {
    eg2_rram_region_off(region);
    return eg2_chk_summary(4) && ok;
  }
  ok = eg2_chk_true("rv_core_ibex_fatal_hw_err at the alert handler",
                    eg2_status_wait_alert((size_t)alert, 10000)) &&
       ok;
  ok = eg2_chk_eq("ERR_STATUS.FATAL_INTG_ERR", kFatalIntg,
                  eg2_dev_read32(kEg2DevRvCoreIbexCfg,
                                 RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) &
                      kFatalIntg) &&
       ok;
  ok = eg2_chk_eq("rram_ctrl reports no fault of its own", 0,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_summary(6) && ok;
  if (!ok) {
    return false;
  }
  EG2_CHK_SYNC("EG2 RRAM host_intg done");
  eg2_chk_delay(kResetWait);
  return eg2_chk_true("the bench reset the chip", false);
}
