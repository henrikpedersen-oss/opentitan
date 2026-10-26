// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_keymgr_dpe_sideload_kmac_error (EG2 verification spec REQ_PBF_15; chip_keymgr_dpe
 * testplan chip_sw_keymgr_dpe_sideload_kmac_error), in both modes: KMAC returns an error during a
 * keymgr_dpe operation. The bench glitches KMAC's application-interface sparse FSM while KMAC
 * computes the digest of a sideload generation for KMAC; then (the testpoint, and keymgr_dpe's
 * theory_of_operation "Fault Handling"):
 *   - OP_STATUS reads DONE_ERROR, FAULT_STATUS.KMAC_DONE is set;
 *   - the `keymgr_dpe_fatal_fault_err` alert and KMAC's own `kmac_fatal_fault_err` (its FSM
 *     error) arrive;
 *   - the key is wiped: the key manager leaves its working state (WORKING_STATE INVALID), and
 *     the software key it generated before reads back different (the output registers are
 *     overwritten).
 * FAULT_STATUS and the fatal alerts stay until a reset, so the bench then resets the chip
 * (boot_phase 1) and the second boot checks a key manager in its reset state with no fault.
 *
 * keymgr_dpe is brought up by keymgr_dpe_testutils_startup (from the test ROM: seeds written,
 * SECRET2 locked, a software reset, then the UDS and the CreatorRootKey).
 *
 * Knobs: boot_phase (the bench's), model_gaps; the bench draws when the glitch comes.
 *
 * | Skipped with model_gaps = 1                | Why                                           |
 * |--------------------------------------------|-----------------------------------------------|
 * | the KMAC error and everything after it     | no bench in eg2sim to glitch KMAC             |
 *
 * Built with EG2_KEYMGR_DPE_SIDELOAD_KMAC_ERROR_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `MEM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/testing/keymgr_dpe_testutils.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define KMAC_ERROR_PARAMS(X) \
  X(boot_phase, 0, 0, 1)     \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(KMAC_ERROR_PARAMS)

enum { kPolls = 1000000, kOutWords = 8 };

static dif_keymgr_dpe_t keymgr;
static dif_kmac_t kmac;

static uint32_t km_rd(uint32_t off) { return eg2_dev_read32(kEg2DevKeymgrDpe, off); }

static void read_sw_out(uint32_t out[2 * kOutWords]) {
  for (uint32_t i = 0; i < kOutWords; ++i) {
    out[i] = km_rd(KEYMGR_DPE_SW_SHARE0_OUTPUT_0_REG_OFFSET + 4 * i);
    out[kOutWords + i] = km_rd(KEYMGR_DPE_SW_SHARE1_OUTPUT_0_REG_OFFSET + 4 * i);
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  bool ok = true;
  if (EG2_PARAM(boot_phase) == 1) {
    ok = eg2_chk_eq("WORKING_STATE after the reset", KEYMGR_DPE_WORKING_STATE_STATE_VALUE_RESET,
                    km_rd(KEYMGR_DPE_WORKING_STATE_REG_OFFSET) & 0x3) &&
         ok;
    ok = eg2_chk_eq("FAULT_STATUS after the reset", 0, km_rd(KEYMGR_DPE_FAULT_STATUS_REG_OFFSET)) &&
         ok;
    return eg2_chk_summary(2) && ok;
  }

  ok = eg2_chk_true("keymgr_dpe startup",
                    status_ok(keymgr_dpe_testutils_startup(&keymgr, &kmac))) &&
       ok;
  // After the UDS and the CreatorRootKey the key manager is Available (keymgr_dpe
  // theory_of_operation "Working State").
  ok = eg2_chk_eq("WORKING_STATE after startup (AVAILABLE)",
                  KEYMGR_DPE_WORKING_STATE_STATE_VALUE_AVAILABLE,
                  eg2_dev_read32(kEg2DevKeymgrDpe, KEYMGR_DPE_WORKING_STATE_REG_OFFSET) & 0x3) &&
       ok;
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET,
                  eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET));
  // The software key the generation in startup left: it must not survive the fault.
  uint32_t before[2 * kOutWords], after[2 * kOutWords];
  read_sw_out(before);

  dif_keymgr_dpe_generate_params_t p = kKeyVersionedParams;
  p.key_dest = kDifKeymgrDpeKeyDestKmac;
  p.sideload_key = true;
  for (uint32_t i = 0; i < 8; ++i) {
    p.salt[i] ^= eg2_rand();
  }
  if (EG2_PARAM(model_gaps)) {
    eg2_chk_skip("the KMAC glitch (no bench in eg2sim)");
    ok = eg2_chk_true("sideload generation for KMAC",
                      status_ok(keymgr_dpe_testutils_generate_key(&keymgr, &p))) &&
         ok;
    return eg2_chk_summary(2) && ok;
  }

  int km_alert = eg2_status_alert_find("keymgr_dpe_fatal_fault_err");
  int kmac_alert = eg2_status_alert_find("kmac_fatal_fault_err");
  if (km_alert >= 0) {
    eg2_status_expect_alert((size_t)km_alert);
  }
  if (kmac_alert >= 0) {
    eg2_status_expect_alert((size_t)kmac_alert);
  }
  // The bench glitches KMAC's application FSM once it leaves idle for this generation.
  EG2_CHK_SYNC("EG2 MEM kmac glitch armed");
  ok = eg2_chk_eq("generate started", kDifOk, dif_keymgr_dpe_generate(&keymgr, &p)) && ok;
  uint32_t st = 0;
  for (uint32_t i = 0; i < kPolls; ++i) {
    st = km_rd(KEYMGR_DPE_OP_STATUS_REG_OFFSET) & 0x3;
    if (st != KEYMGR_DPE_OP_STATUS_STATUS_VALUE_WIP) {
      break;
    }
  }
  ok = eg2_chk_eq("OP_STATUS (DONE_ERROR)", KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR, st) && ok;
  ok = eg2_chk_true("FAULT_STATUS.KMAC_DONE",
                    (km_rd(KEYMGR_DPE_FAULT_STATUS_REG_OFFSET) >>
                     KEYMGR_DPE_FAULT_STATUS_KMAC_DONE_BIT) &
                        1) &&
       ok;
  ok = eg2_chk_true("keymgr_dpe_fatal_fault_err alert",
                    km_alert >= 0 && eg2_status_wait_alert((size_t)km_alert, 10000)) &&
       ok;
  ok = eg2_chk_true("kmac_fatal_fault_err alert",
                    kmac_alert >= 0 && eg2_status_wait_alert((size_t)kmac_alert, 10000)) &&
       ok;
  ok = eg2_chk_eq("WORKING_STATE (INVALID: keys wiped)",
                  KEYMGR_DPE_WORKING_STATE_STATE_VALUE_INVALID,
                  km_rd(KEYMGR_DPE_WORKING_STATE_REG_OFFSET) & 0x3) &&
       ok;
  read_sw_out(after);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < 2 * kOutWords; ++i) {
    kept += after[i] == before[i] && before[i] != 0;
  }
  ok = eg2_chk_eq("software key words that survived the fault", 0, kept) && ok;
  // FAULT_STATUS and the fatal alerts stay until a reset: the bench resets the chip.
  EG2_CHK_SYNC("EG2 MEM kmac error checked");
  eg2_chk_delay(2000000);
  return eg2_chk_true("the bench's reset after the fault", false) && ok;
}
