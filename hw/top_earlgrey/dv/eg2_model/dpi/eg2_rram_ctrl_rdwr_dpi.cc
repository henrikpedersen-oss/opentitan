// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The controller-path reference model (ip/eg2_rram_ctrl_rdwr_core.h: rram_ctrl_arb, rram_ctrl_rd,
// rram_ctrl_wr) behind a C interface for SystemVerilog DPI, for the rram_ctrl_rdwr unit bench
// (edagen rram_ctrl_rdwr.yaml, dv/rram_ctrl_rdwr_cmodel_pkg.sv; the imports are in
// eg2_rram_ctrl_rdwr_cmodel_pkg.sv). Functions that judge an observation return 0 when the
// documents allow it, else 1, with the reason in eg2_rdwr_error(). 39-bit words (32 data bits and
// 7 bus-integrity bits) travel as 64-bit values.

#include <cstdint>
#include <string>

#include "../ip/eg2_rram_ctrl_rdwr_core.h"

namespace {
struct Model {
  eg2_model::RramCtrlRdWr core;
  std::string last, end;
};
Model *m(void *h) { return static_cast<Model *>(h); }
int judge(void *h, const std::string &why) {
  if (!why.empty()) m(h)->last = why;
  return why.empty() ? 0 : 1;
}
}  // namespace

extern "C" {

void *eg2_rdwr_create() { return new Model(); }
void eg2_rdwr_release(void *h) { delete m(h); }
void eg2_rdwr_reset(void *h) { m(h)->core.reset(); }
const char *eg2_rdwr_error(void *h) { return m(h)->last.c_str(); }

void eg2_rdwr_cycle(void *h, int phy_init, int ctrl_init, int dis, int sw_dis) {
  m(h)->core.cycle(phy_init != 0, ctrl_init != 0, dis != 0, sw_dis != 0);
}
int eg2_rdwr_request(void *h, int who, int op, int part, int num, int addr, int phase) {
  return judge(h, m(h)->core.request(who, uint32_t(op), uint32_t(part), uint32_t(num),
                                     uint32_t(addr), uint32_t(phase)));
}
void eg2_rdwr_push(void *h, int who, uint64_t d39) { m(h)->core.push(who, d39); }
int eg2_rdwr_rd_req(void *h, int addr, int ovfl, int sel, int part, int phase) {
  return judge(h, m(h)->core.rd_req(uint32_t(addr), ovfl != 0, uint32_t(sel), uint32_t(part),
                                    uint32_t(phase)));
}
int eg2_rdwr_rd_rsp(void *h, uint64_t d39, int rd_err, int mp_err) {
  return judge(h, m(h)->core.rd_rsp(d39, rd_err != 0, mp_err != 0));
}
int eg2_rdwr_deliver(void *h, int port, uint64_t d39, int check) {
  return judge(h, m(h)->core.deliver(port, d39, check != 0));
}
int eg2_rdwr_wr_req(void *h, int addr, int ovfl, uint64_t d39, int last, int sel, int part,
                    int phase) {
  return judge(h, m(h)->core.wr_req(uint32_t(addr), ovfl != 0, d39, last != 0, uint32_t(sel),
                                    uint32_t(part), uint32_t(phase)));
}
int eg2_rdwr_wr_rsp(void *h, int mp_err, int intg_err) {
  return judge(h, m(h)->core.wr_rsp(mp_err != 0, intg_err != 0));
}
int eg2_rdwr_done(void *h, int who, int err, int err_addr) {
  return judge(h, m(h)->core.done(who, uint32_t(err), uint32_t(err_addr)));
}
const char *eg2_rdwr_end(void *h) {
  m(h)->end = m(h)->core.end_check();
  return m(h)->end.c_str();
}

// What the bench needs for its stimulus and coverage.
uint64_t eg2_rdwr_with_intg(int w) { return eg2_model::RramCtrlRdWr::with_intg(uint32_t(w)); }
uint64_t eg2_rdwr_ones() { return eg2_model::RramCtrlRdWr::ones39(); }
int eg2_rdwr_on(void *h) { return m(h)->core.cur.on; }
int eg2_rdwr_who(void *h) { return m(h)->core.cur.on ? m(h)->core.cur.who : 3; }
int eg2_rdwr_pending(void *h, int who) { return m(h)->core.rq[who & 3].pending; }
int eg2_rdwr_expected_left(void *h) { return int(m(h)->core.cur.expect.size()); }
// 1 when the last eg2_rdwr_done failed only on ERR_ADDR, the failing word being the last one.
int eg2_rdwr_known_erraddr(void *h) { return m(h)->core.known_erraddr_last; }
// 0 rejected, 1 done, 2 words read, 3 words written, 4 completions with an error, 5 cycles with
// software and hardware requests waiting, 6 cycles with both plugs waiting, 7 requests left
// unanswered after rram_disable (end_check first), 8+who*3+op operations begun.
int eg2_rdwr_stat(void *h, int which) {
  const eg2_model::RramCtrlRdWr &c = m(h)->core;
  switch (which) {
    case 0: return int(c.n_rejected);
    case 1: return int(c.n_done);
    case 2: return int(c.n_words_rd);
    case 3: return int(c.n_words_wr);
    case 4: return int(c.n_err_done);
    case 5: return int(c.n_contended);
    case 6: return int(c.n_hw_both);
    case 7: return int(c.n_unanswered_after_dis);
    default: break;
  }
  int k = which - 8;
  if (k >= 0 && k < 9) return int(c.n_ops[k / 3][k % 3]);
  return 0;
}

}  // extern "C"
