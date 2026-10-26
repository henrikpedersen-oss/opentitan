// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rng_health (EG2 verification spec "Random Number Generation Test Suite",
 * REQ_PBF_24, REQ_PBF_12): one health test of entropy_src at one side of its threshold, on a
 * noise pattern whose statistic the bench sets to threshold - 1, threshold or threshold + 1.
 *
 * Knobs (drawn by chip_sw_eg2_rng_health_vseq, which selects the matching pattern of
 * dv/eg2_rng/eg2_rng_source.h at bench step 1, just before the software enables entropy_src, so
 * the pattern's windows start with entropy_src's):
 *   test       0 repcnt, 1 repcnts, 2 adaptp_hi, 3 adaptp_lo, 5 bucket, 6 markov_hi, 7 markov_lo
 *              (4 adaptps and 8/9 extht: gaps, see below)
 *   side       the pattern's statistic V = threshold - 1 (0), threshold (1), threshold + 1 (2)
 *   threshold  the test's threshold register; every other test out of reach (high 0xffff, low 0:
 *              "can be turned off by setting the thresholds to the maximum value", theory of
 *              operation)
 *   mode       0 boot-time (window: BYPASS_WINDOW, 384 bits = 96 symbols; ALERT_THRESHOLD 1),
 *              1 FIPS (window FIPS_WINDOW = `window` symbols; ALERT_THRESHOLD 2, the default)
 *   window     FIPS_WINDOW
 * The pattern's statistic: the longest run of one value on a lane (repcnt), of one symbol
 * (repcnts), the ones per lane per window (adaptp, CONF.THRESHOLD_SCOPE false: per line), the
 * count of symbol 0x0 per window (bucket), the switching pairs per lane per window (markov).
 *
 * The expected verdict rests on the documents:
 *   - beyond the threshold (V = T + 1 for a high threshold, V = T - 1 for a low one) the test
 *     fails; on the other side (V = T - 1, resp. T + 1) it does not;
 *   - at the threshold: the Repetition Count tests fail, as NIST SP 800-90B 4.4.1 defines them
 *     (a run of C identical samples fails; entropy_src states SP 800-90B compliance). For the
 *     other tests the registers' descriptions do not say whether reaching the threshold fails: the
 *     run reports what happened as a gap (`RNG gap ... at the threshold`), unchecked.
 * When the test fails: es_health_test_failed (declared), entropy_src's recoverable alert at the
 * alert handler (declared), RECOV_ALERT_STS.ES_MAIN_SM_ALERT, ALERT_SUMMARY_FAIL_COUNTS =
 * ALERT_THRESHOLD and the test's own ALERT_FAIL_COUNTS field equal to it (every window of the
 * pattern fails, and each symbol of a continuous test counts once: ALERT_THRESHOLD's description),
 * every other test's field 0, MAIN_SM_STATE AlertState or AlertHang, and no seed reaches
 * ENTROPY_DATA (the consumer stalls rather than receive the failed data). When it does not fail:
 * the seeds arrive (one in boot mode, two in FIPS mode: the startup seed and one more), every fail
 * count is 0, no alert.
 *
 * Gaps: adaptps (the Adaptive Proportion Symbol test: no document defines its statistic); the
 * external health test (top_earlgrey ties entropy_src's xht inputs off: nothing to test); the
 * pattern windows of boot mode assume BYPASS_WINDOW in bits over 4-bit symbols (96 symbols).
 * model_gaps = 1 (eg2sim: no health tests in the model) runs only the first check.
 */

#include "hw/top/entropy_src_regs.h"  // Generated.
#include "sw/device/tests/eg2_rng/eg2_rng.h"

EG2_TEST_CONFIG();

#define RNG_HEALTH_PARAMS(X)        \
  X(test, 0, 0, 9)                  \
  X(side, 2, 0, 2)                  \
  X(threshold, 32, 1, 0xfffe)       \
  X(mode, 1, 0, 1)                  \
  X(window, 512, 96, 2048)          \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(RNG_HEALTH_PARAMS)

enum {
  kVerdictPass = 0,
  kVerdictFail = 1,
  kVerdictUndocumented = 2,
};

static uint32_t expected_verdict(uint32_t ht, uint32_t side) {
  bool lo = eg2_rng_ht_is_lo(ht);
  if (side == 1) {
    return (ht == kEg2RngHtRepcnt || ht == kEg2RngHtRepcnts) ? kVerdictFail
                                                             : kVerdictUndocumented;
  }
  bool beyond = lo ? side == 0 : side == 2;
  return beyond ? kVerdictFail : kVerdictPass;
}

/** Waits for the health test failure: RECOV_ALERT_STS.ES_MAIN_SM_ALERT, or a seed. */
static bool wait_outcome(bool *failed, uint32_t seeds_wanted) {
  uint32_t seed[12];
  uint32_t seeds = 0;
  for (uint32_t i = 0; i < kEg2RngPolls; ++i) {
    uint32_t sts = eg2_rng_es_read(ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
    if ((sts >> ENTROPY_SRC_RECOV_ALERT_STS_ES_MAIN_SM_ALERT_BIT) & 1) {
      *failed = true;
      return true;
    }
    if (eg2_rng_es_read_seed(seed, 64) && ++seeds >= seeds_wanted) {
      *failed = false;
      return true;
    }
  }
  return false;
}

bool eg2_test_main(void) {
  eg2_rng_begin("rng_health", EG2_RNG_INJECT);
  bool ok = eg2_rm_params_begin();
  ok = eg2_rng_check_reset_state() && ok;
  uint32_t ht = EG2_PARAM(test), side = EG2_PARAM(side), t = EG2_PARAM(threshold);
  bool fips = EG2_PARAM(mode) == 1;
  if (ht == kEg2RngHtAdaptps || ht >= kEg2RngHtExthtHi) {
    eg2_rng_gap(eg2_rng_ht_name(ht),
                ht == kEg2RngHtAdaptps
                    ? "no document defines the Adaptive Proportion Symbol test's statistic"
                    : "the external health test inputs are tied off in top_earlgrey");
    return ok;
  }
  if (EG2_PARAM(model_gaps)) {
    eg2_rng_model_gap("the health tests (always pass in the model)");
    return ok;
  }
  eg2_puts("RNG health: ");
  eg2_puts(eg2_rng_ht_name(ht));
  eg2_puts(fips ? " FIPS mode, threshold " : " boot mode, threshold ");
  eg2_putdec(t);
  eg2_puts(side == 0 ? ", statistic threshold - 1\r\n"
                     : side == 1 ? ", statistic at the threshold\r\n"
                                 : ", statistic threshold + 1\r\n");

  eg2_rng_es_cfg_t cfg = {
      .mode = fips ? kEg2RngFips : kEg2RngBoot,
      .route_to_fw = true,
      .fips_window = (uint16_t)EG2_PARAM(window),
      .alert_threshold = fips ? 2 : 1,
  };
  eg2_rng_thresholds_off(&cfg);
  cfg.thresh[ht] = (uint16_t)t;
  ok = eg2_rng_stop_all() && ok;
  eg2_rng_es_configure(&cfg);

  uint32_t verdict = expected_verdict(ht, side);
  int recov = eg2_status_alert_find("entropy_src_recov_alert");
  if (verdict == kVerdictFail) {
    eg2_status_expect_alert((size_t)recov);
    eg2_status_expect_intr(kEg2DevEntropySrc,
                           1u << ENTROPY_SRC_INTR_STATE_ES_HEALTH_TEST_FAILED_BIT);
  }
  eg2_rng_bench_step(1);  // the bench selects the pattern
  eg2_rng_es_enable(true);

  bool failed = false;
  ok = eg2_rng_step("health test outcome within the poll budget (an alert or the seeds)",
                    wait_outcome(&failed, fips ? 2 : 1)) &&
       ok;
  if (verdict == kVerdictUndocumented) {
    eg2_rng_gap(failed ? "at the threshold: the test FAILED" : "at the threshold: the test passed",
                "the register descriptions do not say whether reaching the threshold fails");
    if (failed) {
      eg2_status_expect_alert((size_t)recov);
      eg2_status_expect_intr(kEg2DevEntropySrc,
                             1u << ENTROPY_SRC_INTR_STATE_ES_HEALTH_TEST_FAILED_BIT);
    }
    verdict = failed ? kVerdictFail : kVerdictPass;
  } else {
    ok = eg2_rng_check("verdict (1: failed)", verdict == kVerdictFail, failed) && ok;
  }

  uint32_t expect_count = verdict == kVerdictFail ? cfg.alert_threshold : 0;
  if (verdict == kVerdictFail) {
    ok = eg2_rng_step("recoverable alert at the alert handler",
                      eg2_status_wait_alert((size_t)recov, 10000)) &&
         ok;
    uint32_t st;
    bool alert_sm = eg2_rng_main_sm_alert(&st);
    eg2_rng_note("MAIN_SM_STATE", st);
    ok = eg2_rng_step("main state machine in AlertState or AlertHang", alert_sm) && ok;
    uint32_t seed[12];
    ok = eg2_rng_step("no seed after the failure (the consumer stalls)",
                      !eg2_rng_es_read_seed(seed, 20000)) &&
         ok;
  }
  ok = eg2_rng_check("ALERT_SUMMARY_FAIL_COUNTS", expect_count,
                     eg2_rng_es_read(ENTROPY_SRC_ALERT_SUMMARY_FAIL_COUNTS_REG_OFFSET) & 0xffff) &&
       ok;
  for (uint32_t i = 0; i < kEg2RngHtExthtHi; ++i) {
    eg2_puts("RNG health fail count ");
    eg2_puts(eg2_rng_ht_name(i));
    eg2_puts("\r\n");
    ok = eg2_rng_check("ALERT_FAIL_COUNTS field", i == ht ? expect_count : 0,
                       eg2_rng_fail_count(i)) &&
         ok;
  }
  eg2_rng_es_enable(false);
  ok = eg2_rng_stop_all() && ok;
  return ok;
}
