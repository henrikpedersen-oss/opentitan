// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI functions of the rram_ctrl reference models: memory protection (ip/eg2_rram_mp_core.h) and
// scrambling (ip/eg2_rram_scramble_core.h), for the unit benches under
// hw/ip/rram_ctrl/dv/edagen and the self-test eg2_rram_dpi_test.cc. 128-bit values are four 32-bit
// words, least significant first (SystemVerilog's svBitVecVal layout of a bit [127:0]).

#include <cstdint>

#include "../ip/eg2_rram_mp_core.h"
#include "../ip/eg2_rram_scramble_core.h"

using eg2_model::RramMp;
using eg2_model::RramScramble;
using eg2_model::RramTri;
using eg2_model::rram_u128;

namespace {
rram_u128 from_words(const uint32_t *w) {
  rram_u128 v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 32) | w[i];
  return v;
}
void to_words(rram_u128 v, uint32_t *w) {
  for (int i = 0; i < 4; ++i) w[i] = uint32_t(v >> (32 * i));
}
RramTri tri(unsigned v) { return v == 0 ? eg2_model::kRramNo : (v == 1 ? eg2_model::kRramYes : eg2_model::kRramUnpred); }
}  // namespace

extern "C" {

// ---- memory protection ----

void *eg2_rram_mp_create() { return new RramMp(); }
void eg2_rram_mp_release(void *h) { delete static_cast<RramMp *>(h); }

// kind: 0 MP_REGION, 1 MP_REGION_CFG, 2 DEFAULT_REGION, 3 INFO_PAGE_CFG, 4 EMUL_INFO_REGWEN,
// 5 EMUL_INFO_REGION, 6 EMUL_INFO_SUBREGION_REGWEN, 7 EMUL_INFO_SUBREGION, 8
// EMUL_INFO_SUBREGION_CFG, 9 HW_INFO_CFG_OVERRIDE, 10 life-cycle enable idx (0 creator seed, 1
// owner seed, 2 isolated read, 3 isolated write) with value 0 off, 1 on, 2 settling (unpredicted).
void eg2_rram_mp_set(void *h, int kind, int idx, unsigned int value) {
  RramMp &m = *static_cast<RramMp *>(h);
  switch (kind) {
    case 0: m.region[idx] = value; break;
    case 1: m.region_cfg[idx] = value; break;
    case 2: m.default_cfg = value; break;
    case 3: m.info_cfg[idx] = value; break;
    case 4: m.emul_regwen[idx] = value; break;
    case 5: m.emul_region[idx] = value; break;
    case 6: m.sub_regwen[idx] = value; break;
    case 7: m.sub_top[idx] = value; break;
    case 8: m.sub_cfg[idx] = value; break;
    case 9: m.hw_info_override = value; break;
    case 10: {
      RramTri t = tri(value);
      if (idx == 0) m.lc_creator = t;
      else if (idx == 1) m.lc_owner = t;
      else if (idx == 2) m.lc_iso_rd = t;
      else m.lc_iso_wr = t;
      break;
    }
    default: break;
  }
}

// Outputs are 0 no, 1 yes, 2 not predicted; rule and idx say which rule decided (RramMp::Rule).
void eg2_rram_mp_ctrl(void *h, unsigned int sel, unsigned int phase, unsigned int part,
                      unsigned int page, int write, int ovfl, unsigned int rram_disable, int *err,
                      int *fwd, int *scr, int *ecc, int *xr, int *rule, int *idx) {
  RramMp::Ctrl c = static_cast<RramMp *>(h)->ctrl(sel, phase, part, page, write != 0, ovfl != 0,
                                                 rram_disable);
  *err = c.err;
  *fwd = c.fwd;
  *scr = c.scr;
  *ecc = c.ecc;
  *xr = c.xr;
  *rule = c.rule;
  *idx = c.idx;
}

void eg2_rram_mp_host(void *h, unsigned int page, unsigned int sw_disable,
                      unsigned int rram_disable, int *err, int *fwd, int *scr, int *ecc,
                      int *rule, int *idx) {
  RramMp::Host r = static_cast<RramMp *>(h)->host(page, sw_disable, rram_disable);
  *err = r.err;
  *fwd = r.fwd;
  *scr = r.scr;
  *ecc = r.ecc;
  *rule = r.rule;
  *idx = r.idx;
}

// ---- scrambling ----

void *eg2_rram_scr_create(const uint32_t *rst_addr_key, const uint32_t *rst_data_key) {
  return new RramScramble(from_words(rst_addr_key), from_words(rst_data_key));
}
void eg2_rram_scr_release(void *h) { delete static_cast<RramScramble *>(h); }

void eg2_rram_scr_step(void *h, unsigned int keys_disable, int keys_valid, const uint32_t *ak,
                       const uint32_t *dk, const uint32_t *rak, const uint32_t *rdk) {
  static_cast<RramScramble *>(h)->step(keys_disable, keys_valid != 0, from_words(ak), from_words(dk),
                                       from_words(rak), from_words(rdk));
}

unsigned int eg2_rram_scr_epoch(void *h) { return static_cast<RramScramble *>(h)->epoch; }

void eg2_rram_scr_mask(void *h, unsigned int addr, uint32_t *mask) {
  to_words(static_cast<RramScramble *>(h)->mask(addr), mask);
}

void eg2_rram_scr_op(void *h, const uint32_t *din, int decrypt, uint32_t *dout) {
  to_words(static_cast<RramScramble *>(h)->op(from_words(din), decrypt != 0), dout);
}

}  // extern "C"
