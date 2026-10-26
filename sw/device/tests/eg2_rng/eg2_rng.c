// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_rng/eg2_rng.h"

#include "hw/top/dt/api.h"           // Generated.
#include "hw/top/entropy_src_regs.h"  // Generated.
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/dif/dif_csrng_shared.h"
#include "sw/device/lib/dif/dif_edn.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"

// Few globals: the CHERIoT build's globals stay under 4 KiB (REQ_STE_12).
static const char *rng_test = "rng";
static bool rng_corrupt;

enum {
  // MAIN_SM_STATE encodings of the alert states (dif_entropy_src.h, dif_entropy_src_main_fsm_t).
  kMainSmAlertState = 0x1cd,
  kMainSmAlertHang = 0x1fb,
  kDebugStatusMainSmIdleBit = 16,
};

static void head(const char *kind, const char *what) {
  eg2_puts("RNG ");
  eg2_puts(kind);
  eg2_puts(" ");
  eg2_puts(rng_test);
  eg2_puts(": ");
  eg2_puts(what);
}

const char *eg2_rng_ht_name(uint32_t ht) {
  switch (ht) {
    case kEg2RngHtRepcnt:
      return "repcnt";
    case kEg2RngHtRepcnts:
      return "repcnts";
    case kEg2RngHtAdaptpHi:
      return "adaptp_hi";
    case kEg2RngHtAdaptpLo:
      return "adaptp_lo";
    case kEg2RngHtAdaptps:
      return "adaptps";
    case kEg2RngHtBucket:
      return "bucket";
    case kEg2RngHtMarkovHi:
      return "markov_hi";
    case kEg2RngHtMarkovLo:
      return "markov_lo";
    case kEg2RngHtExthtHi:
      return "extht_hi";
    default:
      return "extht_lo";
  }
}

void eg2_rng_begin(const char *test, bool inject_fail) {
  rng_test = test;
  rng_corrupt = inject_fail;
#ifdef __CHERIOT__
  head("test", "CHERIoT mode");
#else
  head("test", "RISC-V mode");
#endif
  if (inject_fail) {
    eg2_puts(" (inject-fail build: the first check expects a wrong value and must fail)");
  }
  eg2_puts("\r\n");
}

bool eg2_rng_step(const char *what, bool ok) {
  head(ok ? "ok" : "FAIL", what);
  eg2_puts("\r\n");
  return ok;
}

bool eg2_rng_check(const char *what, uint32_t expected, uint32_t got) {
  bool corrupt = rng_corrupt;
  rng_corrupt = false;
  uint32_t exp = corrupt ? expected ^ 1u : expected;
  bool ok = exp == got;
  head(ok ? "ok" : "FAIL", what);
  eg2_puts(" expected 0x");
  eg2_puthex(exp);
  eg2_puts(" got 0x");
  eg2_puthex(got);
  if (corrupt) {
    eg2_puts(" (expected value corrupted: EG2_RNG_INJECT_FAIL)");
  }
  eg2_puts("\r\n");
  return ok;
}

void eg2_rng_note(const char *what, uint32_t value) {
  head("note", what);
  eg2_puts(" 0x");
  eg2_puthex(value);
  eg2_puts("\r\n");
}

void eg2_rng_gap(const char *what, const char *why) {
  head("gap", what);
  eg2_puts(": ");
  eg2_puts(why);
  eg2_puts("\r\n");
}

void eg2_rng_model_gap(const char *what) {
  head("model_gap", what);
  eg2_puts(" (skipped: model_gaps = 1, the SystemC model does not model it)\r\n");
}

uint32_t eg2_rng_es_read(uint32_t offset) { return eg2_dev_read32(kEg2DevEntropySrc, offset); }

void eg2_rng_es_write(uint32_t offset, uint32_t value) {
  eg2_dev_write32(kEg2DevEntropySrc, offset, value);
}

bool eg2_rng_check_reset_state(void) {
  // The test ROM enables entropy_src, csrng and EDN0 before the test (test_rom_start.S: "the
  // minimum level of entropy required to initialize memory scrambling"), so the test starts with
  // MODULE_ENABLE true, and every test stops the complex before it configures it.
  return eg2_rng_check("entropy_src enabled by the test ROM at the start (MODULE_ENABLE true)",
                       kMultiBitBool4True,
                       eg2_rng_es_read(ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET) & 0xf);
}

void eg2_rng_bench_step(uint32_t step) {
  eg2_puts("RNG bench step ");
  eg2_putdec(step);
  eg2_puts("\r\n");
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET,
                  kEg2RngBenchCode | (step & 0xff));
  // The bench acts in the simulation step it sees the code; a few instructions are margin.
  for (volatile uint32_t i = 0; i < 16; ++i) {
  }
  eg2_result_in_test();
}

bool eg2_rng_stop_all(void) {
  return eg2_rng_step("entropy complex stopped (EDN, csrng, entropy_src)",
                      status_ok(entropy_testutils_stop_all()));
}

void eg2_rng_thresholds_off(eg2_rng_es_cfg_t *cfg) {
  for (uint32_t i = 0; i < kEg2RngHtCount; ++i) {
    cfg->thresh[i] = eg2_rng_ht_is_lo(i) ? 0 : 0xffff;
  }
}

static uint32_t mubi4(bool b) { return b ? kMultiBitBool4True : kMultiBitBool4False; }

void eg2_rng_es_configure(const eg2_rng_es_cfg_t *cfg) {
  bool fips = cfg->mode != kEg2RngBoot;
  bool es_type = cfg->mode == kEg2RngBypass;
  for (uint32_t i = 0; i < kEg2RngHtCount; ++i) {
    eg2_rng_es_write(ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET + 4 * i, cfg->thresh[i]);
  }
  eg2_rng_es_write(ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   (uint32_t)cfg->fips_window | (384u << ENTROPY_SRC_HEALTH_TEST_WINDOWS_BYPASS_WINDOW_OFFSET));
  eg2_rng_es_write(ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   (uint32_t)cfg->alert_threshold |
                       ((uint32_t)(uint16_t)~cfg->alert_threshold << 16));
  eg2_rng_es_write(ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   (mubi4(cfg->route_to_fw || es_type) << ENTROPY_SRC_ENTROPY_CONTROL_ES_ROUTE_OFFSET) |
                       (mubi4(es_type) << ENTROPY_SRC_ENTROPY_CONTROL_ES_TYPE_OFFSET));
  eg2_rng_es_write(ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   (mubi4(cfg->fw_ov || cfg->insert) << ENTROPY_SRC_FW_OV_CONTROL_FW_OV_MODE_OFFSET) |
                       (mubi4(cfg->insert) << ENTROPY_SRC_FW_OV_CONTROL_FW_OV_ENTROPY_INSERT_OFFSET));
  eg2_rng_es_write(ENTROPY_SRC_OBSERVE_FIFO_THRESH_REG_OFFSET, 32);
  eg2_rng_es_write(
      ENTROPY_SRC_CONF_REG_OFFSET,
      (mubi4(fips) << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
          (mubi4(fips) << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
          (mubi4(cfg->rng_fips) << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
          (mubi4(false) << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
          (mubi4(false) << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
          (mubi4(cfg->route_to_fw || es_type) << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET));
  // Recoverable alert causes are kept across a disable (programmer's guide): clear them (rw0c).
  eg2_rng_es_write(ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0);
  // Interrupts left from an earlier phase.
  eg2_rng_es_write(ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xf);
}

bool eg2_rng_es_enable(bool enable) {
  eg2_rng_es_write(ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET, mubi4(enable));
  if (enable) {
    return true;
  }
  // Programmer's guide, "Entropy Source Module Disable": poll DEBUG_STATUS bit 16.
  for (uint32_t i = 0; i < kEg2RngPolls; ++i) {
    if ((eg2_rng_es_read(ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET) >> kDebugStatusMainSmIdleBit) & 1) {
      return true;
    }
  }
  return eg2_rng_step("entropy_src idle after the disable (DEBUG_STATUS.MAIN_SM_IDLE)", false);
}

bool eg2_rng_es_read_seed(uint32_t words[12], uint32_t polls) {
  for (uint32_t w = 0; w < 12; ++w) {
    uint32_t i = 0;
    while (!((eg2_rng_es_read(ENTROPY_SRC_INTR_STATE_REG_OFFSET) >>
              ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT) & 1)) {
      if (++i >= polls) {
        return false;
      }
    }
    words[w] = eg2_rng_es_read(ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
    // As dif_entropy_src_non_blocking_read: clear es_entropy_valid after the read; it is set
    // again while words remain.
    eg2_rng_es_write(ENTROPY_SRC_INTR_STATE_REG_OFFSET,
                     1u << ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT);
  }
  return true;
}

uint32_t eg2_rng_fail_count(uint32_t ht) {
  if (ht >= kEg2RngHtExthtHi) {
    return (eg2_rng_es_read(ENTROPY_SRC_EXTHT_FAIL_COUNTS_REG_OFFSET) >>
            (4 * (ht - kEg2RngHtExthtHi))) & 0xf;
  }
  return (eg2_rng_es_read(ENTROPY_SRC_ALERT_FAIL_COUNTS_REG_OFFSET) >> (4 * ht)) & 0xf;
}

bool eg2_rng_main_sm_alert(uint32_t *state) {
  *state = eg2_rng_es_read(ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) & 0x1ff;
  return *state == kMainSmAlertState || *state == kMainSmAlertHang;
}

bool eg2_rng_csrng_edn0_auto(uint32_t reseed_interval) {
  dif_csrng_t csrng;
  dif_edn_t edn0;
  if (dif_csrng_init_from_dt(kDtCsrng, &csrng) != kDifOk ||
      dif_edn_init_from_dt(kDtEdn0, &edn0) != kDifOk || dif_csrng_configure(&csrng) != kDifOk) {
    return eg2_rng_step("csrng enabled", false);
  }
  dif_edn_auto_params_t params = {
      .instantiate_cmd = {.cmd = csrng_cmd_header_build(kCsrngAppCmdInstantiate,
                                                        kDifCsrngEntropySrcToggleEnable, 0, 0),
                          .seed_material = {.len = 0}},
      .reseed_cmd = {.cmd = csrng_cmd_header_build(kCsrngAppCmdReseed,
                                                   kDifCsrngEntropySrcToggleEnable, 0, 0),
                     .seed_material = {.len = 0}},
      .generate_cmd = {.cmd = csrng_cmd_header_build(kCsrngAppCmdGenerate,
                                                     kDifCsrngEntropySrcToggleEnable, 0, 1),
                       .seed_material = {.len = 0}},
      .reseed_interval = reseed_interval,
  };
  return eg2_rng_step("EDN0 in auto mode (generate 1 block, reseed interval set)",
                      dif_edn_set_auto_mode(&edn0, params) == kDifOk);
}

bool eg2_rng_rnd_read(uint32_t *word, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    uint32_t st = eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
    if ((st >> RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT) & 1) {
      *word = eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_RND_DATA_REG_OFFSET);
      return true;
    }
  }
  return false;
}

// One LOG_INFO format per tap and mode: the DV log carries no string arguments.
#define EG2_RNG_FMT_ "%08x %08x %08x %08x %08x %08x %08x %08x"
#define EG2_RNG_W_ w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]
void eg2_rng_log_line(uint32_t tap, eg2_rng_esmode_t mode, const uint32_t w[8]) {
  switch (tap * 3 + (uint32_t)mode) {
    case 0:
      LOG_INFO("EG2_RNG raw_boot " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 1:
      LOG_INFO("EG2_RNG raw_fips " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 2:
      LOG_INFO("EG2_RNG raw_bypass " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 3:
      LOG_INFO("EG2_RNG cond_boot " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 4:
      LOG_INFO("EG2_RNG cond_fips " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 5:
      LOG_INFO("EG2_RNG cond_bypass " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 6:
      LOG_INFO("EG2_RNG drbg_boot " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 7:
      LOG_INFO("EG2_RNG drbg_fips " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 8:
      LOG_INFO("EG2_RNG drbg_bypass " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 9:
      LOG_INFO("EG2_RNG edn_boot " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    case 10:
      LOG_INFO("EG2_RNG edn_fips " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
    default:
      LOG_INFO("EG2_RNG edn_bypass " EG2_RNG_FMT_, EG2_RNG_W_);
      break;
  }
}
