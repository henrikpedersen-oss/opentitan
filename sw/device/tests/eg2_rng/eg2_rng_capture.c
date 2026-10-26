// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rng_capture (EG2 verification spec "Random Number Generation Test Suite",
 * REQ_PBF_24): every tap of the random-number path in turn, `capture_words` words each, written
 * to the DV log as `EG2_RNG <tap>_<mode> w0 .. w7` lines for scripts/rng_extract.py and
 * `make rng-assess`. The chip bench drives the noise source from the simulation host's CPU random
 * generator (`+eg2_rng=host`); its scoreboard compares every word software reads with the model
 * fed the same bits.
 *
 * Taps (bits of `taps`) and the modes they run in (bits of `modes`: 1 boot, 2 FIPS, 4 bypass):
 *   raw   the observe FIFO (FW_OV_CONTROL.FW_OV_MODE, observe only), in each mode; the capture is
 *         contiguous only if FW_OV_RD_FIFO_OVERFLOW and RECOV_ALERT_STS.POSTHT_ENTROPY_DROP_ALERT
 *         stay clear (registers.md), which the test checks;
 *   cond  ENTROPY_DATA (ES_ROUTE, ENTROPY_DATA_REG_ENABLE): boot mode gives one 384-bit seed per
 *         enable (the boot-time seed), so the block is re-enabled for each; FIPS mode the SHA-3
 *         conditioned seeds; bypass mode (FIPS_ENABLE and ES_TYPE) the unconditioned ones;
 *   drbg  the csrng software instance, instantiated from entropy_src (hardware route) in boot and
 *         FIPS mode, generate commands of at most 64 blocks; bypass mode routes entropy to
 *         firmware only, so drbg does not run there;
 *   edn   an EDN0 consumer, rv_core_ibex RND_DATA, with EDN0 in auto mode (FIPS mode only:
 *         EDN's auto mode needs csrng instantiated from the hardware route, which bypass mode
 *         closes, and boot mode delivers a single seed).
 *
 * Own check (besides the bench's): no two consecutive words of a tap's stream are equal (a
 * repeat of a 32-bit word has probability 2^-32 per pair from a random source; entropy_src's
 * ES_BUS_CMP_ALERT treats an equal consecutive value as a possible attack). The first check of
 * the test is entropy_src's reset state (eg2_rng.h).
 *
 * EG2_RNG_INJECT_FAIL (spec): every captured word is written twice; SP 800-22 must then fail on the
 * captured files, and the test's own checks fail (`RNG FAIL`): the corrupted first check and the
 * consecutive-word check.
 *
 * model_gaps = 1 (eg2sim) skips the raw tap (the model has no observe FIFO) and the edn tap
 * (rv_core_ibex's RND_STATUS is never valid in the model).
 */

#include "hw/top/dt/api.h"            // Generated.
#include "hw/top/entropy_src_regs.h"  // Generated.
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/tests/eg2_rng/eg2_rng.h"

EG2_TEST_CONFIG();

// capture_words up to 2^22: the model's assessment run (make rng-assess-model) takes 3,200,000
// words per tap, SP 800-22's 100 sequences of 10^6 bits (2026-10-26; was at most 2^20).
#define RNG_CAPTURE_PARAMS(X)          \
  X(capture_words, 4096, 8, 4194304)   \
  X(taps, 15, 1, 15)                   \
  X(modes, 7, 1, 7)                    \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(RNG_CAPTURE_PARAMS)

enum {
  kDrbgChunkWords = 64 * 4,
};

/** The stream of one tap: lines of 8 words, and the consecutive-word count. */
typedef struct sink {
  uint32_t tap;
  eg2_rng_esmode_t mode;
  uint32_t line[8];
  uint32_t fill;
  uint32_t written;
  uint32_t prev;
  bool have_prev;
  uint32_t repeats;
} sink_t;

static void sink_put1(sink_t *s, uint32_t w) {
  if (s->have_prev && w == s->prev) {
    ++s->repeats;
  }
  s->prev = w;
  s->have_prev = true;
  s->line[s->fill++] = w;
  ++s->written;
  if (s->fill == 8) {
    eg2_rng_log_line(s->tap, s->mode, s->line);
    s->fill = 0;
  }
}

static void sink_put(sink_t *s, uint32_t w) {
  sink_put1(s, w);
  if (EG2_RNG_INJECT) {
    sink_put1(s, w);  // the spec's inject: every captured word twice
  }
}

static bool sink_done(sink_t *s, const char *what) {
  eg2_puts("RNG capture ");
  eg2_puts(what);
  eg2_puts(": ");
  eg2_putdec(s->written);
  eg2_puts(" words logged\r\n");
  return eg2_rng_check("no two consecutive captured words equal", 0, s->repeats);
}

static uint32_t words(void) { return (EG2_PARAM(capture_words) + 7) & ~7u; }

static const char *mode_name(eg2_rng_esmode_t m) {
  return m == kEg2RngBoot ? "boot" : m == kEg2RngFips ? "fips" : "bypass";
}

static void base_cfg(eg2_rng_es_cfg_t *cfg, eg2_rng_esmode_t mode) {
  *cfg = (eg2_rng_es_cfg_t){
      .mode = mode,
      .fips_window = 512,
      // No alert: the capture observes; a health-test failure on host bits would be a finding
      // reported by the fail counts below.
      .alert_threshold = 0,
  };
  eg2_rng_thresholds_off(cfg);
}

static bool health_clean(void) {
  uint32_t f = eg2_rng_es_read(ENTROPY_SRC_ALERT_SUMMARY_FAIL_COUNTS_REG_OFFSET) & 0xffff;
  eg2_rng_note("ALERT_SUMMARY_FAIL_COUNTS after the tap", f);
  return true;
}

static bool capture_raw(eg2_rng_esmode_t mode) {
  sink_t s = {.tap = kEg2RngTapRaw, .mode = mode};
  eg2_rng_es_cfg_t cfg;
  base_cfg(&cfg, mode);
  cfg.fw_ov = true;
  cfg.route_to_fw = mode == kEg2RngBypass;
  bool ok = eg2_rng_stop_all();
  eg2_rng_es_configure(&cfg);
  eg2_rng_es_enable(true);
  uint32_t n = words();
  for (uint32_t i = 0; i < n && ok; ++i) {
    uint32_t p = 0;
    while (eg2_rng_es_read(ENTROPY_SRC_OBSERVE_FIFO_DEPTH_REG_OFFSET) == 0) {
      if (++p >= kEg2RngPolls) {
        ok = eg2_rng_step("observe FIFO delivers a word", false);
        break;
      }
    }
    if (ok) {
      sink_put(&s, eg2_rng_es_read(ENTROPY_SRC_FW_OV_RD_DATA_REG_OFFSET));
    }
  }
  uint32_t ovf = eg2_rng_es_read(ENTROPY_SRC_FW_OV_RD_FIFO_OVERFLOW_REG_OFFSET) & 1;
  uint32_t drop = (eg2_rng_es_read(ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET) >>
                   ENTROPY_SRC_RECOV_ALERT_STS_POSTHT_ENTROPY_DROP_ALERT_BIT) & 1;
  ok = eg2_rng_step("raw capture contiguous (FW_OV_RD_FIFO_OVERFLOW clear)", ovf == 0) && ok;
  ok = eg2_rng_step("no post-health-test drop (POSTHT_ENTROPY_DROP_ALERT clear)", drop == 0) &&
       ok;
  ok = health_clean() && ok;
  eg2_rng_es_enable(false);
  return sink_done(&s, mode_name(mode)) && ok;
}

static bool capture_cond(eg2_rng_esmode_t mode) {
  sink_t s = {.tap = kEg2RngTapCond, .mode = mode};
  eg2_rng_es_cfg_t cfg;
  base_cfg(&cfg, mode);
  cfg.route_to_fw = true;
  bool ok = eg2_rng_stop_all();
  eg2_rng_es_configure(&cfg);
  eg2_rng_es_enable(true);
  uint32_t n = words();
  uint32_t seed[12];
  while (s.written < n * (EG2_RNG_INJECT ? 2 : 1) && ok) {
    if (!eg2_rng_es_read_seed(seed, kEg2RngPolls)) {
      ok = eg2_rng_step("ENTROPY_DATA delivers a seed", false);
      break;
    }
    for (uint32_t i = 0; i < 12 && s.written < n * (EG2_RNG_INJECT ? 2 : 1); ++i) {
      sink_put(&s, seed[i]);
    }
    if (mode == kEg2RngBoot) {
      // One boot-time seed per enable (theory of operation, "Boot-Time / Bypass Mode").
      eg2_rng_es_enable(false);
      eg2_rng_es_configure(&cfg);
      eg2_rng_es_enable(true);
    }
  }
  ok = health_clean() && ok;
  eg2_rng_es_enable(false);
  return sink_done(&s, mode_name(mode)) && ok;
}

static bool csrng_wait_ready(const dif_csrng_t *csrng, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    dif_csrng_cmd_status_t st;
    if (dif_csrng_get_cmd_interface_status(csrng, &st) != kDifOk ||
        st.kind == kDifCsrngCmdStatusError) {
      return false;
    }
    if (st.kind == kDifCsrngCmdStatusReady) {
      return true;
    }
  }
  return false;
}

static bool capture_drbg(eg2_rng_esmode_t mode) {
  sink_t s = {.tap = kEg2RngTapDrbg, .mode = mode};
  eg2_rng_es_cfg_t cfg;
  base_cfg(&cfg, mode);
  dif_csrng_t csrng;
  bool ok = eg2_rng_stop_all() && dif_csrng_init_from_dt(kDtCsrng, &csrng) == kDifOk;
  eg2_rng_es_configure(&cfg);
  eg2_rng_es_enable(true);
  ok = ok && dif_csrng_configure(&csrng) == kDifOk;
  dif_csrng_seed_material_t none = {.seed_material_len = 0};
  ok = eg2_rng_step("csrng software instance instantiated from entropy_src",
                    ok && dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleEnable, &none) ==
                              kDifOk &&
                        csrng_wait_ready(&csrng, kEg2RngPolls)) &&
       ok;
  uint32_t n = words();
  uint32_t got = 0;
  while (got < n && ok) {
    uint32_t chunk = n - got < kDrbgChunkWords ? n - got : kDrbgChunkWords;
    ok = dif_csrng_generate_start(&csrng, NULL, chunk) == kDifOk;
    for (uint32_t i = 0; i < chunk && ok; i += 4) {
      uint32_t p = 0;
      dif_csrng_output_status_t os = {0};
      while (ok && !os.valid_data) {
        ok = dif_csrng_get_output_status(&csrng, &os) == kDifOk && ++p < kEg2RngPolls;
      }
      uint32_t blk[4];
      ok = ok && dif_csrng_generate_read(&csrng, blk, 4) == kDifOk;
      for (uint32_t k = 0; k < 4 && ok; ++k) {
        sink_put(&s, blk[k]);
      }
    }
    got += chunk;
  }
  ok = eg2_rng_step("csrng generated every block", ok);
  (void)dif_csrng_uninstantiate(&csrng);
  ok = health_clean() && ok;
  eg2_rng_stop_all();
  return sink_done(&s, mode_name(mode)) && ok;
}

static bool capture_edn(void) {
  sink_t s = {.tap = kEg2RngTapEdn, .mode = kEg2RngFips};
  eg2_rng_es_cfg_t cfg;
  base_cfg(&cfg, kEg2RngFips);
  bool ok = eg2_rng_stop_all();
  eg2_rng_es_configure(&cfg);
  eg2_rng_es_enable(true);
  ok = eg2_rng_csrng_edn0_auto(4) && ok;
  uint32_t n = words();
  for (uint32_t i = 0; i < n && ok; ++i) {
    uint32_t w;
    ok = eg2_rng_rnd_read(&w, kEg2RngPolls);
    if (ok) {
      sink_put(&s, w);
    } else {
      eg2_rng_step("rv_core_ibex RND_DATA valid (EDN0 delivers)", false);
    }
  }
  ok = health_clean() && ok;
  eg2_rng_stop_all();
  return sink_done(&s, "fips") && ok;
}

bool eg2_test_main(void) {
  eg2_rng_begin("rng_capture", EG2_RNG_INJECT);
  bool ok = eg2_rm_params_begin();
  ok = eg2_rng_check_reset_state() && ok;
  uint32_t taps = EG2_PARAM(taps), modes = EG2_PARAM(modes);
  bool gaps = EG2_PARAM(model_gaps) != 0;
  for (uint32_t m = 0; m < 3; ++m) {
    if (!((modes >> m) & 1)) {
      continue;
    }
    eg2_rng_esmode_t mode = (eg2_rng_esmode_t)m;
    if ((taps >> kEg2RngTapRaw) & 1) {
      if (gaps) {
        eg2_rng_model_gap("raw tap: the observe FIFO (FW_OV_*)");
      } else {
        ok = capture_raw(mode) && ok;
      }
    }
    if ((taps >> kEg2RngTapCond) & 1) {
      ok = capture_cond(mode) && ok;
    }
    if ((taps >> kEg2RngTapDrbg) & 1) {
      if (mode == kEg2RngBypass) {
        eg2_rng_gap("drbg tap in bypass mode",
                    "ES_TYPE needs ES_ROUTE, which closes the hardware route to csrng");
      } else {
        ok = capture_drbg(mode) && ok;
      }
    }
    if (((taps >> kEg2RngTapEdn) & 1) && mode == kEg2RngFips) {
      if (gaps) {
        eg2_rng_model_gap("edn tap: rv_core_ibex RND_STATUS never valid");
      } else {
        ok = capture_edn() && ok;
      }
    }
  }
  if ((taps >> kEg2RngTapEdn) & 1) {
    eg2_rng_gap("edn tap in boot and bypass mode",
                "EDN's auto mode needs the hardware route and repeated seeds (FIPS mode only)");
  }
  eg2_rng_stop_all();
  return ok;
}
