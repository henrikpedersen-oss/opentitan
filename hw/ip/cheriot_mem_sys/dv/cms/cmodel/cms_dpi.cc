// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// cms_dpi.cc: the C interface of cms_core.h for SystemVerilog DPI (cms_cmodel_pkg.sv). One handle
// per model; every check returns its number of mismatches, and cms_last_messages() their text.

#include <string>

#include "cms_core.h"

namespace {
std::string g_msgs;
cms::Model *M(void *h) { return static_cast<cms::Model *>(h); }
int done(void *h, int n) {
  if (n) {
    g_msgs.clear();
    for (const std::string &s : M(h)->messages()) g_msgs += s + "\n";
  }
  M(h)->clear_messages();
  return n;
}
}  // namespace

extern "C" {

// map: 0 Earl Grey, 1 the cms bench's small map.
void *cms_new(int map) { return new cms::Model(map ? cms::Map::cms_small() : cms::Map::earlgrey()); }
void cms_free(void *h) { delete M(h); }
void cms_reset(void *h) { M(h)->reset(); }
void cms_dut_reset(void *h) { M(h)->dut_reset(); }
void cms_open_tag(void *h, unsigned addr) { M(h)->open_tag(addr); }
void cms_set_mode(void *h, int ena) { M(h)->set_mode(uint8_t(ena)); }

void cms_backdoor_data(void *h, unsigned addr, unsigned word) { M(h)->backdoor_data(addr, word); }
void cms_backdoor_tag(void *h, unsigned addr, int tag) { M(h)->backdoor_tag(addr, tag); }
void cms_backdoor_revbm(void *h, unsigned word_idx, unsigned v) { M(h)->backdoor_revbm(word_idx, v); }

int cms_core(void *h, int op, unsigned addr, int size, int mask, unsigned wdata, int tag, int ena,
             int host_err, int meta_err, int err, unsigned rdata, int rtag) {
  cms::CoreTxn t{};
  t.op = uint8_t(op); t.addr = addr; t.size = uint8_t(size); t.mask = uint8_t(mask);
  t.wdata = wdata; t.tag = tag; t.ena = uint8_t(ena); t.host_err = host_err;
  t.meta_err = meta_err; t.err = err; t.rdata = rdata; t.rtag = rtag;
  return done(h, M(h)->core(t));
}

int cms_bitmap(void *h, int core_port, int op, unsigned off, int size, int mask, unsigned wdata,
               int ena, int err, unsigned rdata) {
  return done(h, M(h)->bitmap(core_port, uint8_t(op), off, uint8_t(size), uint8_t(mask), wdata,
                              uint8_t(ena), err, rdata));
}

int cms_csr(void *h, int write, unsigned off, unsigned wdata, unsigned rdata) {
  return done(h, M(h)->csr(write, off, wdata, rdata));
}

void cms_sweep_read_error(void *h, unsigned addr) { M(h)->sweep_read_error(addr); }
void cms_sweep_fault(void *h, int on_bitmap_lookup, unsigned addr) {
  M(h)->sweep_fault(on_bitmap_lookup, addr);
}
void cms_sweep_end(void *h) { M(h)->sweep_end(); }
void cms_sweep_keep(void *h, unsigned addr) { M(h)->sweep_keep(addr); }
void cms_sweep_revoke(void *h, unsigned addr) { M(h)->sweep_revoke(addr); }
void cms_sweep_dont_care(void *h, unsigned addr) { M(h)->sweep_dont_care(addr); }
void cms_sweep_error(void *h, int alert) { M(h)->sweep_error(alert); }
int cms_sweep_active(void *h) { return M(h)->sweep_active(); }

int cms_intr_expected(void *h) { return M(h)->intr_expected(); }
int cms_alerts_expected(void *h) { return int(M(h)->alerts_expected()); }
// The tag the model allows at addr: 1 only 0, 2 only 1, 3 either.
int cms_tag(void *h, unsigned addr) { return M(h)->tag(addr); }
int cms_check_meta_word(void *h, unsigned off, unsigned actual) {
  return done(h, M(h)->check_meta_word(off, actual));
}
unsigned cms_meta_top(void *h) { return M(h)->map().meta_top_off(); }

const char *cms_last_messages() { return g_msgs.c_str(); }

}  // extern "C"
