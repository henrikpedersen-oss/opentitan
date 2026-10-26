// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_entropy_fault (EG2 verification spec REQ_PBF_12, "Post-boot faults"): a health
 * test of entropy_src forced to fail while the entropy complex serves its consumers, in both
 * modes.
 *
 * 1. The entropy complex in auto mode (csrng, edn0 and edn1 serving every EDN consumer);
 *    entropy_src in FIPS mode (MAIN_SM_STATE ContHTRunning, the DIF's encoding of the
 *    documented state). csrng's software instance is instantiated from entropy and generates
 *    `blocks` blocks: no two may be equal (no repeated data).
 * 2. The fault: entropy_src is re-enabled with the adaptive-proportion test's high threshold at
 *    `threshold` (far below the ones any window holds) and ALERT_THRESHOLD 1, so the first window
 *    fails.
 * 3. Check (entropy_src theory of operation and registers.md): the recoverable alert reaches the
 *    alert handler (declared to the status module); es_health_test_failed is raised (declared);
 *    ALERT_FAIL_COUNTS counts the adaptive-proportion failure; MAIN_SM_STATE leaves the healthy
 *    states for AlertState / AlertHang; a reseed of the software instance from entropy stalls:
 *    no acknowledgement and no new genbits within `stall_polls` polls (a consumer that needs
 *    entropy receives none rather than stale data).
 * 4. The complex is stopped and the alert status cleared, so the status sweep sees only what was
 *    declared.
 * Whether every hardware EDN consumer (AES, KMAC, OTBN, keymgr, rv_core_ibex, AST) stops
 * receiving words at its next reseed is the chip bench's check (chip_sw_eg2_pbf_entropy_fault_vseq
 * counts EDN acknowledgements after the alert).
 *
 * Knobs: blocks (2-16), threshold (1-64), stall_polls, model_gaps.
 *
 * | Skipped with model_gaps = 1 | Why |
 * |---|---|
 * | steps 2-3 | eg2sim's entropy_src runs no health test ("Health tests always pass",
 *   eg2_crypto_entropy.h) |
 *
 * With EG2_PBF_INJECT_FAIL the first value check (MAIN_SM_STATE in FIPS mode) expects bit 0
 * flipped: `PBF FAIL`.
 */

#include "hw/top/csrng_regs.h"        // Generated.
#include "hw/top/dt/api.h"            // Generated.
#include "hw/top/dt/csrng.h"          // Generated.
#include "hw/top/dt/entropy_src.h"    // Generated.
#include "hw/top/entropy_src_regs.h"  // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/dif/dif_entropy_src.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

EG2_TEST_CONFIG();

#define ENTROPY_FAULT_PARAMS(X) \
  X(blocks, 8, 2, 16)           \
  X(threshold, 1, 1, 64)        \
  X(stall_polls, 20000, 100, 1000000) \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(ENTROPY_FAULT_PARAMS)

enum {
  kPolls = 200000,
};

static dif_csrng_t csrng;
static dif_entropy_src_t es;

static bool ready(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    dif_csrng_cmd_status_t s;
    if (dif_csrng_get_cmd_interface_status(&csrng, &s) != kDifOk ||
        s.kind == kDifCsrngCmdStatusError) {
      return false;
    }
    if (s.kind == kDifCsrngCmdStatusReady) {
      return true;
    }
  }
  return false;
}

static uint32_t main_sm(void) {
  dif_entropy_src_main_fsm_t st = 0;
  (void)dif_entropy_src_get_main_fsm_state(&es, &st);
  return (uint32_t)st;
}

static bool before_fault(void) {
  bool ok = eg2_pbf_check("entropy_src MAIN_SM_STATE ContHTRunning (FIPS mode)",
                          kDifEntropySrcMainFsmStateContHTRunning, main_sm());
  uint32_t n = EG2_PARAM(blocks);
  uint32_t w[4 * 16];
  dif_csrng_seed_material_t none;
  memset(&none, 0, sizeof(none));
  bool inst = ready() &&
              dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleEnable, &none) == kDifOk &&
              ready();
  if (!inst) {
    eg2_pbf_note("csrng SW_CMD_STS", eg2_dev_read32(kEg2DevCsrng, CSRNG_SW_CMD_STS_REG_OFFSET));
  }
  ok = eg2_pbf_step("csrng software instance instantiated from entropy", inst) && ok;
  ok = eg2_pbf_step("csrng generates before the fault",
                    inst && dif_csrng_generate_start(&csrng, NULL, 4 * n) == kDifOk &&
                        dif_csrng_generate_read(&csrng, w, 4 * n) == kDifOk) &&
       ok;
  bool distinct = true;
  for (uint32_t a = 0; a < n; ++a) {
    for (uint32_t b = a + 1; b < n; ++b) {
      if (memcmp(&w[4 * a], &w[4 * b], 16) == 0) {
        distinct = false;
      }
    }
  }
  ok = eg2_pbf_step("csrng blocks before the fault all differ (no repeated data)", distinct) && ok;
  eg2_pbf_activity("csrng blocks before the fault", n);
  return ok;
}

static bool force_failure(void) {
  int alert = eg2_status_alert_find("entropy_src_recov_alert");
  if (alert >= 0) {
    eg2_status_expect_alert((size_t)alert);
  }
  eg2_status_expect_intr(kEg2DevEntropySrc, 1u << ENTROPY_SRC_INTR_STATE_ES_HEALTH_TEST_FAILED_BIT);
  bool ok = eg2_pbf_step("entropy_src disabled for reconfiguration",
                         dif_entropy_src_set_enabled(&es, kDifToggleDisabled) == kDifOk);
  dif_entropy_src_health_test_config_t adaptp = {
      .test_type = kDifEntropySrcTestAdaptiveProportion,
      .high_threshold = (uint16_t)EG2_PARAM(threshold),
      .low_threshold = 0,
  };
  ok = eg2_pbf_step("adaptive-proportion high threshold below any window",
                    dif_entropy_src_health_test_configure(&es, adaptp) == kDifOk) &&
       ok;
  dif_entropy_src_config_t cfg = {
      .fips_enable = true,
      .fips_flag = true,
      .rng_fips = true,
      .route_to_firmware = false,
      .bypass_conditioner = false,
      .single_bit_mode = kDifEntropySrcSingleBitModeDisabled,
      .health_test_threshold_scope = false,
      .health_test_window_size = 2048,
      .alert_threshold = 1,
  };
  ok = eg2_pbf_step("entropy_src re-enabled, FIPS mode, ALERT_THRESHOLD 1",
                    dif_entropy_src_configure(&es, cfg, kDifToggleEnabled) == kDifOk) &&
       ok;
  ok = eg2_pbf_step("entropy_src_recov_alert reached the alert handler",
                    alert >= 0 && eg2_status_wait_alert((size_t)alert, kPolls)) &&
       ok;
  dif_entropy_src_alert_fail_counts_t fc;
  memset(&fc, 0, sizeof(fc));
  (void)dif_entropy_src_get_alert_fail_counts(&es, &fc);
  ok = eg2_pbf_step("ALERT_FAIL_COUNTS counts the adaptive-proportion failure",
                    fc.high_fails[kDifEntropySrcTestAdaptiveProportion] >= 1) &&
       ok;
  uint32_t st = main_sm();
  eg2_pbf_note("entropy_src MAIN_SM_STATE after the failure", st);
  ok = eg2_pbf_step("entropy_src MAIN_SM_STATE AlertState or AlertHang",
                    st == kDifEntropySrcMainFsmStateAlertState ||
                        st == kDifEntropySrcMainFsmStateAlertHang) &&
       ok;
  // A consumer that needs entropy now: the software instance's reseed must not complete.
  dif_csrng_seed_material_t none;
  memset(&none, 0, sizeof(none));
  ok = ready() && ok;
  ok = eg2_pbf_step("csrng reseed from entropy requested",
                    dif_csrng_reseed(&csrng, &none) == kDifOk) &&
       ok;
  bool acked = false;
  for (uint32_t i = 0; i < EG2_PARAM(stall_polls); ++i) {
    uint32_t s = eg2_dev_read32(kEg2DevCsrng, CSRNG_SW_CMD_STS_REG_OFFSET);
    if (s & (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) {
      acked = true;
      break;
    }
  }
  ok = eg2_pbf_step("csrng reseed stalls: no acknowledgement without healthy entropy", !acked) &&
       ok;
  ok = eg2_pbf_check("csrng GENBITS_VLD stays 0", 0,
                     eg2_dev_read32(kEg2DevCsrng, CSRNG_GENBITS_VLD_REG_OFFSET) &
                         (1u << CSRNG_GENBITS_VLD_GENBITS_VLD_BIT)) &&
       ok;
  eg2_pbf_activity("health-test failures forced", 1);
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("entropy_fault", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  (void)eg2_pbf_boot_index();
  ok = eg2_pbf_entropy_up() && ok;
  ok = eg2_pbf_step("dif init", dif_csrng_init_from_dt(kDtCsrng, &csrng) == kDifOk &&
                                    dif_entropy_src_init_from_dt(kDtEntropySrc, &es) == kDifOk) &&
       ok;
  ok = before_fault() && ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("forced health-test failure, the alert, the stall");
    return ok;
  }
  ok = force_failure() && ok;
  // Stop the complex and clear entropy_src's recoverable alert status (rw0c).
  ok = eg2_pbf_step("entropy complex stopped", status_ok(entropy_testutils_stop_all())) && ok;
  eg2_dev_write32(kEg2DevEntropySrc, ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0);
  return ok;
}
