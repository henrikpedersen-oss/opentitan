// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The alert handler's reference model for the alert_handler block bench (alert_handler_cmodel_sb):
// the core of ip/eg2_alert_core.h behind a C interface for SystemVerilog DPI
// (eg2_alert_cmodel_pkg.sv). From the alert handler's documents (hw/top_earlgrey/ip_autogen/
// alert_handler: doc/theory_of_operation.md, doc/programmers_guide.md, data/alert_handler.hjson),
// never from its RTL.
//
// The register file is this binding's, with the access rules of alert_handler.hjson as reggen
// generates them (the eg2_regs table of eg2_model_regs.h): REGWEN-gated writes dropped, rw1c,
// rw0c, write-only fields read 0, INTR_TEST setting INTR_STATE, the shadowed registers' two-write
// protocol (the core's before_write). Time is the bench's alert-handler clock cycle (eg2_alert_time).
//
// Inputs, all from the block's ports as the bench's agents observe them, never a signal inside the
// design:
//   - an alert received (the alert agent's completed alert handshake): eg2_alert_alert;
//   - the outcome of a ping the design chose (its LFSR timer is not predicted, theory_of_operation
//     "LFSR Timer" leaves each randomized value free): a ping that timed out at the sender or the
//     escalation receiver, and signal integrity failures, as the local alerts the documents name
//     (alert_handler.hjson local alert IDs): eg2_alert_local;
//   - the low-power groups' indications (lpg_cg_en / lpg_rst_en, OR'ed: "Low-power Management of
//     Alert Channels"): eg2_alert_lpg, with each alert's group from the top's LpgMap
//     (eg2_alert_lpg_map; configuration, top_earlgrey alert_lpgs).
// The ping timer itself is not run (Eg2AlertPort::ping_modelled false).
//
// A read returns an expectation (exp), the bits to compare (mask) and, for the escalation counters,
// a tolerance in cycles (tol): the documents give no cycle latency for the alert handshake or the
// start of Phase0, and the bench sees an alert a few cycles after the design does. While the design
// is not settled (quiet 0: an alert, a local alert or a configuration write within the bench's
// settle window), the fields hardware updates are not compared (mask leaves them out).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../eg2_model_regs.h"
#include "../ip/eg2_alert_core.h"

namespace {

const eg2_regs::Block &block() {
  for (const eg2_regs::Block &b : eg2_regs::kBlocks) {
    if (std::string(b.ip) == "alert_handler") return b;
  }
  std::fprintf(stderr, "eg2_alert_dpi: no alert_handler block in eg2_model_regs.h\n");
  std::abort();
}

uint32_t field_mask(const eg2_regs::Field &f) {
  return (f.width >= 32 ? 0xffffffffu : ((1u << f.width) - 1)) << f.lsb;
}

class AlertDpi : public Eg2AlertPort {
 public:
  explicit AlertDpi(unsigned nalerts)
      : d_(block()), val_(d_.nregs, 0), core_(*this, nalerts), lpg_of_(nalerts, -1) {
    for (uint16_t i = 0; i < d_.nregs; ++i) by_off_[d_.regs[i].offset] = i;
    intr_state_ = find("INTR_STATE");
    intr_enable_ = find("INTR_ENABLE");
    intr_test_ = find("INTR_TEST");
    for (uint16_t r : core_.modelled_regs()) hw_reg_[r] = true;
    reset(0);
  }

  void reset(uint64_t cyc) {
    now_ = cyc;
    for (uint16_t i = 0; i < d_.nregs; ++i) val_[i] = d_.regs[i].resval;
    for (bool &e : esc_) e = false;
    lpg_lp_.assign(lpg_lp_.size(), false);
    due_ = kNever;
    core_.reset();
  }

  void time(uint64_t cyc) {
    if (cyc < now_) return;
    // Every scheduled step up to cyc, each at its own cycle (the core counts from now_cyc()).
    while (due_ != kNever && due_ <= cyc) {
      now_ = due_;
      due_ = kNever;
      core_.advance();
    }
    now_ = cyc;
  }

  void csr_write(uint32_t addr, uint32_t data, uint32_t be_mask) {
    auto it = by_off_.find(addr & ~3u);
    if (it == by_off_.end()) return;
    uint16_t r = it->second;
    const eg2_regs::Reg &rd = d_.regs[r];
    if (rd.regwen >= 0 && !(rd.flags & 2) &&
        (val_[uint16_t(rd.regwen)] & rd.regwen_mask) != rd.regwen_true) {
      return;  // locked (a shadowed register's lock is the core's before_write)
    }
    if (!core_.before_write(r, data, be_mask)) return;
    if (rd.flags & 2) {
      if (rd.regwen >= 0 && (val_[uint16_t(rd.regwen)] & rd.regwen_mask) != rd.regwen_true) return;
    }
    uint32_t v = val_[r];
    for (uint16_t k = 0; k < rd.nfields; ++k) {
      const eg2_regs::Field &f = d_.fields[rd.field0 + k];
      uint32_t fm = field_mask(f) & be_mask;
      switch (f.sw) {
        case eg2_regs::kAccRw:
        case eg2_regs::kAccWo: v = (v & ~fm) | (data & fm); break;
        case eg2_regs::kAccRw1c:
        case eg2_regs::kAccR0w1c: v &= ~(data & fm); break;
        case eg2_regs::kAccRw0c: v &= ~(~data & fm); break;
        case eg2_regs::kAccRw1s: v |= data & fm; break;
        default: break;
      }
    }
    val_[r] = v;
    if (int(r) == intr_test_ && intr_state_ >= 0) {
      val_[uint16_t(intr_state_)] |= data & be_mask & 0xf;  // INTR_TEST sets the interrupt state
      val_[r] = 0;
    }
    core_.after_write(r, data, be_mask);
  }

  // The expectation of a read of addr; quiet: the design has settled.
  void csr_read(uint32_t addr, bool quiet, uint32_t *exp, uint32_t *mask, uint32_t *tol) {
    *exp = 0;
    *mask = 0;
    *tol = 0;
    auto it = by_off_.find(addr & ~3u);
    if (it == by_off_.end()) return;
    uint16_t r = it->second;
    core_.before_read(r);
    const eg2_regs::Reg &rd = d_.regs[r];
    uint32_t v = 0, m = 0, hw = 0;
    for (uint16_t k = 0; k < rd.nfields; ++k) {
      const eg2_regs::Field &f = d_.fields[rd.field0 + k];
      uint32_t fm = field_mask(f);
      if (f.sw == eg2_regs::kAccWo || f.sw == eg2_regs::kAccNone) {
        m |= fm;  // reads 0
        continue;
      }
      v |= val_[r] & fm;
      m |= fm;
      if (f.hw_writes || hw_reg_.count(r) || int(r) == intr_state_) hw |= fm;
    }
    if (!quiet) m &= ~hw;
    // A class's state and escalation counter right at a state change of the model: not compared
    // (the documents fix no cycle latency for the trigger, Phase0, or a phase's end).
    if (std::strncmp(rd.name, "CLASS", 5) == 0 &&
        (std::strstr(rd.name, "_STATE") || std::strstr(rd.name, "_ESC_CNT")) &&
        near_transition(unsigned(rd.name[5] - 'A'))) {
      return;  // exp, mask and tol stay 0: nothing compared
    }
    // The escalation counters: a cycle count, compared with a tolerance by the bench.
    if (std::strstr(rd.name, "_ESC_CNT")) *tol = tol_;
    // Shadowed registers read back the committed value; a read restarts the staging (the core).
    *exp = v;
    *mask = m;
  }

  uint32_t csr_value(uint32_t addr) const {
    auto it = by_off_.find(addr & ~3u);
    return it == by_off_.end() ? 0 : val_[it->second];
  }

  void alert(uint16_t id) { core_.alert(id); }
  void local_alert(unsigned i) { core_.local_alert(i); }
  void lpg(unsigned g, bool low_power) {
    if (g < lpg_lp_.size()) lpg_lp_[g] = low_power;
  }
  void lpg_map(unsigned alert, int g) {
    if (alert < lpg_of_.size()) lpg_of_[alert] = g;
  }
  bool esc(unsigned e) const { return e < 4 && esc_[e]; }
  uint32_t intr_state() const { return intr_state_ < 0 ? 0 : val_[uint16_t(intr_state_)] & 0xf; }
  uint32_t intr_enable() const {
    return intr_enable_ < 0 ? 0 : val_[uint16_t(intr_enable_)] & 0xf;
  }
  uint32_t class_state(unsigned k) const { return core_.class_state(k); }
  // Class k changed state within tol_ cycles, or will within tol_ cycles.
  bool near_transition(unsigned k) const {
    if (k >= Eg2AlertCore::kClasses) return false;
    uint64_t start = core_.class_start(k), dur = core_.class_duration(k);
    if (now_ - start < tol_) return true;
    return dur > 0 && start + dur >= now_ && start + dur - now_ < tol_;
  }
  bool near_any_transition() const {
    for (unsigned k = 0; k < Eg2AlertCore::kClasses; ++k) {
      if (near_transition(k)) return true;
    }
    return false;
  }
  std::vector<uint32_t> crashdump() { return core_.crashdump(); }
  bool dump_latched() const { return core_.dump_latched(); }
  const std::string &last_note() const { return note_; }
  void set_tol(uint32_t t) { tol_ = t; }
  void set_verbose(bool v) { verbose_ = v; }

  // ---- Eg2AlertPort ----
  const eg2_regs::Block &desc() const override { return d_; }
  uint32_t value(uint16_t r) const override { return val_[r]; }
  void set_value(uint16_t r, uint32_t v) override { val_[r] = v; }
  uint64_t now_cyc() const override { return now_; }
  void schedule(uint64_t cyc) override { due_ = cyc; }
  void set_intr(unsigned k) override {
    if (intr_state_ >= 0) val_[uint16_t(intr_state_)] |= 1u << k;
  }
  void set_escalation(unsigned e, bool level) override {
    if (e < 4) esc_[e] = level;
  }
  bool escalation(unsigned e) const override { return e < 4 && esc_[e]; }
  void note(const std::string &s) override {
    note_ = s;
    if (verbose_) std::printf("eg2_alert_dpi @%llu: %s\n", (unsigned long long)now_, s.c_str());
  }
  bool ping_modelled() const override { return false; }
  bool lpg_low_power(uint16_t id) const override {
    int g = id < lpg_of_.size() ? lpg_of_[id] : -1;
    return g >= 0 && unsigned(g) < lpg_lp_.size() && lpg_lp_[g];
  }

 private:
  int find(const char *name) const {
    for (uint16_t i = 0; i < d_.nregs; ++i) {
      if (std::strcmp(d_.regs[i].name, name) == 0) return i;
    }
    return -1;
  }

  const eg2_regs::Block &d_;
  std::vector<uint32_t> val_;
  Eg2AlertCore core_;
  std::vector<int> lpg_of_;
  std::vector<bool> lpg_lp_ = std::vector<bool>(64, false);
  std::map<uint32_t, uint16_t> by_off_;
  std::map<uint16_t, bool> hw_reg_;
  int intr_state_ = -1, intr_enable_ = -1, intr_test_ = -1;
  uint64_t now_ = 0, due_ = kNever;
  bool esc_[4] = {};
  std::string note_;
  uint32_t tol_ = 16;
  bool verbose_ = false;
};

AlertDpi *H(void *h) { return static_cast<AlertDpi *>(h); }

}  // namespace

extern "C" {

void *eg2_alert_create(unsigned int nalerts) { return new AlertDpi(nalerts); }
void eg2_alert_release(void *h) { delete H(h); }
void eg2_alert_reset(void *h, unsigned long long cyc) { H(h)->reset(cyc); }
void eg2_alert_time(void *h, unsigned long long cyc) { H(h)->time(cyc); }
void eg2_alert_csr_write(void *h, unsigned int addr, unsigned int data, unsigned int be_mask) {
  H(h)->csr_write(addr, data, be_mask);
}
void eg2_alert_csr_read(void *h, unsigned int addr, int quiet, unsigned int *exp,
                        unsigned int *mask, unsigned int *tol) {
  uint32_t e, m, t;
  H(h)->csr_read(addr, quiet != 0, &e, &m, &t);
  *exp = e;
  *mask = m;
  *tol = t;
}
unsigned int eg2_alert_csr_value(void *h, unsigned int addr) { return H(h)->csr_value(addr); }
void eg2_alert_alert(void *h, unsigned int id) { H(h)->alert(uint16_t(id)); }
void eg2_alert_local(void *h, unsigned int idx) { H(h)->local_alert(idx); }
void eg2_alert_lpg(void *h, unsigned int g, int low_power) { H(h)->lpg(g, low_power != 0); }
void eg2_alert_lpg_map(void *h, unsigned int alert, int g) { H(h)->lpg_map(alert, g); }
int eg2_alert_esc(void *h, unsigned int e) { return H(h)->esc(e); }
unsigned int eg2_alert_intr_state(void *h) { return H(h)->intr_state(); }
unsigned int eg2_alert_intr_enable(void *h) { return H(h)->intr_enable(); }
unsigned int eg2_alert_class_state(void *h, unsigned int k) { return H(h)->class_state(k); }
unsigned int eg2_alert_dump_words(void *h) { return unsigned(H(h)->crashdump().size()); }
unsigned int eg2_alert_dump_word(void *h, unsigned int i) {
  std::vector<uint32_t> w = H(h)->crashdump();
  return i < w.size() ? w[i] : 0;
}
int eg2_alert_dump_latched(void *h) { return H(h)->dump_latched(); }
int eg2_alert_near_transition(void *h) { return H(h)->near_any_transition(); }
void eg2_alert_set_tol(void *h, unsigned int t) { H(h)->set_tol(t); }
void eg2_alert_verbose(void *h, int v) { H(h)->set_verbose(v != 0); }
const char *eg2_alert_last_note(void *h) { return H(h)->last_note().c_str(); }

}  // extern "C"
