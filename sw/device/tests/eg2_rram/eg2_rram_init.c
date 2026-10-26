// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_init (EG2 verification spec REQ_RRAM_21, REQ_RRAM_27): after
 * INIT the controller has its scrambling keys and has read the seeds only with
 * lc_seed_hw_rd_en; the keys are never software-readable; when RRAM is disabled
 * the scrambler takes random keys and the seeds are no longer valid.
 *
 * Documents: RFC RRAM §4.4 p10; rram_ctrl theory_of_operation.md
 * "Initialization" (StReqAddrKey, StReqDataKey: keys_valid; StReadSeeds only
 * with lc_seed_hw_rd_en, each seed read twice, seed_valid only for a page read
 * twice without an error or a discrepancy; "The seed_valid bits are cleared
 * again when the RRAM controller is disabled"), "RRAM Scrambling" ("Software
 * cannot read them"; on disable "the scrambler switches to a separate pair of
 * random keys"), programmers_guide.md "Initialization" (an operation before
 * init_done ends with op_err) and "Errors Encountered During Hardware Initiated
 * Operations" (seed_err).
 *
 * The software:
 *   1. STATUS.init_done and keys_valid set (the boot stages ran INIT);
 * PHY_STATUS.init_done set; FAULT_STATUS (seed_err, lcmgr errors) 0;
 *   2. INIT written again (rw1s, already set): nothing changes, a controller
 * read still works;
 *   3. every register of the rram_ctrl.core window (but the FIFOs) read and
 * printed as `EG2 RRAM init reg off=0x<o> val=0x<v>`: the vseq checks no 32-bit
 * slice of the address key, the data key or either seed appears;
 *   4. `EG2 RRAM init seeds`: the vseq checks each seed's valid against
 * lc_seed_hw_rd_en (its life-cycle state drawn: RMA or DEV read the seeds,
 * TEST_UNLOCKED0 does not), and that the scrambler uses the sideloaded keys;
 *   5. with `disable`: `EG2 RRAM init disable` and DIS.SW_DIS set (MuBi4True;
 * rw1s). RRAM is then disabled and the core fetches nothing more from it: the
 * vseq checks the scrambler's keys are the random keys and differ from the
 * sideloaded ones, both seeds invalid, then resets the chip (boot_phase = 1);
 * boot 1 checks steps 1 and 2 again.
 *
 * Not stimulated at chip level: a discrepancy between the two reads of a seed
 * page (seed_err). Both reads of a page come through the RRAM read buffer, so
 * no chip-level fault on the macro's contents changes the second read only; the
 * lcmgr's own seed registers would have to be forced (rram_ctrl's block bench,
 * sec_cm of the lcmgr). The coverage cross `lc_seed_hw_rd_en` x corruption is
 * closed there.
 *
 * Knobs:
 *   disable     1: the disable step and the second boot (chip only)
 *   boot_phase  written by the vseq before the reset
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1 | Why |
 * |-----------------------------|--------------------------------------------------------------|
 * | (nothing in steps 1-3)      | eg2sim's rram_ctrl sets init_done and
 * keys_valid at INIT     | |                             |   (ip/eg2_mem.h);
 * steps 4-5 are the vseq's                   |
 *
 * Built with EG2_RRAM_INIT_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define INIT_PARAMS(X)   \
  X(disable, 0, 0, 1)    \
  X(boot_phase, 0, 0, 1) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(INIT_PARAMS)

enum {
  kPolls = 100000,
  kResetWait = 4000000,
  kReady = (1u << RRAM_CTRL_STATUS_INIT_DONE_BIT) |
           (1u << RRAM_CTRL_STATUS_KEYS_VALID_BIT),
  kLastReg = RRAM_CTRL_RD_FIFO_REG_OFFSET,  // the last register; the window
                                            // beyond is unmapped
};

static bool init_state(void) {
  bool ok = eg2_chk_eq("STATUS init_done and keys_valid", kReady,
                       eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & kReady);
  ok = eg2_chk_eq("PHY_STATUS.init_done", 1u << 1,
                  eg2_rram_reg(RRAM_CTRL_PHY_STATUS_REG_OFFSET) & (1u << 1)) &&
       ok;
  ok = eg2_chk_eq("FAULT_STATUS (seed_err, lcmgr errors)", 0,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("STD_FAULT_STATUS", 0,
                  eg2_rram_reg(RRAM_CTRL_STD_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  // INIT again: rw1s and already set; the controller stays initialised and
  // usable.
  eg2_rram_reg_wr(RRAM_CTRL_INIT_REG_OFFSET, 1u << RRAM_CTRL_INIT_VAL_BIT);
  ok = eg2_chk_eq("INIT.VAL reads set", 1,
                  eg2_rram_reg(RRAM_CTRL_INIT_REG_OFFSET) & 1) &&
       ok;
  ok = eg2_chk_true("still initialised after a second INIT",
                    eg2_rram_wait_init(kPolls)) &&
       ok;
  uint32_t region = eg2_rram_free_region();
  if (region < kEg2RramRegions) {
    eg2_rram_region_set(region, kEg2RramScratchLastPage, 1,
                        kEg2RramAttrRd | kEg2RramAttrEcc);
    uint32_t w;
    eg2_rram_result_t r;
    ok = eg2_chk_true("controller read done",
                      eg2_rram_read(kEg2RramPartData,
                                    eg2_rram_addr(kEg2RramScratchLastPage, 0),
                                    &w, 1, &r)) &&
         ok;
    ok = eg2_chk_eq("controller read error code", 0, r.err_code) && ok;
    eg2_rram_region_off(region);
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  eg2_chk_step("boot phase", EG2_PARAM(boot_phase));
  bool ok = init_state();
  if (EG2_PARAM(boot_phase) == 1) {
    return eg2_chk_summary(6) && ok;
  }
  // Every register but the FIFOs (a read of rd_fifo pops it).
  for (uint32_t off = 0; off <= kLastReg; off += 4) {
    if (off == RRAM_CTRL_RD_FIFO_REG_OFFSET ||
        off == RRAM_CTRL_WR_FIFO_REG_OFFSET) {
      continue;
    }
    LOG_INFO("EG2 RRAM init reg off=0x%x val=0x%x", off, eg2_rram_reg(off));
  }
  EG2_CHK_SYNC("EG2 RRAM init seeds");
  if (!EG2_PARAM(disable) || EG2_PARAM(model_gaps)) {
    return eg2_chk_summary(6) && ok;
  }
  ok = eg2_chk_summary(6) && ok;
  if (!ok) {
    return false;
  }
  EG2_CHK_SYNC("EG2 RRAM init disable");
  // From here RRAM is disabled: the core can fetch nothing more from it until
  // the reset.
  eg2_rram_reg_wr(RRAM_CTRL_DIS_REG_OFFSET, (uint32_t)kEg2RramMubiTrue);
  eg2_chk_delay(kResetWait);
  return eg2_chk_true("the bench reset the chip", false);
}
