// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl as a whole, as software and the hardware peers see it: the reference model of the
// top-level unit bench (hw/ip/rram_ctrl/dv/edagen/rram_ctrl, through dpi/eg2_rram_ctrl_top_dpi.cc)
// and the behaviour eg2sim's RRAM controller (Eg2RramCtrl, ip/eg2_mem.h) runs. It is composed of
// the unit cores, which the unit benches check against the RTL, plus what only the top level has:
//   eg2_rram_mp_core.h          memory protection of every controller and host access;
//   eg2_rram_ctrl_rdwr_core.h   the controller path: the arbiter and the read and write engines.
//                               It is a judge (it says whether an event is allowed); here it is
//                               driven as the producer: each step this model takes is reported to
//                               it, and the values software sees (the words of the read FIFO,
//                               ERR_CODE, ERR_ADDR) are the judge's own (its expected words, its
//                               error and first-error address). An objection of the judge to a
//                               step of this model is a composition error (findings());
//   eg2_rram_lcmgr_core.h       the life-cycle manager plug: INIT, the key requests, the seed
//                               reads, RMA, the disable. Driven the same way;
//   eg2_rram_scramble_core.h    the scrambler's key registers (keys_valid, the random keys after a
//                               disable). The array this model keeps is plaintext (see below), so
//                               the cipher itself is not used here.
// Not composed here: eg2_rram_otp_core.h (the OTP plug sits below otp_ctrl's macro interface; the
// owner of the OTP array is otp_ctrl's model) and eg2_rram_phy_core.h: the owner supplies the array
// (mem_rd / mem_wr with each access's attributes); eg2sim stores it physically through the PHY core
// and the macro model (ip/eg2_rram_macro.h), the top-level self-test logically.
//
// From the documentation, never from the RTL:
//   hw/ip/rram_ctrl/doc/programmers_guide.md "Initialization", "Memory-Mapped Reads via host_tl",
//     "Code Execution from RRAM", "Issuing a Controller Read / Write / Rewrite", "Write Alignment",
//     "Debugging Errors" (ERR_CODE, ERR_ADDR, multi-word reads, hardware-initiated operations),
//     "FIFO Management";
//   doc/theory_of_operation.md "LCMGR Hardware Plug" (Initialization stages, RMA Handling), "RRAM
//     Errors and Faults" (every ERR_CODE error triggers recov_err once the operation completes),
//     "RRAM Escalation & Disable" (global, local, software; latched until reset; the local
//     escalation from every STD_FAULT_STATUS / FAULT_STATUS bit, phy_relbl_err only with
//     DIS.LOCAL_ESC_RELBL_ERR; the keys switch to random keys);
//   doc/registers.md CONTROL, ADDR, OP_STATUS, STATUS, ERR_CODE (rw1c), ERR_ADDR ("set whenever a
//     software operation ends with MP_ERR, RD_ERR or WR_ERR", the address of the first word),
//     DIS (rw1s), EXEC, INIT (rw1s), FAULT_STATUS (seed_err bit 8, lcmgr_* bits 0-3);
//   README.md "Address Map" (2 MiB data partition, OTP pages 4090-4095, 8 info pages).
//
// What the documents leave open, and what this model takes (each a finding in
// status/rtl_tickets.md or status/dv_tickets.md):
//   - a software operation while RRAM is disabled: "all host and controller requests are blocked",
//     nothing says whether or how the operation completes. The model completes it at once with
//     OP_STATUS.err and ERR_CODE.op_err (eg2sim's choice since 2026-10; the rdwr core does not
//     predict it, the top-level bench does not check it);
//   - the word a read of the empty read FIFO returns: the model returns 0 (not checked by the
//     bench);
//   - a word software pushes to wr_fifo with no write operation running: dropped (the rdwr core
//     records it as a stray word that must never reach the RRAM);
//   - the RMA wipe data (an LFSR seeded with rma_seed, polynomial not documented): the owner's;
//   - the timing: the model has none. An operation completes when its last word is available
//     (a read: at once; a write: when the last word is pushed). The bench accepts the hardware's
//     later completion (OP_STATUS.done 0 while the operation runs).

#ifndef EG2_RRAM_CTRL_TOP_CORE_H_
#define EG2_RRAM_CTRL_TOP_CORE_H_

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "eg2_rram_ctrl_rdwr_core.h"
#include "eg2_rram_lcmgr_core.h"
#include "eg2_rram_mp_core.h"
#include "eg2_rram_scramble_core.h"

namespace eg2_model {

class RramCtrlTop {
 public:
  static constexpr uint32_t kPageBytes = 512, kDataPages = 4096, kOtpFirstPage = 4090;
  static constexpr uint32_t kInfoPages = 8, kWordsPerPage = kPageBytes / 4;
  static constexpr uint32_t kDataBytes = kDataPages * kPageBytes;
  static constexpr uint32_t kMuBi4True = 0x6, kMuBi4False = 0x9, kLcOn = 0x5, kLcOff = 0xa;
  static constexpr uint32_t kOpErr = 1, kMpErr = 2, kRdErr = 4, kWrErr = 8;
  static constexpr uint32_t kExecMagic = 0xa26a38f7u;
  static constexpr unsigned kCreatorPage = 5, kOwnerPage = 6, kIsolatedPage = 7;
  // FAULT_STATUS bits this level sets (registers.md FAULT_STATUS).
  static constexpr unsigned kFaultLcmgrMp = 1, kFaultLcmgrRd = 2, kFaultSeed = 8, kFaultRelbl = 9;
  // The scrambler's reset keys (RndCnstAddrKey / RndCnstDataKey: netlist constants; the owner
  // sets them if it knows them).
  using Key = std::array<uint32_t, 4>;

  // ---- The array, supplied by the owner (byte addresses, 4-aligned, within the partition) ----
  // Each access carries the attributes memory protection gave it (scrambling, ECC, address XOR:
  // theory_of_operation.md "Access attributes"), which a physical owner (eg2sim: the PHY core and
  // the macro model) stores and decodes with. A read answers with the macro's ECC outcome:
  // kRdUncorr is an RRAM read error (ERR_CODE.rd_err); kRdUnknown: the word's contents are not
  // known (a bench: never written in this run), delivered unpredicted, never as an error.
  struct Phys {
    bool scr = false, ecc = true, xr = true;
  };
  enum RdSt { kRdOk = 0, kRdCorr1, kRdCorr2, kRdUncorr, kRdUnknown };
  std::function<RdSt(unsigned part, uint32_t byte, uint32_t *w, const Phys &p)> mem_rd;
  // last: the last word of the operation (rram_phy commits the page then).
  std::function<void(unsigned part, uint32_t byte, uint32_t w, const Phys &p, bool last)> mem_wr;
  // The end of a write, Rewrite or wipe page (an operation that stopped at an error included).
  std::function<void(unsigned part)> mem_end;

  // ---- Configuration and inputs the owner keeps current ----
  RramMp mp;                    // the MP registers and the life-cycle enables
  uint32_t dis = 0x99;          // DIS (SW_DIS 3:0, LOCAL_ESC_RELBL_ERR 7:4)
  bool lc_escalate = false;     // lc_escalate_en On
  uint32_t std_fault = 0, fault = 0;  // STD_FAULT_STATUS, FAULT_STATUS (the owner's bits too)
  uint32_t exec = 0;            // EXEC
  Key addr_key{}, data_key{}, rand_addr_key{}, rand_data_key{};  // what otp_ctrl answers

  // ---- Software-visible state ----
  uint32_t err_code = 0;        // ERR_CODE
  uint32_t err_addr = 0;        // ERR_ADDR (byte address)
  bool op_done = false, op_err = false;  // OP_STATUS
  bool regwen = true;           // CTRL_REGWEN
  std::deque<uint32_t> rd_fifo;  // the words of the read FIFO not yet read
  std::deque<bool> rd_known;     // per word: its value is predicted

  // What happened at a step, for the owner (interrupts, alerts).
  struct Ev {
    bool done = false;     // an operation completed: OP_STATUS.done, the op_done interrupt
    bool recov = false;    // the recov_err alert (the operation ended with an error)
    bool fatal = false;    // FAULT_STATUS changed: the fatal_err alert
    uint32_t err = 0;      // the completed operation's ERR_CODE bits
  };

  RramCtrlTop() : scr_(0, 0) {}

  void reset() {
    RramCtrlTop fresh;
    fresh.mem_rd = mem_rd;
    fresh.mem_wr = mem_wr;
    fresh.mem_end = mem_end;
    fresh.scr_ = RramScramble(rst_addr_key_, rst_data_key_);
    fresh.rst_addr_key_ = rst_addr_key_;
    fresh.rst_data_key_ = rst_data_key_;
    *this = fresh;
  }
  void set_reset_keys(rram_u128 a, rram_u128 d) {
    rst_addr_key_ = a;
    rst_data_key_ = d;
    scr_ = RramScramble(a, d);
  }

  // "RRAM Escalation & Disable": latched until reset.
  bool disabled() {
    uint32_t f = fault;
    if (((dis >> 4) & 0xf) == kMuBi4False) f &= ~(1u << kFaultRelbl);
    if ((dis & 0xf) != kMuBi4False || lc_escalate || std_fault != 0 || f != 0) disabled_ = true;
    if (disabled_) {
      lc_.rram_disable(kMuBi4True);
      step_keys();
    }
    return disabled_;
  }
  uint32_t disable_mubi() { return disabled() ? kMuBi4True : kMuBi4False; }

  // ---- Initialization (INIT.VAL, rw1s: only the first write starts it) ----
  // lc_seed_hw_rd_en: the life-cycle signal as lc_ctrl drives it (On or not).
  Ev init(uint32_t lc_seed_hw_rd_en) {
    Ev ev;
    if (lc_.init_seen()) return ev;
    disabled();
    lc_.lc_seed_hw_rd_en(lc_seed_hw_rd_en);
    lc_.init();
    // StReqAddrKey, StReqDataKey: otp_ctrl answers each request.
    if (lc_.key_req(0).empty()) lc_.key_ack(0, addr_key, rand_addr_key);
    if (lc_.key_req(1).empty()) lc_.key_ack(1, data_key, rand_data_key);
    step_keys();
    // StReadSeeds: each seed page read twice, creator first.
    if (lc_.state() == Eg2RramLcmgrCore::kSeeds) {
      for (int s = 0; s < 2; ++s)
        for (int pass = 0; pass < 2; ++pass) seed_pass(s);
    }
    if (lc_.seed_err_lo() && !((fault >> kFaultSeed) & 1)) {
      fault |= 1u << kFaultSeed;  // FAULT_STATUS.seed_err, part of the local escalation
      ev.fatal = true;
    }
    disabled();
    return ev;
  }
  bool init_done() const { return lc_.init_done_allowed(); }
  bool keys_valid() const { return lc_.keys_acked(); }
  // The seed keymgr gets (256 bits as 8 words) and whether it is valid.
  bool seed(int s, std::vector<uint8_t> *bytes) {
    disabled();
    bytes->assign(32, 0);
    for (int i = 0; i < 8; ++i)
      for (int b = 0; b < 4; ++b) (*bytes)[4 * i + b] = uint8_t(lc_.seed_word_of(s, i) >> (8 * b));
    return lc_.seed_valid(s);
  }
  // rma_sw_dis: from the start of the RMA entry on ("RMA Handling").
  bool rma_sw_dis() const { return lc_.rma_ever(); }

  // ---- RMA (lc_ctrl's rma_req On): the wipe; true when rma_ack would be On ----
  // byte(): the wipe data, one byte at a time.
  bool rma(const std::function<uint8_t()> &byte) {
    lc_.rma_req(kLcOn);
    disabled();
    if (!lc_.rma_started().empty()) return false;  // refused: disabled, seeds being read, done
    uint32_t dm = disable_mubi();
    // The wipe's attributes are not documented (eg2_rram_mp_core.h: lcmgr data rules): the model
    // writes and reads back with ECC and address XOR, unscrambled.
    const Phys ph{false, true, true};
    auto wipe_page = [&](unsigned part, uint32_t page) {
      RramMp::Ctrl cw = mp.ctrl(RramMp::kSelHwLcMgr, RramMp::kPhaseRma, part, page, true, false, dm);
      RramMp::Ctrl cr = mp.ctrl(RramMp::kSelHwLcMgr, RramMp::kPhaseRma, part, page, false, false, dm);
      if (cw.err != kRramNo || cr.err != kRramNo) {
        lc_.rma_error();
        return;
      }
      uint32_t written[kWordsPerPage];
      for (uint32_t i = 0; i < kWordsPerPage; ++i) {
        uint32_t a = page * kPageBytes + 4 * i, w = 0;
        for (int b = 0; b < 4; ++b) w |= uint32_t(byte()) << (8 * b);
        written[i] = w;
        if (mem_wr) mem_wr(part, a, w, ph, i + 1 == kWordsPerPage);
        lc_.rma_write(part, a / 4);
      }
      if (mem_end) mem_end(part);
      for (uint32_t i = 0; i < kWordsPerPage; ++i) {
        uint32_t a = page * kPageBytes + 4 * i, back = 0;
        RdSt st = mem_rd ? mem_rd(part, a, &back, ph) : kRdUnknown;
        if (st == kRdUncorr) lc_.rma_error();
        lc_.rma_read(part, a / 4, st == kRdUnknown || back == written[i]);
      }
    };
    for (uint32_t p : {kCreatorPage, kOwnerPage, kIsolatedPage}) wipe_page(1, p);
    for (uint32_t p = 0; p < kOtpFirstPage; ++p) wipe_page(0, p);
    if (lc_.rma_failed()) {
      lc_.rma_fail_seen();
      return false;
    }
    return lc_.rma_ack_seen().empty();
  }

  // ---- Software operations ----
  // CONTROL written with START while CTRL_REGWEN is 1: control is the value written, addr ADDR.
  Ev start(uint32_t control, uint32_t addr) {
    unsigned op = (control >> 4) & 3, part = (control >> 8) & 1, num = (control >> 16) & 0x3ff;
    bool dis_now = disabled();
    wr_on_ = false;
    rdwr_.cycle(true, init_done(), dis_now, rma_sw_dis());
    if (dis_now) {  // blocked: completed at once with op_err (the documentation is silent)
      rdwr_.rq[RramCtrlRdWr::kSw] = RramCtrlRdWr::Request();
      return complete(kOpErr, 0);
    }
    note(rdwr_.request(RramCtrlRdWr::kSw, op, part, num, addr, RramMp::kPhaseNone));
    const RramCtrlRdWr::Request &r = rdwr_.rq[RramCtrlRdWr::kSw];
    if (r.static_bad || r.seen_closed) {
      note(rdwr_.done(RramCtrlRdWr::kSw, kOpErr));
      return complete(kOpErr, 0);
    }
    part_ = part;
    base_ = addr >> 2;
    n_ = op == RramCtrlRdWr::kRewrite ? 4 : num + 1;
    if (op == RramCtrlRdWr::kRead) return read_op();
    if (op == RramCtrlRdWr::kRewrite) return rewrite_op();
    // A write: "START clears the write FIFO"; the words come from wr_fifo, CTRL_REGWEN low until
    // the operation is done.
    wr_on_ = true;
    wr_i_ = 0;
    regwen = false;
    return Ev();
  }

  // A word software pushed to wr_fifo.
  Ev push(uint32_t w) {
    uint64_t d = RramCtrlRdWr::with_intg(w);
    note_push(d);
    if (!wr_on_) return Ev();  // no write running: never reaches the RRAM
    const RramCtrlRdWr::Cur &c = rdwr_.cur;
    if (!(c.on && c.err_seen)) {
      uint32_t wa;
      bool ovfl;
      word_addr(wr_i_, &wa, &ovfl);
      note(rdwr_.wr_req(wa, ovfl, d, wr_i_ + 1 == n_, RramCtrlRdWr::kSelSw, part_, RramMp::kPhaseNone));
      Phys ph;
      bool mperr = denied(RramMp::kSelSw, wa, ovfl, true, &ph);
      if (!mperr && mem_wr) mem_wr(part_, wa * 4, w, ph, wr_i_ + 1 == n_);
      note(rdwr_.wr_rsp(mperr, false));
    }
    if (++wr_i_ < n_) return Ev();
    wr_on_ = false;
    if (mem_end) mem_end(part_);
    return finish_cur();
  }

  // A read of rd_fifo: the oldest word (0 when empty, not predicted).
  bool pop(uint32_t *w, bool *known = nullptr) {
    if (rd_fifo.empty()) {
      *w = 0;
      if (known) *known = false;
      return false;
    }
    *w = rd_fifo.front();
    if (known) *known = rd_known.front();
    rd_fifo.pop_front();
    rd_known.pop_front();
    return true;
  }
  // FIFO_CLR: the read FIFO's words go; the write FIFO holds no word in this model (each pushed
  // word is programmed at once), so clearing it changes nothing and the operation keeps waiting.
  void fifo_clr(bool wr, bool rd) {
    (void)wr;
    if (rd) {
      rd_fifo.clear();
      rd_known.clear();
    }
  }
  bool write_running() const { return wr_on_; }

  // ---- Host reads and fetches (host_tl) ----
  // A host_tl read of byte offset off of the data partition: answered with data (true) or with
  // an error. The OTP pages and the information partition are never reachable ("Memory-Mapped
  // Reads via host_tl"), the memory protection of the data partition applies, rma_sw_dis and the
  // disable answer with an error.
  bool host_read_ok(uint32_t off) {
    if (off >= kDataBytes) return false;
    RramMp::Host h = mp.host(off / kPageBytes, rma_sw_dis() ? kMuBi4True : kMuBi4False, disable_mubi());
    return h.err == kRramNo && h.fwd == kRramYes;
  }
  // Instruction fetches: EXEC must hold the magic value and RRAM must not be disabled.
  bool exec_enabled() { return exec == kExecMagic && !disabled(); }

  // ---- For reports and the bench ----
  const std::vector<std::string> &findings() const { return findings_; }
  void clear_findings() { findings_.clear(); }
  const RramCtrlRdWr &rdwr() const { return rdwr_; }
  const Eg2RramLcmgrCore &lcmgr() const { return lc_; }
  const RramScramble &scrambler() const { return scr_; }

 private:
  void note(const std::string &e) {
    if (!e.empty()) findings_.push_back(e);
  }
  void note_push(uint64_t d) { rdwr_.push(RramCtrlRdWr::kSw, d); }

  void word_addr(uint32_t i, uint32_t *wa, bool *ovfl) const {
    uint64_t a = uint64_t(base_ & ((1u << RramCtrlRdWr::kBusAddrW) - 1)) + i;
    *wa = uint32_t(a & ((1u << RramCtrlRdWr::kBusAddrW) - 1));
    *ovfl = (a >> RramCtrlRdWr::kBusAddrW) != 0;
  }
  // Memory protection of one controller access (eg2sim needs an answer: an undecided case denies).
  // An attribute the documentation leaves open (kRramUnpred) is taken as off, as eg2sim denies an
  // open permission; the address XOR as on (software regions and info pages have it).
  static Phys phys_of(RramTri scr, RramTri ecc, RramTri xr) {
    return Phys{scr == kRramYes, ecc == kRramYes, xr != kRramNo};
  }
  bool denied(unsigned sel, uint32_t wa, bool ovfl, bool write, Phys *ph) {
    RramMp::Ctrl c = mp.ctrl(sel, RramMp::kPhaseInvalid, part_, (wa * 4) / kPageBytes, write, ovfl,
                             disable_mubi());
    *ph = phys_of(c.scr, c.ecc, c.xr);
    return !(c.err == kRramNo && c.fwd == kRramYes);
  }
  // One RRAM read of the current operation: the PHY's answer (the word still XORed with its
  // bus-word address, the integrity over the clean word) handed to the controller path.
  void rram_read(unsigned sel, unsigned mp_sel, uint32_t i, uint32_t phase) {
    uint32_t wa;
    bool ovfl;
    word_addr(i, &wa, &ovfl);
    note(rdwr_.rd_req(wa, ovfl, sel, part_, phase));
    Phys ph;
    bool mperr = denied(mp_sel, wa, ovfl, false, &ph);
    uint32_t w = 0;
    bool rderr = false, known = true;
    if (!mperr) {
      RdSt st = mem_rd ? mem_rd(part_, wa * 4, &w, ph) : kRdUnknown;
      rderr = st == kRdUncorr;
      known = st != kRdUnknown;
      if (rderr) w = 0;
    }
    uint64_t d = (uint64_t(RramCtrlRdWr::intg(w)) << 32) | (w ^ wa);
    note(rdwr_.rd_rsp(d, rderr, mperr));
    if (!known && !mperr && !rderr) unknown_.push_back(rdwr_.cur.rd_answered - 1);
  }
  void drain(int who) {
    RramCtrlRdWr::Cur &c = rdwr_.cur;
    while (!c.expect.empty()) {
      uint32_t idx = c.delivered;
      uint64_t d = c.expect.front();
      note(rdwr_.deliver(who, d, true));
      bool known = true;
      for (uint32_t u : unknown_) known = known && u != idx;
      if (who == RramCtrlRdWr::kSw) {
        rd_fifo.push_back(uint32_t(d));
        rd_known.push_back(known);
      } else {
        seed_buf_.push_back({uint32_t(d), known});
      }
    }
  }
  Ev read_op() {
    unknown_.clear();
    for (uint32_t i = 0; i < n_; ++i) {
      if (rdwr_.cur.on && rdwr_.cur.err_seen) break;  // "stops issuing new RRAM read requests"
      rram_read(RramCtrlRdWr::kSelSw, RramMp::kSelSw, i, RramMp::kPhaseNone);
      drain(RramCtrlRdWr::kSw);
    }
    drain(RramCtrlRdWr::kSw);
    return finish_cur();
  }
  // Rewrite: the RRAM word read (four bus words) and written back unchanged, never shown to
  // software. Checked against the software rules (eg2_rram_mp_core.h: the loopback requester's
  // own rules are not documented).
  Ev rewrite_op() {
    unknown_.clear();
    for (uint32_t i = 0; i < 4; ++i) {
      if (rdwr_.cur.on && rdwr_.cur.err_seen) break;
      rram_read(RramCtrlRdWr::kSelLoop, RramMp::kSelSw, i, RramMp::kPhaseNone);
    }
    for (uint32_t i = 0; i < 4 && !(rdwr_.cur.on && rdwr_.cur.err_seen); ++i) {
      uint32_t wa;
      bool ovfl;
      word_addr(i, &wa, &ovfl);
      uint64_t d = rdwr_.cur.rw_data[i];
      note(rdwr_.wr_req(wa, ovfl, d, i == 3, RramCtrlRdWr::kSelLoop, part_, RramMp::kPhaseNone));
      Phys ph;
      bool mperr = denied(RramMp::kSelSw, wa, ovfl, true, &ph);
      bool known = true;
      for (uint32_t u : unknown_) known = known && u != i;
      if (!mperr && known && mem_wr) mem_wr(part_, wa * 4, uint32_t(d), ph, i == 3);
      note(rdwr_.wr_rsp(mperr, false));
    }
    if (mem_end) mem_end(part_);
    return finish_cur();
  }
  Ev finish_cur() {
    uint32_t err = rdwr_.cur.err, ea = rdwr_.cur.err_addr;
    note(rdwr_.done(RramCtrlRdWr::kSw, err, ea));
    return complete(err, ea);
  }
  Ev complete(uint32_t err, uint32_t bus_word) {
    Ev ev;
    err_code |= err;
    if (err & (kMpErr | kRdErr | kWrErr)) err_addr = bus_word << 2;
    op_done = true;
    if (err) op_err = true;
    regwen = true;
    ev.done = true;
    ev.recov = err != 0;
    ev.err = err;
    return ev;
  }

  // One pass of a seed read (8 bus words of the creator or owner info page, PhaseSeed), through
  // the controller path as the lcmgr plug's request.
  void seed_pass(int s) {
    uint32_t page = s ? kOwnerPage : kCreatorPage;
    note(lc_.request(Eg2RramLcmgrCore::kOpRead, 1, page * kPageBytes, 8, Eg2RramLcmgrCore::kPhaseSeed));
    rdwr_.cycle(true, init_done(), disabled(), rma_sw_dis());
    note(rdwr_.request(RramCtrlRdWr::kLc, RramCtrlRdWr::kRead, 1, 7, page * kPageBytes,
                       RramMp::kPhaseSeed));
    part_ = 1;
    base_ = page * kWordsPerPage;
    n_ = 8;
    unknown_.clear();
    seed_buf_.clear();
    uint32_t dm = disable_mubi();
    for (uint32_t i = 0; i < 8; ++i) {
      if (rdwr_.cur.on && rdwr_.cur.err_seen) break;
      uint32_t wa = base_ + i;
      note(rdwr_.rd_req(wa, false, RramCtrlRdWr::kSelLc, 1, RramMp::kPhaseSeed));
      RramMp::Ctrl c = mp.ctrl(RramMp::kSelHwLcMgr, RramMp::kPhaseSeed, 1, page, false, false, dm);
      bool mperr = !(c.err == kRramNo && c.fwd == kRramYes);
      uint32_t w = 0;
      bool rderr = false;
      if (!mperr) {
        RdSt st = mem_rd ? mem_rd(1, wa * 4, &w, phys_of(c.scr, c.ecc, c.xr)) : kRdUnknown;
        rderr = st == kRdUncorr;
        if (rderr || st == kRdUnknown) w = 0;
      }
      note(rdwr_.rd_rsp((uint64_t(RramCtrlRdWr::intg(w)) << 32) | (w ^ wa), rderr, mperr));
      drain(RramCtrlRdWr::kLc);
    }
    drain(RramCtrlRdWr::kLc);
    uint32_t err = rdwr_.cur.err;
    for (uint32_t i = 0; i < seed_buf_.size() && i < 8; ++i) lc_.seed_word(i, seed_buf_[i].first, true);
    note(rdwr_.done(RramCtrlRdWr::kLc, err));
    if (err & kMpErr) fault |= 1u << kFaultLcmgrMp;
    if (err & kRdErr) fault |= 1u << kFaultLcmgrRd;
    lc_.seed_done(err);
  }

  // The scrambler's key registers: the sideloaded keys once valid, the random keys after a disable.
  static rram_u128 k128(const Key &k) {
    rram_u128 v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 32) | k[i];
    return v;
  }
  void step_keys() {
    scr_.step(disabled_ ? kMuBi4True : kMuBi4False, lc_.keys_acked(), k128(lc_.addr_key()),
              k128(lc_.data_key()), k128(lc_.rand_addr_key()), k128(lc_.rand_data_key()));
  }

  RramCtrlRdWr rdwr_;
  Eg2RramLcmgrCore lc_;
  RramScramble scr_;
  rram_u128 rst_addr_key_ = 0, rst_data_key_ = 0;
  bool disabled_ = false;
  bool wr_on_ = false;
  unsigned part_ = 0;
  uint32_t base_ = 0, n_ = 0, wr_i_ = 0;
  std::vector<uint32_t> unknown_;
  std::vector<std::pair<uint32_t, bool>> seed_buf_;
  std::vector<std::string> findings_;
};

}  // namespace eg2_model

#endif  // EG2_RRAM_CTRL_TOP_CORE_H_
