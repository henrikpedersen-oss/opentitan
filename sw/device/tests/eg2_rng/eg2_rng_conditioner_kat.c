// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rng_conditioner_kat (EG2 verification spec "Random Number Generation Test Suite",
 * REQ_PBF_24): known words inserted into entropy_src's SHA-3 conditioner through the firmware
 * override (programmer's guide, "Firmware Override - Extract & Insert", with hardware
 * conditioning: CONF.FIPS_ENABLE, ES_ROUTE to firmware and ES_TYPE false, FW_OV_MODE and
 * FW_OV_ENTROPY_INSERT; FW_OV_SHA3_START.FW_OV_INSERT_START true, the words into FW_OV_WR_DATA
 * while FW_OV_WR_FIFO_FULL is clear, then false), and the 384-bit result read from ENTROPY_DATA.
 * It must equal SHA3-384 of the inserted words, computed on the host by
 * gen_rng_conditioner_kat.py (the order is OpenTitan's own conditioner known answer, which the
 * table's first vector is).
 *
 * Before each vector the conditioner is flushed as the legacy known-answer test does (a start and
 * stop without data, the seed read and dropped): it keeps what it absorbed from aborted seeds.
 *
 * Knobs: `len_class` the messages run (0: 2 and 4 words, 1: 12 to 34 words, 2: 64 words, one
 * FIPS window, 3: 96 and 128 words, 4: every vector), `model_gaps`.
 * Gaps: odd word counts (the documents do not say how they are packed into the conditioner's
 * 64-bit input). model_gaps = 1 (eg2sim: no firmware override in the model) runs only the first
 * check.
 */

#include "hw/top/entropy_src_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_rng/eg2_rng.h"
#include "sw/device/tests/eg2_rng/eg2_rng_conditioner_kat_table.h"

EG2_TEST_CONFIG();

#define RNG_KAT_PARAMS(X)      \
  X(len_class, 4, 0, 4)        \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(RNG_KAT_PARAMS)

static bool insert_and_read(const uint32_t *msg, uint32_t n, uint32_t out[12]) {
  eg2_rng_es_write(ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET, kMultiBitBool4True);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t p = 0;
    while (eg2_rng_es_read(ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) & 1) {
      if (++p >= kEg2RngPolls) {
        return eg2_rng_step("FW_OV_WR_FIFO_FULL clears", false);
      }
    }
    eg2_rng_es_write(ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, msg[i]);
  }
  // FW_OV_SHA3_START: "check that FW_OV_WR_FIFO_FULL is clear before setting this field to
  // kMultiBitBool4False".
  uint32_t p = 0;
  while (eg2_rng_es_read(ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) & 1) {
    if (++p >= kEg2RngPolls) {
      return eg2_rng_step("FW_OV_WR_FIFO_FULL clears before the stop", false);
    }
  }
  eg2_rng_es_write(ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET, kMultiBitBool4False);
  return eg2_rng_es_read_seed(out, kEg2RngPolls);
}

bool eg2_test_main(void) {
  eg2_rng_begin("rng_conditioner_kat", EG2_RNG_INJECT);
  bool ok = eg2_rm_params_begin();
  ok = eg2_rng_check_reset_state() && ok;
  eg2_rng_gap("odd word counts", "their packing into the 64-bit conditioner input is undocumented");
  if (EG2_PARAM(model_gaps)) {
    eg2_rng_model_gap("the firmware override (FW_OV_*: extract and insert)");
    return ok;
  }
  eg2_rng_es_cfg_t cfg = {
      .mode = kEg2RngFips,
      .route_to_fw = true,
      .fw_ov = true,
      .insert = true,
      .fips_window = 512,
      .alert_threshold = 0,
  };
  eg2_rng_thresholds_off(&cfg);
  ok = eg2_rng_stop_all() && ok;
  uint32_t ran = 0;
  for (uint32_t v = 0; v < kKatNum; ++v) {
    if (EG2_PARAM(len_class) != 4 && kKatClass[v] != EG2_PARAM(len_class)) {
      continue;
    }
    eg2_rng_es_enable(false);
    eg2_rng_es_configure(&cfg);
    eg2_rng_es_enable(true);
    uint32_t got[12];
    // The flush (see the file comment): an empty message, its result dropped.
    ok = eg2_rng_step("conditioner flushed", insert_and_read(kKatMsg, 0, got)) && ok;
    eg2_puts("RNG kat vector ");
    eg2_putdec(v);
    eg2_puts(", words ");
    eg2_putdec(kKatLen[v]);
    eg2_puts("\r\n");
    bool read = insert_and_read(&kKatMsg[kKatOff[v]], kKatLen[v], got);
    ok = eg2_rng_step("conditioned seed read from ENTROPY_DATA", read) && ok;
    uint32_t diff = 0;
    for (uint32_t i = 0; i < 12 && read; ++i) {
      if (got[i] != kKatDigest[v][i]) {
        if (diff == 0) {
          eg2_rng_note("first differing word index", i);
          eg2_rng_note("  expected", kKatDigest[v][i]);
          eg2_rng_note("  got", got[i]);
        }
        ++diff;
      }
    }
    ok = eg2_rng_check("words differing from SHA3-384 of the inserted words", 0, diff) && ok;
    ++ran;
  }
  ok = eg2_rng_step("at least one vector ran", ran > 0) && ok;
  eg2_rng_es_enable(false);
  ok = eg2_rng_stop_all() && ok;
  return ok;
}
