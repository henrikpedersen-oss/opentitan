// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The clock manager's reference model for the clkmgr block bench (clkmgr_cmodel_sb): the core of
// ip/eg2_clkmgr_core.h behind a C interface for SystemVerilog DPI (eg2_clkmgr_cmodel_pkg.sv).
// Bounds: `lo` the bits that must read 1, `hi` the bits that may read 1 (eg2_clkmgr_core.h says
// what is left open and why).

#include <cstdint>

#include "../ip/eg2_clkmgr_core.h"

namespace {
inline Eg2ClkmgrCore *C(void *h) { return static_cast<Eg2ClkmgrCore *>(h); }
}  // namespace

extern "C" {

void *eg2_clk_create(int mutate) { return new Eg2ClkmgrCore(mutate); }

void eg2_clk_release(void *h) { delete C(h); }

void eg2_clk_reset(void *h) {
  if (h) C(h)->por();
}

void eg2_clk_csr_write(void *h, uint32_t addr, uint32_t data, int quiet) {
  if (h) C(h)->write(addr, data, quiet != 0);
}

void eg2_clk_csr_read(void *h, uint32_t addr, int quiet, uint32_t *lo, uint32_t *hi) {
  *lo = 0;
  *hi = 0xffffffffu;
  if (h) C(h)->read(addr, quiet != 0, *lo, *hi);
}

// idle: four mubi4 values, aes in bits 3:0; en / run: main bit 0, io bit 1, usb bit 2.
void eg2_clk_inputs(void *h, uint32_t idle, uint32_t scanmode, uint32_t lc_debug_en,
                    uint32_t lc_byp_req, uint32_t io_byp_ack, uint32_t all_byp_ack,
                    uint32_t step_down, uint32_t calib, uint32_t en, uint32_t run) {
  if (!h) return;
  Eg2ClkmgrCore::Inputs in;
  for (unsigned b = 0; b < 4; ++b) in.idle[b] = (idle >> (4 * b)) & 0xf;
  in.scanmode = scanmode & 0xf;
  in.lc_debug_en = lc_debug_en & 0xf;
  in.lc_byp_req = lc_byp_req & 0xf;
  in.io_byp_ack = io_byp_ack & 0xf;
  in.all_byp_ack = all_byp_ack & 0xf;
  in.step_down = step_down & 0xf;
  in.calib = calib & 0xf;
  in.en_main = en & 1;
  in.en_io = (en >> 1) & 1;
  in.en_usb = (en >> 2) & 1;
  in.run_main = run & 1;
  in.run_io = (run >> 1) & 1;
  in.run_usb = (run >> 2) & 1;
  C(h)->set_inputs(in);
}

void eg2_clk_settled(void *h) {
  if (h) C(h)->settled();
}

// cnt_*: the source clock's rising edges in the always-on period; steady: bit 0 io, 1 main, 2 usb.
void eg2_clk_aon_period(void *h, uint32_t cnt_io, uint32_t cnt_main, uint32_t cnt_usb,
                        uint32_t steady) {
  if (!h) return;
  const uint32_t cnt[3] = {cnt_io, cnt_main, cnt_usb};
  const bool st[3] = {(steady & 1) != 0, (steady & 2) != 0, (steady & 4) != 0};
  C(h)->aon_period(cnt, st);
}

void eg2_clk_outputs(void *h, int quiet, uint32_t *lo, uint32_t *hi) {
  *lo = 0;
  *hi = 0xffffffffu;
  if (h) C(h)->outputs(quiet != 0, *lo, *hi);
}

uint32_t eg2_clk_sel(void *h) { return h && C(h)->sel(); }

uint32_t eg2_clk_sel_other(void *h) { return h && C(h)->sel_other(); }

// Counts for the report: 0 meas_errors, 1 meas_ok, 2 update_errors, 3 commits, 4 aon_periods,
// 5 settled.
uint32_t eg2_clk_stat(void *h, int which) {
  if (!h) return 0;
  const Eg2ClkmgrCore::Stats &s = C(h)->stats();
  switch (which) {
    case 0: return uint32_t(s.meas_errors);
    case 1: return uint32_t(s.meas_ok);
    case 2: return uint32_t(s.update_errors);
    case 3: return uint32_t(s.commits);
    case 4: return uint32_t(s.aon_periods);
    case 5: return uint32_t(s.settled);
    default: return 0;
  }
}

}  // extern "C"
