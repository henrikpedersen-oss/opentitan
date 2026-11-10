// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The reset manager's reference model for the rstmgr block bench (rstmgr_cmodel_sb): the core of
// eg2_rstmgr_core.h behind a C interface for SystemVerilog DPI (eg2_rstmgr_cmodel_pkg.sv).
//
// From rstmgr's documents (hw/top_earlgrey/ip_autogen/rstmgr: data/rstmgr.hjson,
// doc/theory_of_operation.md), never from its RTL; the top's leaf resets and power domains from
// top_earlgrey.gen.hjson (domain 0 always-on, 1 main).
//
// The register file is this binding's, with the access rules of rstmgr.hjson: ALERT_TEST write-only;
// RESET_REQ rw (MuBi4, resval false); RESET_INFO rw1c (resval POR); ALERT_REGWEN, CPU_REGWEN and
// SW_RST_REGWEN_<i> rw0c gating ALERT_INFO_CTRL, CPU_INFO_CTRL and SW_RST_CTRL_N_<i>; the read-only
// *_INFO_ATTR, *_INFO and ERR_CODE.
//
// Two register domains (rstmgr.hjson): RESET_INFO, ALERT_INFO_CTRL / _ATTR / ALERT_INFO and the CPU_*
// equivalents carry `sync: clk_por_i`: they are reset by the POR reset (rst_por_ni, the por_io_div4
// leaf in top_earlgrey.gen.hjson's reset_connections). Every other register is in clk_i's domain and
// reset by rst_ni, which the top connects to the lc_io_div4 leaf: it is reset by every life-cycle
// reset, not only a power-on reset.
//
// The inputs are the block's ports as levels: rst_por_ni (or por_n_i) low, a power-on reset;
// rst_ni low, the clk_i domain's reset; pwrmgr's reset requests (rst_lc_req and rst_sys_req per power domain), its reset
// category (reset_cause: none, low-power entry, hardware request) and rstreqs (b0 sysrst_ctrl, b1
// aon_timer, b2 main power glitch, b3 escalation, b4 rv_dm NDM), scan reset (scanmode with
// scan_rst_ni low), and the alert and CPU crash dumps as 32-bit words.
//
// The rules (theory_of_operation.md "Reset Information", "Crash Dump Information", "Software
// Controlled Resets", the reset tree sections):
// - A reset begins when pwrmgr requests one (any rst_lc_req or rst_sys_req bit), it ends when all
//   requests are released. The processor's reset is the main domain's rst_lc tree: its release after
//   a power-on reset allows recording ("active recording is silenced until the first processor reset
//   release").
// - While a reset with category hardware request is active, rstmgr records rstreqs (HW_REQ) and, for
//   a reset that began with RESET_REQ true, SW_RESET; with category low-power entry,
//   LOW_POWER_EXIT. Bits are only added.
// - A reset with category hardware request captures the enabled crash dumps and clears both EN bits;
//   RESET_REQ is cleared when the reset completes.
// - Leaf resets, when settled: everything in reset during a power-on reset; rst_lc_n[d] and
//   rst_sys_n[d] follow pwrmgr's requests for domain d (reset trees "derived from rst_por_n"; "When
//   Domain B is powered off, all of Domain B's resets ... are asserted"); a software-controllable
//   leaf (rst_<module>_n, "derived from rst_lc_n"; top_earlgrey.gen.hjson gives every one parent
//   lc_src and domain Main) is in reset while the main rst_lc tree is, or while SW_RST_CTRL_N_<i> is
//   0 ("When a particular bit value is 0, the corresponding module is held in reset"). rst_sys_n
//   exists in the main domain only (top_earlgrey.gen.hjson). pwr_o.rst_lc_src_n / rst_sys_src_n
//   report the trees' state.
// - sw_rst_req_o is RESET_REQ ("a reset to power manager is requested").
//
// Bounds, not one value (the documents give no cycle timing and leave some outcomes open): a read
// returns `lo` (bits that must read 1) and `hi` (bits that may read 1). Not predicted:
// - while the design settles (the scoreboard's `quiet` is 0), the fields hardware updates;
// - RESET_INFO.HW_REQ b7 (NDM) during a hardware-request reset: "Non-debug-module resets are allowed
//   only when no resets have been triggered by pwrmgr", while the HW_REQ field lists rv_dm's request;
//   whether an NDM request seen under the hardware-request category is recorded is not written;
// - after a low-power entry reset: the EN bits and whether a dump was captured ("prior to a triggered
//   reset": whether a low-power entry counts is not written; EN "upon system reset");
// - *_INFO for an INDEX at or beyond CNT_AVAIL, and which word an INDEX selects: a read is checked
//   to be the dump's word INDEX counted from either end (eg2_rst_dump_word_ok);
// - after a scan reset, every field hardware updates, until the next power-on reset.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../ip/eg2_rstmgr_core.h"

namespace {

enum : uint32_t {
  kAlertTest = 0x00, kResetReq = 0x04, kResetInfo = 0x08, kAlertRegwen = 0x0c, kAlertCtrl = 0x10,
  kAlertAttr = 0x14, kAlertInfo = 0x18, kCpuRegwen = 0x1c, kCpuCtrl = 0x20, kCpuAttr = 0x24,
  kCpuInfo = 0x28, kSwRegwen0 = 0x2c, kSwCtrl0 = 0x54, kErrCode = 0x7c, kNumRegs = 0x80 / 4,
};
constexpr unsigned kNumSw = 10;  // NumSwResets
constexpr uint32_t kCtrlMask = 0xf1;  // EN, INDEX
constexpr uint32_t kNdmBit = 1u << (Eg2RstmgrCore::kHwReqLsb + 4);
enum Cause : uint32_t { kNone = 0, kLowPwrEntry = 1, kHwReq = 2, kUndefined = 3 };

bool break_test() {
  static int b = -1;
  if (b < 0) b = std::getenv("EG2_RST_TEST_BREAK") != nullptr;
  return b;
}

class Model : public Eg2RstmgrPort {
 public:
  Model() : core_(*this) { reset(); }

  // The clk_i domain's registers back to their reset values (rst_ni).
  void reset_local() {
    regs_[kResetReq / 4] = Eg2RstmgrCore::kMuBi4False;
    regs_[kAlertRegwen / 4] = 1;
    regs_[kCpuRegwen / 4] = 1;
    for (unsigned i = 0; i < kNumSw; ++i) {
      regs_[kSwRegwen0 / 4 + i] = 1;
      regs_[kSwCtrl0 / 4 + i] = 1;
    }
    regs_[kErrCode / 4] = 0;
  }

  void reset() {
    for (auto &r : regs_) r = 0;
    regs_[kResetInfo / 4] = 1;
    reset_local();
    core_.power_on_reset();
    in_reset_ = false;
    lc_ = sys_ = 0;
    rstreqs_ = 0;
    cause_ = kNone;
    por_ = true;  // released by the first inputs with por inactive
    scan_ = false;
    lp_open_ = false;
    ndm_open_ = false;
    sw_at_start_ = false;
    ++por_count_;
  }

  // Eg2RstmgrPort.
  uint32_t rd(Eg2RstReg r) override { return regs_[addr(r) / 4]; }
  void wr(Eg2RstReg r, uint32_t v) override { regs_[addr(r) / 4] = v; }

  void csr_write(uint32_t a, uint32_t d) {
    a &= 0x7c;
    uint32_t &r = regs_[a / 4];
    if (a == kResetReq) {
      r = d & 0xf;
    } else if (a == kResetInfo) {
      r &= ~(d & Eg2RstmgrCore::kInfoMask);
    } else if (a == kAlertRegwen || a == kCpuRegwen ||
               (a >= kSwRegwen0 && a < kSwRegwen0 + 4 * kNumSw)) {
      r &= d & 1;  // rw0c
    } else if (a == kAlertCtrl) {
      if (regs_[kAlertRegwen / 4] & 1) { r = d & kCtrlMask; ctrl_open_[0] = false; }
    } else if (a == kCpuCtrl) {
      if (regs_[kCpuRegwen / 4] & 1) { r = d & kCtrlMask; ctrl_open_[1] = false; }
    } else if (a >= kSwCtrl0 && a < kSwCtrl0 + 4 * kNumSw) {
      if (regs_[kSwRegwen0 / 4 + (a - kSwCtrl0) / 4] & 1) r = d & 1;
    }
    // ALERT_TEST and the read-only registers: no state.
  }

  void csr_read(uint32_t a, bool quiet, uint32_t *lo, uint32_t *hi) {
    a &= 0x7c;
    uint32_t v = a == kAlertTest ? 0 : regs_[a / 4];
    if (a == kAlertAttr) v = cnt(alert_words_);
    if (a == kCpuAttr) v = cnt(cpu_words_);
    *lo = *hi = v;
    if (a == kAlertInfo || a == kCpuInfo || a == kAlertTest) {
      // *_INFO: checked by eg2_rst_dump_word_ok; ALERT_TEST is write-only (what a read returns is
      // not documented).
      *lo = 0;
      *hi = 0xffffffffu;
      return;
    }
    bool open_hw = !quiet || scan_;
    if (a == kResetInfo) {
      if (open_hw) *hi = Eg2RstmgrCore::kInfoMask;  // bits are only added, never cleared by hw
      if (ndm_open_) *hi |= kNdmBit;
    } else if (a == kResetReq) {
      if (open_hw) { *lo = 0; *hi = 0xf; }
    } else if (a == kAlertCtrl || a == kCpuCtrl) {
      if (open_hw || lp_open_) { *lo &= ~1u; *hi |= 1u; }
      if (ctrl_open_[a == kCpuCtrl]) { *lo = 0; *hi = kCtrlMask; }
    }
    if (break_test() && *lo == *hi && a == kResetInfo) *lo = *hi = v ^ 1;
  }

  // The inputs, as levels.
  // `por_ast`: por_n_i low (the AST's power-ok, the documented POR); `por`: rst_por_ni low. After a
  // scan reset, rst_por_ni alone is not taken as a power-on reset (a scan reset asserts every reset
  // tree; it is not described as one): everything hardware updates stays open until por_n_i.
  void inputs(bool por_ast, bool por, bool local, uint32_t lc, uint32_t sys, uint32_t rstreqs,
              uint32_t cause, bool scan) {
    if (scan) scan_ = true;
    if (por_ast) {
      if (!por_ || scan_) reset();
      por_ = true;
      proc_in_reset_ = true;
      return;
    }
    if (por && scan_) return;
    if (por) {
      if (!por_) reset();
      por_ = true;
      proc_in_reset_ = true;
      return;
    }
    por_ = false;
    if (scan) scan_ = true;
    bool was = in_reset_;
    lc_ = lc & 3;
    sys_ = sys & 3;
    rstreqs_ = rstreqs & 0x1f;
    cause_ = cause & 3;
    in_reset_ = (lc_ | sys_) != 0;
    if (in_reset_ && !was) begin_reset();
    if (in_reset_) record();
    // rst_ni (the clk_i domain's reset) after the reset's start has been noted: a reset that falls
    // in the same sample as pwrmgr's request still began with RESET_REQ as software left it.
    if (local && !local_) reset_local();
    local_ = local;
    if (!in_reset_ && was) {
      core_.reset_complete();
      ++resets_;
    }
    // The processor's reset (the main domain's rst_lc tree) released: neither the power-on reset nor
    // pwrmgr's main-domain rst_lc request holds it any more.
    bool proc_was = proc_in_reset_;
    proc_in_reset_ = (lc_ & 2) != 0;
    if (proc_was && !proc_in_reset_) core_.processor_reset_released();
  }

  // The bench overwrote ALERT_INFO_CTRL / CPU_INFO_CTRL behind the model (OpenTitan's rstmgr
  // scoreboard writes them by backdoor after every reset): not predicted until the next write.
  void ctrl_unknown() { ctrl_open_[0] = ctrl_open_[1] = true; }

  void set_dump(bool alert, unsigned n, unsigned idx, uint32_t w) {
    auto &d = alert ? alert_words_ : cpu_words_;
    d.resize(n);
    if (idx < n) d[idx] = w;
  }

  // Is `v` the captured dump's word `idx` (from either end), or open?
  bool dump_word_ok(bool alert, uint32_t idx, uint32_t v, bool quiet) {
    if (!quiet || scan_ || lp_open_) return true;
    const auto &d = alert ? core_.alert_dump() : core_.cpu_dump();
    unsigned n = cnt(alert ? alert_words_ : cpu_words_);
    if (idx >= n) return true;  // not documented
    if (d.empty()) return v == 0;  // nothing captured since the power-on reset
    uint32_t a = Eg2RstmgrCore::word(d, idx), b = Eg2RstmgrCore::word(d, n - 1 - idx);
    bool ok = v == a || v == b;
    return break_test() ? !ok : ok;
  }

  // The outputs when settled, as bounds per bit. lc / sys: domain d's rst_lc_n / rst_sys_n leaf
  // (and pwr_o.rst_*_src_n); sw: bit i the main-domain leaf of SW_RST_CTRL_N_<i>; req: sw_rst_req_o
  // is kMultiBitBool4True.
  void outputs(uint32_t *lc_lo, uint32_t *lc_hi, uint32_t *sys_lo, uint32_t *sys_hi,
               uint32_t *sw_lo, uint32_t *sw_hi, uint32_t *req_lo, uint32_t *req_hi) {
    uint32_t lc_n = por_ ? 0 : (~lc_ & 3), sys_n = por_ ? 0 : (~sys_ & 3);
    *lc_lo = *lc_hi = lc_n;
    *sys_lo = *sys_hi = sys_n;
    uint32_t sw = 0;
    for (unsigned i = 0; i < kNumSw; ++i) {
      if (regs_[kSwCtrl0 / 4 + i] & 1) sw |= 1u << i;
    }
    if (!(lc_n & 2)) sw = 0;
    *sw_lo = *sw_hi = sw;
    uint32_t req = core_.sw_reset_requested() ? 1 : 0;
    *req_lo = *req_hi = req;
    if (scan_) {
      *lc_lo = *sys_lo = *sw_lo = *req_lo = 0;
      *lc_hi = *sys_hi = 3;
      *sw_hi = (1u << kNumSw) - 1;
      *req_hi = 1;
    }
  }

  uint32_t value(uint32_t a) { return regs_[(a & 0x7c) / 4]; }
  unsigned resets() const { return resets_; }
  unsigned pors() const { return por_count_; }
  bool recording() const { return core_.recording(); }

 private:
  static uint32_t addr(Eg2RstReg r) {
    switch (r) {
      case Eg2RstReg::kResetReq: return kResetReq;
      case Eg2RstReg::kResetInfo: return kResetInfo;
      case Eg2RstReg::kAlertCtrl: return kAlertCtrl;
      case Eg2RstReg::kCpuCtrl: return kCpuCtrl;
    }
    return kResetInfo;
  }
  static uint32_t cnt(const std::vector<uint32_t> &d) { return uint32_t(d.size()) & 0xf; }

  void begin_reset() {
    sw_at_start_ = core_.sw_reset_requested();
    ndm_open_ = false;
    if (cause_ == kHwReq) {
      core_.triggered_reset(alert_words_, cpu_words_);
      lp_open_ = false;
    } else if (cause_ == kLowPwrEntry) {
      lp_open_ = true;  // capture and EN after a low-power entry reset: not documented
    }
  }
  void record() {
    if (cause_ == kHwReq) {
      if (rstreqs_ & 0x10) ndm_open_ = core_.recording();
      core_.record(rstreqs_ & 0x0f, sw_at_start_, false);
    } else if (cause_ == kLowPwrEntry) {
      core_.record(0, false, true);
    } else if (cause_ == kNone && (rstreqs_ & 0x10)) {
      core_.record(0x10, false, false);  // an NDM reset: pwrmgr triggered no reset
    }
  }

  Eg2RstmgrCore core_;
  uint32_t regs_[kNumRegs];
  std::vector<uint32_t> alert_words_, cpu_words_;  // the dump inputs
  bool in_reset_ = false, por_ = true, local_ = true, scan_ = false, lp_open_ = false,
       ndm_open_ = false, proc_in_reset_ = true;
  bool ctrl_open_[2] = {false, false};
  bool sw_at_start_ = false;
  uint32_t lc_ = 0, sys_ = 0, rstreqs_ = 0, cause_ = 0;
  unsigned resets_ = 0, por_count_ = 0;
};

Model *m(void *h) { return static_cast<Model *>(h); }

}  // namespace

extern "C" {
void *eg2_rst_create() { return new Model(); }
void eg2_rst_release(void *h) { delete m(h); }
void eg2_rst_reset(void *h) { m(h)->reset(); }
void eg2_rst_csr_write(void *h, uint32_t addr, uint32_t data) { m(h)->csr_write(addr, data); }
void eg2_rst_csr_read(void *h, uint32_t addr, int quiet, uint32_t *lo, uint32_t *hi) {
  m(h)->csr_read(addr, quiet != 0, lo, hi);
}
void eg2_rst_inputs(void *h, int por_ast, int por, int local, uint32_t lc, uint32_t sys,
                    uint32_t rstreqs, uint32_t cause, int scan) {
  m(h)->inputs(por_ast != 0, por != 0, local != 0, lc, sys, rstreqs, cause, scan != 0);
}
void eg2_rst_set_dump(void *h, int alert, uint32_t n, uint32_t idx, uint32_t w) {
  m(h)->set_dump(alert != 0, n, idx, w);
}
int eg2_rst_dump_word_ok(void *h, int alert, uint32_t idx, uint32_t v, int quiet) {
  return m(h)->dump_word_ok(alert != 0, idx, v, quiet != 0);
}
void eg2_rst_outputs(void *h, uint32_t *lc_lo, uint32_t *lc_hi, uint32_t *sys_lo,
                     uint32_t *sys_hi, uint32_t *sw_lo, uint32_t *sw_hi, uint32_t *req_lo,
                     uint32_t *req_hi) {
  m(h)->outputs(lc_lo, lc_hi, sys_lo, sys_hi, sw_lo, sw_hi, req_lo, req_hi);
}
uint32_t eg2_rst_csr_value(void *h, uint32_t addr) { return m(h)->value(addr); }
void eg2_rst_ctrl_unknown(void *h) { m(h)->ctrl_unknown(); }
uint32_t eg2_rst_resets(void *h) { return m(h)->resets(); }
uint32_t eg2_rst_pors(void *h) { return m(h)->pors(); }
int eg2_rst_recording(void *h) { return m(h)->recording(); }
}
