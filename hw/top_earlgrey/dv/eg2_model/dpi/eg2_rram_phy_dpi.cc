// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The rram_ctrl PHY reference model (ip/eg2_rram_phy_core.h) behind a C interface for SystemVerilog
// DPI (hw/ip/rram_ctrl/dv/edagen/rram_phy/dv/rram_phy_cmodel_pkg.sv). 128-bit values travel as four
// 32-bit words, least significant first (svBitVecVal); 39-bit bus words (32 data + 7 integrity) as
// two.

#include <cstdint>
#include <cstring>

#include "../ip/eg2_rram_phy_core.h"

using eg2_model::RramPhy;
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
RramPhy *phy(void *h) { return static_cast<RramPhy *>(h); }
}  // namespace

extern "C" {

void *eg2_rram_phy_create(const uint32_t *rst_addr_key, const uint32_t *rst_data_key) {
  return new RramPhy(from_words(rst_addr_key), from_words(rst_data_key));
}
void eg2_rram_phy_release(void *h) { delete phy(h); }

void eg2_rram_phy_step(void *h, unsigned int keys_disable, int keys_valid, const uint32_t *ak,
                       const uint32_t *dk, const uint32_t *rak, const uint32_t *rdk) {
  phy(h)->scr.step(keys_disable, keys_valid != 0, from_words(ak), from_words(dk), from_words(rak),
                   from_words(rdk));
}
unsigned int eg2_rram_phy_epoch(void *h) { return phy(h)->scr.epoch; }

void eg2_rram_phy_preload(void *h, int part, unsigned int addr, const uint32_t *d) {
  phy(h)->preload(part != 0, addr, from_words(d));
}
void eg2_rram_phy_set_status(void *h, int part, unsigned int addr, unsigned int st) {
  phy(h)->set_status(part != 0, addr, st);
}

void eg2_rram_phy_wr_word(void *h, unsigned int bus_addr, const uint32_t *d39, int part, int scr,
                          int ecc, int xr, int last) {
  uint64_t d = uint64_t(d39[0]) | (uint64_t(d39[1] & 0x7f) << 32);
  phy(h)->ctrl_wr_word(bus_addr, d, part != 0, scr != 0, ecc != 0, xr != 0, last != 0);
}

int eg2_rram_phy_macro_wr(void *h, int last, unsigned int addr, int part, int ecc,
                          const uint32_t *d) {
  return phy(h)->macro_wr(last != 0, addr, part != 0, ecc != 0, from_words(d)) ? 1 : 0;
}

const char *eg2_rram_phy_error(void *h) { return phy(h)->error.c_str(); }
unsigned int eg2_rram_phy_pending(void *h) { return unsigned(phy(h)->expected.size()); }
unsigned int eg2_rram_phy_wr_intg_errs(void *h) { return phy(h)->wr_intg_errs; }

void eg2_rram_phy_read(void *h, unsigned int bus_addr, int part, int scr, int ecc, int xr, int *err,
                       uint32_t *d39) {
  bool e = false;
  uint64_t d = phy(h)->read(bus_addr, part != 0, scr != 0, ecc != 0, xr != 0, &e);
  *err = e ? 1 : 0;
  d39[0] = uint32_t(d);
  d39[1] = uint32_t(d >> 32) & 0x7f;
}

// The bus integrity of a 32-bit word, for the bench's write data.
unsigned int eg2_rram_phy_intg(unsigned int w) { return RramPhy::intg(w); }

}  // extern "C"
