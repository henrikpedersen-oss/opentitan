// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rng_stuck (EG2 verification spec "Random Number Generation Test Suite",
 * REQ_PBF_24, REQ_PBF_12): the noise source stuck at 0 or at 1 (the bench's pattern:stuck0 /
 * stuck1), from the start (boot-time: entropy_src in boot mode, enabled on the stuck source) or
 * during continuous operation (FIPS mode, EDN0 serving rv_core_ibex, then the source sticks).
 *
 * Knobs (chip_sw_eg2_rng_stuck_vseq): `value` 0 / 1 (the pattern), `phase` 0 boot-time / 1
 * continuous, `threshold` the Repetition Count threshold (every other health test out of reach,
 * so the failure is the RCT's alone), `model_gaps`.
 *
 * Checks (the documents: entropy_src.hjson ALERT_THRESHOLD / ALERT_FAIL_COUNTS, the theory of
 * operation "Health Test Failure Alert", "Once the threshold is reached, the ENTROPY_SRC block
 * stops serving entropy and signals a recoverable alert"):
 *   - the Repetition Count test fails: es_health_test_failed (declared), REPCNT's ALERT_FAIL_COUNTS
 *     field = ALERT_SUMMARY_FAIL_COUNTS = ALERT_THRESHOLD (2), every other field 0,
 *     RECOV_ALERT_STS.ES_MAIN_SM_ALERT, MAIN_SM_STATE AlertState or AlertHang;
 *   - the recoverable alert reaches the alert handler (declared, eg2_status_wait_alert);
 *   - no EDN consumer receives fresh entropy: EDN0 runs in auto mode with a reseed after every
 *     one-block generate, so after the failure rv_core_ibex (an EDN0 consumer) may only receive
 *     the block generated before it (4 words) and the word it holds, then nothing within the
 *     poll budget; a csrng software instance instantiated from entropy_src does not complete.
 * The bench checks that the failure came within the RCT's window: at most `threshold` (+ the
 * pipeline's few samples) stuck symbols were delivered when es_health_test_failed rose.
 *
 * model_gaps = 1 (eg2sim: no health tests, no rv_core_ibex RND) runs only the first check.
 */

#include "hw/top/dt/api.h"            // Generated.
#include "hw/top/entropy_src_regs.h"  // Generated.
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/tests/eg2_rng/eg2_rng.h"

EG2_TEST_CONFIG();

#define RNG_STUCK_PARAMS(X)      \
  X(value, 0, 0, 1)              \
  X(phase, 1, 0, 1)              \
  X(threshold, 64, 8, 1024)      \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(RNG_STUCK_PARAMS)

enum {
  kAlertThreshold = 2,
  kWordsBeforeStuck = 8,
  kWordsAllowedAfter = 5,  // one generated block (4 words) and the word rv_core_ibex holds
  kAfterPolls = 200000,
};

static bool wait_failure(void) {
  for (uint32_t i = 0; i < kEg2RngPolls; ++i) {
    if ((eg2_rng_es_read(ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET) >>
         ENTROPY_SRC_RECOV_ALERT_STS_ES_MAIN_SM_ALERT_BIT) & 1) {
      return true;
    }
  }
  return false;
}

bool eg2_test_main(void) {
  eg2_rng_begin("rng_stuck", EG2_RNG_INJECT);
  bool ok = eg2_rm_params_begin();
  ok = eg2_rng_check_reset_state() && ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_rng_model_gap("the health tests and rv_core_ibex RND (not in the model)");
    return ok;
  }
  bool continuous = EG2_PARAM(phase) == 1;
  eg2_rng_es_cfg_t cfg = {
      .mode = continuous ? kEg2RngFips : kEg2RngBoot,
      .route_to_fw = false,
      .fips_window = 512,
      .alert_threshold = kAlertThreshold,
  };
  eg2_rng_thresholds_off(&cfg);
  cfg.thresh[kEg2RngHtRepcnt] = (uint16_t)EG2_PARAM(threshold);
  ok = eg2_rng_stop_all() && ok;
  eg2_rng_es_configure(&cfg);

  int recov = eg2_status_alert_find("entropy_src_recov_alert");
  eg2_status_expect_alert((size_t)recov);
  eg2_status_expect_intr(kEg2DevEntropySrc,
                         1u << ENTROPY_SRC_INTR_STATE_ES_HEALTH_TEST_FAILED_BIT);

  if (!continuous) {
    eg2_rng_bench_step(1);  // the bench selects pattern:stuck<value>
  }
  eg2_rng_es_enable(true);
  ok = eg2_rng_csrng_edn0_auto(1) && ok;
  if (continuous) {
    uint32_t w;
    uint32_t got = 0;
    for (uint32_t i = 0; i < kWordsBeforeStuck; ++i) {
      got += eg2_rng_rnd_read(&w, kEg2RngPolls) ? 1 : 0;
    }
    ok = eg2_rng_check("rv_core_ibex RND words before the source sticks", kWordsBeforeStuck, got) &&
         ok;
    eg2_rng_bench_step(2);  // the bench selects pattern:stuck<value>
  }
  ok = eg2_rng_step("the health test failure (RECOV_ALERT_STS.ES_MAIN_SM_ALERT)", wait_failure()) &&
       ok;
  ok = eg2_rng_step("recoverable alert at the alert handler",
                    eg2_status_wait_alert((size_t)recov, 10000)) &&
       ok;
  uint32_t st;
  bool alert_sm = eg2_rng_main_sm_alert(&st);
  eg2_rng_note("MAIN_SM_STATE", st);
  ok = eg2_rng_step("main state machine in AlertState or AlertHang", alert_sm) && ok;
  ok = eg2_rng_check("ALERT_SUMMARY_FAIL_COUNTS", kAlertThreshold,
                     eg2_rng_es_read(ENTROPY_SRC_ALERT_SUMMARY_FAIL_COUNTS_REG_OFFSET) & 0xffff) &&
       ok;
  for (uint32_t i = 0; i < kEg2RngHtExthtHi; ++i) {
    eg2_puts("RNG stuck fail count ");
    eg2_puts(eg2_rng_ht_name(i));
    eg2_puts("\r\n");
    ok = eg2_rng_check("ALERT_FAIL_COUNTS field", i == kEg2RngHtRepcnt ? kAlertThreshold : 0,
                       eg2_rng_fail_count(i)) &&
         ok;
  }

  // The consumers after the failure.
  uint32_t w, after = 0;
  while (after <= kWordsAllowedAfter && eg2_rng_rnd_read(&w, kAfterPolls)) {
    ++after;
  }
  eg2_rng_note("rv_core_ibex RND words after the failure", after);
  ok = eg2_rng_step("no fresh EDN word after the failure (at most the block generated before)",
                    after <= kWordsAllowedAfter) &&
       ok;
  dif_csrng_t csrng;
  dif_csrng_seed_material_t none = {.seed_material_len = 0};
  ok = ok && dif_csrng_init_from_dt(kDtCsrng, &csrng) == kDifOk;
  // csrng's software instance (instance 2) has not been instantiated: an instantiate needs a seed.
  ok = ok && dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleEnable, &none) == kDifOk;
  bool completed = false;
  for (uint32_t i = 0; i < kAfterPolls && !completed; ++i) {
    dif_csrng_cmd_status_t cs;
    completed = dif_csrng_get_cmd_interface_status(&csrng, &cs) == kDifOk &&
                cs.kind != kDifCsrngCmdStatusBusy;
  }
  ok = eg2_rng_step("a csrng instantiate from entropy_src does not complete", !completed) && ok;

  eg2_rng_es_enable(false);
  ok = eg2_rng_stop_all() && ok;
  return ok;
}
