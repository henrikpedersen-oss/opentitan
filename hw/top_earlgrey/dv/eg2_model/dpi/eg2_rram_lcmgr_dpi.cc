// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The life-cycle manager plug's reference model (ip/eg2_rram_lcmgr_core.h) behind a C interface for
// SystemVerilog DPI, for the rram_ctrl_lcmgr unit bench (edagen rram_ctrl_lcmgr.yaml,
// dv/rram_ctrl_lcmgr_cmodel_pkg.sv). Functions that judge an observation return 0 when the documents
// allow it, else 1, with the reason in eg2_lcmgr_last_error(). Keys are passed as four 32-bit words,
// least significant first.

#include <array>
#include <cstdint>
#include <string>

#include "../ip/eg2_rram_lcmgr_core.h"

namespace {
struct Model {
  Eg2RramLcmgrCore core;
  std::string last;
};
Model *m(void *h) { return static_cast<Model *>(h); }
int judge(void *h, const std::string &why) {
  m(h)->last = why;
  return why.empty() ? 0 : 1;
}
}  // namespace

extern "C" {

void *eg2_lcmgr_create() { return new Model(); }
void eg2_lcmgr_release(void *h) { delete m(h); }
void eg2_lcmgr_reset(void *h) { m(h)->core.reset(); }
const char *eg2_lcmgr_last_error(void *h) { return m(h)->last.c_str(); }

void eg2_lcmgr_init(void *h) { m(h)->core.init(); }
void eg2_lcmgr_lc_seed(void *h, int v) { m(h)->core.lc_seed_hw_rd_en(uint32_t(v)); }
void eg2_lcmgr_rma_req(void *h, int v) { m(h)->core.rma_req(uint32_t(v)); }
void eg2_lcmgr_disable(void *h, int v) { m(h)->core.rram_disable(uint32_t(v)); }

int eg2_lcmgr_key_req(void *h, int kind) { return judge(h, m(h)->core.key_req(kind)); }
void eg2_lcmgr_key_ack(void *h, int kind, int k0, int k1, int k2, int k3, int r0, int r1, int r2,
                       int r3) {
  std::array<uint32_t, 4> k{uint32_t(k0), uint32_t(k1), uint32_t(k2), uint32_t(k3)};
  std::array<uint32_t, 4> r{uint32_t(r0), uint32_t(r1), uint32_t(r2), uint32_t(r3)};
  m(h)->core.key_ack(kind, k, r);
}
void eg2_lcmgr_provisioned_was(void *h, int p) { m(h)->core.provisioned_was(p != 0); }

int eg2_lcmgr_rma_started(void *h) { return judge(h, m(h)->core.rma_started()); }
int eg2_lcmgr_request(void *h, int op, int part, int byte_addr, int num_words, int phase) {
  return judge(h, m(h)->core.request(uint32_t(op), uint32_t(part), uint32_t(byte_addr),
                                     uint32_t(num_words), uint32_t(phase)));
}
void eg2_lcmgr_seed_word(void *h, int idx, int data, int intg_ok) {
  m(h)->core.seed_word(uint32_t(idx), uint32_t(data), intg_ok != 0);
}
void eg2_lcmgr_seed_done(void *h, int err) { m(h)->core.seed_done(uint32_t(err)); }
void eg2_lcmgr_seed_err_now(void *h, int err) {
  if (err) m(h)->core.seed_rd_err_pending(m(h)->core.cur_seed());
}
void eg2_lcmgr_rma_write(void *h, int part, int word_addr) {
  m(h)->core.rma_write(uint32_t(part), uint32_t(word_addr));
}
void eg2_lcmgr_rma_read(void *h, int part, int word_addr, int same) {
  m(h)->core.rma_read(uint32_t(part), uint32_t(word_addr), same != 0);
}
void eg2_lcmgr_rma_error(void *h) { m(h)->core.rma_error(); }
int eg2_lcmgr_rma_ack_seen(void *h) { return judge(h, m(h)->core.rma_ack_seen()); }
void eg2_lcmgr_rma_fail_seen(void *h) { m(h)->core.rma_fail_seen(); }
const char *eg2_lcmgr_wipe_incomplete(void *h) {
  m(h)->last = m(h)->core.wipe_incomplete();
  return m(h)->last.c_str();
}

int eg2_lcmgr_state(void *h) { return int(m(h)->core.state()); }
const char *eg2_lcmgr_state_name(void *h) {
  return Eg2RramLcmgrCore::state_name(m(h)->core.state());
}
int eg2_lcmgr_keys_acked(void *h) { return m(h)->core.keys_acked(); }
int eg2_lcmgr_key_word(void *h, int which, int i) {
  const Eg2RramLcmgrCore &c = m(h)->core;
  const std::array<uint32_t, 4> &k = which == 0 ? c.addr_key() : which == 1 ? c.data_key()
                                   : which == 2 ? c.rand_addr_key() : c.rand_data_key();
  return int(k[i & 3]);
}
int eg2_lcmgr_provisioned(void *h) { return m(h)->core.provisioned(); }
int eg2_lcmgr_seed_evaluated(void *h, int s) { return m(h)->core.seed_evaluated(s); }
int eg2_lcmgr_seed_valid(void *h, int s) { return m(h)->core.seed_valid(s); }
int eg2_lcmgr_seed_word_of(void *h, int s, int i) { return int(m(h)->core.seed_word_of(s, i)); }
int eg2_lcmgr_seed_err_lo(void *h) { return m(h)->core.seed_err_lo(); }
int eg2_lcmgr_seed_err_hi(void *h) { return m(h)->core.seed_err_hi(); }
int eg2_lcmgr_intg_err(void *h) { return m(h)->core.intg_err(); }
int eg2_lcmgr_init_done_allowed(void *h) { return m(h)->core.init_done_allowed(); }
int eg2_lcmgr_rma_ever(void *h) { return m(h)->core.rma_ever(); }
int eg2_lcmgr_rma_failed(void *h) { return m(h)->core.rma_failed(); }
int eg2_lcmgr_disabled(void *h) { return m(h)->core.disabled(); }
int eg2_lcmgr_rma_requested(void *h) { return m(h)->core.rma_requested(); }

}  // extern "C"
