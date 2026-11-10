// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl controller path: the reference model of the arbiter (rram_ctrl_arb), the read engine
// (rram_ctrl_rd) and the write engine (rram_ctrl_wr), for the controller-path unit bench
// (hw/ip/rram_ctrl/dv/edagen/rram_ctrl_rdwr, through dpi/eg2_rram_ctrl_rdwr_dpi.cc). It is a
// judge: the bench reports what it sees (requests of the three requesters, the words they push,
// the RRAM requests and their answers, the words delivered and the completions) and every report
// is checked against the documented behaviour; a non-empty string is the reason it is not allowed.
//
// From the documentation, never from the RTL:
//   hw/ip/rram_ctrl/doc/theory_of_operation.md
//     "RRAM Controller Description": rram_ctrl_arb arbitrates between software (FIFO path) and the
//       hardware plugs; rram_ctrl_rd "returns data to the software read FIFO"; rram_ctrl_wr "drains
//       the software write FIFO and issues bursts to rram_phy".
//     "Stage 1": "Until then [phy_init_done], no RRAM accesses (including hardware interfaces) are
//       possible." "Stage 2": software operations are rejected with op_err until init_done and
//       keys_valid.
//     "Address-XOR": the XOR (with the full bus-word address) is removed "in rram_ctrl_rd for
//       software-initiated controller reads"; the integrity bits pass unchanged (computed by the
//       PHY over the clean word).
//     "RMA Handling": "waits for any pending RRAM transaction to complete, then switches priority to
//       the hardware interface"; with rma_sw_dis new software operations "are not started, but
//       completed immediately with OP_STATUS.err and ERR_CODE.op_err set"; an operation already in
//       progress "completes normally".
//     "RRAM Escalation & Disable": "all host and controller requests are blocked".
//     "Requesters": SwSel, HwOtpSel, HwLcMgrSel, HwLoopBack (Rewrite); lcmgr requests carry a phase.
//     "RRAM Errors and Faults": ERR_CODE op_err / mp_err / rd_err / wr_err, set when the operation
//       completes.
//     "RRAM Write Data Path": up to CtrlMaxWords = 1024 bus words, aligned to an RRAM word.
//   hw/ip/rram_ctrl/doc/programmer's guide
//     "Issuing a Controller Read": NUM + 1 bus words from ADDR (truncated to a 4-byte boundary);
//       the controller stalls while the read FIFO is full; done when all words are read.
//     "Issuing a Controller Write": START "clears the write FIFO, discarding any data pushed to it
//       earlier"; "Data must be pushed after the start has been sent"; done "when all words have
//       been programmed". "Write Alignment": a write that is not 16-byte aligned or not a multiple
//       of 4 words "is rejected without accessing the RRAM" with op_err.
//     "Issuing a Rewrite Operation": reads the addressed RRAM word (4 bus words, NUM ignored) and
//       writes the same data back, "without exposing it to software or the read/write FIFOs";
//       unaligned: op_err without access; a failing read or write ends it with the error.
//     "Error Encountered by Software Initiated Controller Operations": op_err covers undefined OP,
//       not initialized, RMA software disable, write/Rewrite not covering whole RRAM words; ERR_ADDR
//       "the address at which the first error was encountered"; "the controller aborts at the first
//       error". "Errors During Multi-Word Controller Reads": no new RRAM read after the first error,
//       the full number of words still returned, all-ones with valid bus integrity from the failing
//       word on.
//   hw/ip/rram_ctrl/doc/registers.md CONTROL (NUM, OP values Read 0 / Write 1 / Rewrite 2, other
//     values reserved), ADDR, ERR_CODE, ERR_ADDR.
//   RFC "RRAM Controller for Earl Grey" 4.1: the write FIFO "is shared between software controlled
//     operations and hardware plugs and is cleared whenever the arbiter switches the interface";
//     "the controller arbiter can lock the interface selection until a command is fully processed".
//   hw/ip/tlul doc (bus integrity): prim_secded_inv_39_32 over the 32 data bits (OpenTitan's
//     secded_enc.c, as eg2_rram_phy_core.h).
//
// Not in the documentation (findings drafted in status/rtl_tickets.md); the model takes:
//   - the order between the two hardware plugs when both request: not checked (either may go first);
//   - whether a write that failed still takes its remaining words from the FIFO: both allowed (no
//     further RRAM write either way);
//   - whether a software request made after rram_disable is ever completed: not predicted (counted);
//   - the arbiter's decision latency: a software operation must not begin when a hardware request
//     has been waiting for kDecide cycles or more (the bench's allowance for the decision cycle and
//     the engine's first request);
//   - multibit disables: any value other than MuBi4False disables (the fail-safe reading the
//     documents use for DIS.SW_DIS).

#ifndef EG2_RRAM_CTRL_RDWR_CORE_H_
#define EG2_RRAM_CTRL_RDWR_CORE_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace eg2_model {

struct RramCtrlRdWr {
  static constexpr uint32_t kBusAddrW = 19;   // bus-word address of a partition (ADDR.START >> 2)
  static constexpr uint32_t kMaxWords = 1024;  // CtrlMaxWords
  static constexpr uint32_t kDecide = 2;
  enum Who : int { kSw = 0, kOtp = 1, kLc = 2, kNone = 3 };
  enum Op : uint32_t { kRead = 0, kWrite = 1, kRewrite = 2 };
  // ERR_CODE bits
  static constexpr uint32_t kOpErr = 1, kMpErr = 2, kRdErr = 4, kWrErr = 8;
  // requester select ("Requesters" table; the interface's encoding)
  enum Sel : uint32_t { kSelNone = 0, kSelSw = 1, kSelOtp = 2, kSelLc = 3, kSelLoop = 4 };

  static uint8_t parity(uint32_t w, bool inv) {
    return uint8_t((__builtin_popcount(w) & 1) ^ (inv ? 1 : 0));
  }
  static uint8_t intg(uint32_t w) {
    return uint8_t(parity(w & 0x2606bd25, false) << 0 | parity(w & 0xdeba8050, true) << 1 |
                   parity(w & 0x413d89aa, false) << 2 | parity(w & 0x31234ed1, true) << 3 |
                   parity(w & 0xc2c1323b, false) << 4 | parity(w & 0x2dcc624c, true) << 5 |
                   parity(w & 0x98505586, false) << 6);
  }
  static uint64_t with_intg(uint32_t w) { return uint64_t(intg(w)) << 32 | w; }
  static uint64_t ones39() { return with_intg(0xffffffffu); }
  static constexpr uint64_t kMask39 = (uint64_t(1) << 39) - 1;

  struct Request {
    bool pending = false;
    uint32_t op = 0, part = 0, num = 0, addr = 0, phase = 0;
    uint64_t since = 0;
    bool static_bad = false;       // undefined op, or write/Rewrite not covering whole RRAM words
    bool seen_closed = false;      // a cycle while pending with init not done or software disabled
    bool seen_open = false;        // a cycle while pending with init done and software enabled
    bool after_disable = false;    // made or still pending when rram_disable came
    std::deque<uint64_t> words;    // words pushed after the request, before its operation began
  };

  struct Cur {
    bool on = false;
    int who = kNone;
    bool loop = false;             // a Rewrite (HwLoopBack)
    int stage = 0;                 // Rewrite: 0 reading, 1 writing
    uint32_t op = 0, part = 0, n = 0, base = 0, phase = 0;
    uint32_t rd_issued = 0, rd_answered = 0, wr_issued = 0, wr_answered = 0, delivered = 0;
    bool rd_out = false, wr_out = false;
    uint32_t out_rd_addr = 0, out_wr_addr = 0;
    std::deque<uint64_t> expect;   // words to deliver, in order
    std::deque<uint64_t> fifo;     // the requester's words for the write, in order
    std::vector<uint64_t> rw_data; // Rewrite: the four words read, XOR removed
    uint32_t err = 0;
    bool err_seen = false;
    uint32_t err_addr = 0;
    uint64_t begun = 0;
  };

  Request rq[3];
  Cur cur;
  uint64_t cyc = 0;
  bool phy_init = false, ctrl_init = false, disabled = false, sw_dis = false;
  uint64_t disable_cyc = 0;

  // statistics for the report and coverage
  uint32_t n_ops[3][3] = {};       // [who][op] begun
  uint32_t n_rejected = 0, n_done = 0, n_words_rd = 0, n_words_wr = 0, n_err_done = 0;
  uint32_t n_contended = 0, n_hw_both = 0, n_unanswered_after_dis = 0;
  bool known_erraddr_last = false;  // the last done() failed only on ERR_ADDR, failing word last

  static const char *who_name(int w) {
    return w == kSw ? "software" : w == kOtp ? "OTP plug" : w == kLc ? "lcmgr plug" : "none";
  }
  static std::string fmt(const char *f, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0,
                         uint64_t d = 0) {
    char buf[320];
    snprintf(buf, sizeof buf, f, (unsigned long long)a, (unsigned long long)b,
             (unsigned long long)c, (unsigned long long)d);
    return buf;
  }

  void reset() { *this = RramCtrlRdWr(); }

  // Once per cycle, before that cycle's events: the gating inputs.
  void cycle(bool phy_init_done, bool ctrl_init_done, bool disable_any, bool sw_disable_any) {
    ++cyc;
    phy_init = phy_init || phy_init_done;
    ctrl_init = ctrl_init_done;
    if (disable_any && !disabled) disable_cyc = cyc;
    disabled = disabled || disable_any;
    sw_dis = sw_disable_any;
    for (Request &r : rq) {
      if (!r.pending) continue;
      if (disabled) r.after_disable = true;
    }
    Request &s = rq[kSw];
    if (s.pending && !(cur.on && cur.who == kSw)) {
      if (!ctrl_init || sw_dis) s.seen_closed = true;
      else s.seen_open = true;
    }
    if (rq[kSw].pending && (rq[kOtp].pending || rq[kLc].pending)) ++n_contended;
    if (rq[kOtp].pending && rq[kLc].pending) ++n_hw_both;
  }

  // A new request: software START, or a plug's start with its req.
  std::string request(int who, uint32_t op, uint32_t part, uint32_t num, uint32_t byte_addr,
                      uint32_t phase) {
    Request &r = rq[who];
    if (r.pending) return std::string(who_name(who)) + ": a new request while its previous one is pending";
    r = Request();
    r.pending = true;
    r.op = op;
    r.part = part;
    r.num = num;
    r.addr = byte_addr;
    r.phase = phase;
    r.since = cyc;
    r.after_disable = disabled;
    if (who == kSw) {
      bool bad_op = op > kRewrite;
      bool unaligned = (byte_addr & 15) != 0;
      bool bad_num = ((num + 1) & 3) != 0;
      r.static_bad = bad_op || (op == kWrite && (unaligned || bad_num)) || (op == kRewrite && unaligned);
      if (!ctrl_init || sw_dis) r.seen_closed = true;
      else r.seen_open = true;
    }
    return "";
  }

  // A word the requester's write port handed over (valid and ready).
  void push(int who, uint64_t d39) {
    d39 &= kMask39;
    if (cur.on && cur.who == who && !cur.loop) {
      if (cur.op == kWrite) cur.fifo.push_back(d39);
      return;
    }
    Request &r = rq[who];
    if (r.pending && r.op == kWrite) r.words.push_back(d39);
    // otherwise a stray word: it must never reach the RRAM (no write expects it)
  }

  bool hw_waiting_long() const {
    for (int w : {kOtp, kLc})
      if (rq[w].pending && rq[w].since + kDecide <= cyc && !(cur.on && cur.who == w)) return true;
    return false;
  }

  // The first RRAM request of an operation: whose it is (sel) and whether it may begin.
  std::string begin(uint32_t sel, bool is_wr) {
    if (!phy_init) return "an RRAM access before phy_init_done";
    int who = sel == kSelSw || sel == kSelLoop ? kSw : sel == kSelOtp ? kOtp : sel == kSelLc ? kLc : kNone;
    if (who == kNone) return fmt("an RRAM request with requester select %llu (no requester)", sel);
    Request &r = rq[who];
    if (!r.pending) return std::string("an RRAM request for the ") + who_name(who) + ", which has no request";
    if (disabled && disable_cyc + kDecide <= cyc)
      return std::string("an operation of the ") + who_name(who) + " began after rram_disable";
    if (who == kSw) {
      if (r.static_bad) return "a software operation began that must be rejected (undefined OP, or not whole RRAM words)";
      if (!r.seen_open) return "a software operation began while init was not done or software access was disabled";
      if (hw_waiting_long()) return "a software operation began while a hardware request was waiting (hardware has priority)";
      bool loop = sel == kSelLoop;
      if (loop != (r.op == kRewrite))
        return std::string(r.op == kRewrite ? "a software Rewrite" : "a software Read/Write") +
               fmt(" ran under requester select %llu", sel);
    }
    uint32_t op = r.op;
    if (who != kSw && op > kWrite) return std::string("the ") + who_name(who) + " was started with an undefined OP, yet it accesses the RRAM";
    if (op == kRead && is_wr) return "a write request for a read operation";
    if ((op == kWrite) && !is_wr) return "a read request for a write operation";
    if (op == kRewrite && is_wr) return "a Rewrite began with a write, not with the read";
    cur = Cur();
    cur.on = true;
    cur.who = who;
    cur.loop = op == kRewrite;
    cur.op = op;
    cur.part = r.part;
    cur.n = op == kRewrite ? 4 : r.num + 1;
    cur.base = (r.addr >> 2) & ((1u << kBusAddrW) - 1);
    cur.phase = r.phase;
    cur.begun = cyc;
    if (op == kWrite) cur.fifo = r.words;  // pushed after the START, so part of this operation
    r.words.clear();
    n_ops[who][op]++;
    return "";
  }

  std::string check_req(bool is_wr, uint32_t addr, bool ovfl, uint32_t sel, uint32_t part, uint32_t phase,
                        uint32_t idx) {
    uint32_t want_sel = cur.loop ? kSelLoop : cur.who == kSw ? kSelSw : cur.who == kOtp ? kSelOtp : kSelLc;
    if (sel != want_sel)
      return fmt("RRAM request under requester select %llu, the operation is the ", sel) + who_name(cur.who) + "'s";
    uint64_t a = uint64_t(cur.base) + idx;
    uint32_t want = uint32_t(a & ((1u << kBusAddrW) - 1));
    bool want_ovfl = a >> kBusAddrW;
    if (addr != want || ovfl != want_ovfl)
      return std::string(is_wr ? "write" : "read") +
             fmt(" request %llu of the operation at bus-word address 0x%llx (ovfl %llu)", idx, addr, ovfl) +
             fmt(", expected 0x%llx (ovfl %llu)", want, want_ovfl);
    if (part != cur.part) return fmt("RRAM request in partition %llu, the operation's is %llu", part, cur.part);
    if (cur.who == kLc && phase != cur.phase)
      return fmt("lcmgr request with phase %llu, lcmgr asked for phase %llu", phase, cur.phase);
    return "";
  }

  std::string rd_req(uint32_t addr, bool ovfl, uint32_t sel, uint32_t part, uint32_t phase) {
    if (!cur.on) {
      std::string e = begin(sel, false);
      if (!e.empty()) return e;
    }
    if (cur.rd_out) return "a second RRAM read request while one is outstanding";
    if (cur.op == kWrite || (cur.loop && cur.stage != 0)) return "an RRAM read during a write";
    if (cur.err_seen) return "an RRAM read after the operation's first error (documented: no further accesses)";
    if (cur.rd_issued >= cur.n) return fmt("RRAM read %llu of an operation of %llu words", cur.rd_issued + 1, cur.n);
    std::string e = check_req(false, addr, ovfl, sel, part, phase, cur.rd_issued);
    if (!e.empty()) return e;
    cur.rd_out = true;
    cur.out_rd_addr = addr;
    cur.rd_issued++;
    return "";
  }

  // The answer to the outstanding read: the PHY's word (the address XOR still in it), its errors.
  std::string rd_rsp(uint64_t d39, bool rd_err, bool mp_err) {
    if (!cur.on || !cur.rd_out) return "a read answer without a read request";
    cur.rd_out = false;
    cur.rd_answered++;
    n_words_rd++;
    uint64_t w;
    if (rd_err || mp_err) {
      if (!cur.err_seen) {
        cur.err_seen = true;
        cur.err_addr = cur.out_rd_addr;
        cur.err |= (rd_err ? kRdErr : 0) | (mp_err ? kMpErr : 0);
      }
      w = ones39();
    } else {
      uint32_t clean = uint32_t(d39) ^ cur.out_rd_addr;  // "XORed with its full bus-word address"
      w = (d39 & ~uint64_t(0xffffffffu)) | clean;
    }
    if (cur.loop) {
      cur.rw_data.push_back(w);
      if (cur.err_seen) {
        // the rest is never read; nothing goes to software
      }
    } else {
      cur.expect.push_back(w);
      if (cur.err_seen)  // the remaining words: all ones, no RRAM access
        while (cur.expect.size() + cur.delivered < cur.n) cur.expect.push_back(ones39());
    }
    if (cur.loop && !cur.err_seen && cur.rd_answered == 4) cur.stage = 1;
    return "";
  }

  // A word delivered to a requester's read port (OTP: no data, check_data false).
  std::string deliver(int port, uint64_t d39, bool check_data) {
    d39 &= kMask39;
    if (!cur.on) return std::string("a read word to the ") + who_name(port) + " with no operation running";
    if (cur.loop) return "a Rewrite's read data reached the software read FIFO (documented: not exposed)";
    if (port != cur.who) return std::string("a read word to the ") + who_name(port) + ", the operation is the " + who_name(cur.who) + "'s";
    if (cur.op != kRead) return "a read word during a write";
    if (cur.expect.empty()) return "a read word before its RRAM answer";
    uint64_t e = cur.expect.front();
    cur.expect.pop_front();
    cur.delivered++;
    if (check_data && d39 != e)
      return fmt("read word %llu delivered as 0x%010llx, expected 0x%010llx", cur.delivered - 1, d39, e) +
             (e == ones39() ? " (all ones after an error)" : " (XOR with the bus-word address removed)");
    return "";
  }

  std::string wr_req(uint32_t addr, bool ovfl, uint64_t d39, bool last, uint32_t sel, uint32_t part,
                     uint32_t phase) {
    d39 &= kMask39;
    if (!cur.on) {
      std::string e = begin(sel, true);
      if (!e.empty()) return e;
    }
    if (cur.wr_out) return "a second RRAM write request while one is outstanding";
    if (cur.op == kRead) return "an RRAM write during a read";
    if (cur.loop && cur.stage != 1) return "a Rewrite writes before its read completed without error";
    if (cur.err_seen) return "an RRAM write after the operation's first error (documented: the controller aborts)";
    if (cur.wr_issued >= cur.n) return fmt("RRAM write %llu of an operation of %llu words", cur.wr_issued + 1, cur.n);
    std::string e = check_req(true, addr, ovfl, sel, part, phase, cur.wr_issued);
    if (!e.empty()) return e;
    uint64_t want;
    if (cur.loop) {
      want = cur.rw_data[cur.wr_issued];
    } else {
      if (cur.wr_issued >= cur.fifo.size())
        return fmt("RRAM write %llu carries a word the requester never pushed for this operation (pushed %llu): ",
                   cur.wr_issued, cur.fifo.size()) + who_name(cur.who);
      want = cur.fifo[cur.wr_issued];
    }
    if (d39 != want)
      return fmt("RRAM write %llu with data 0x%010llx, expected 0x%010llx (the requester's word %llu)", cur.wr_issued, d39, want, cur.wr_issued);
    bool want_last = cur.wr_issued + 1 == cur.n;
    if (last != want_last) return fmt("RRAM write %llu: last %llu, expected %llu", cur.wr_issued, last, want_last);
    cur.wr_out = true;
    cur.out_wr_addr = addr;
    cur.wr_issued++;
    return "";
  }

  std::string wr_rsp(bool mp_err, bool intg_err) {
    if (!cur.on || !cur.wr_out) return "a write answer without a write request";
    cur.wr_out = false;
    cur.wr_answered++;
    n_words_wr++;
    if ((mp_err || intg_err) && !cur.err_seen) {
      cur.err_seen = true;
      cur.err_addr = cur.out_wr_addr;
      cur.err |= (mp_err ? kMpErr : 0) | (intg_err ? kWrErr : 0);
    }
    return "";
  }

  // A completion to a requester (Rewrite: to software). err in ERR_CODE bit order; err_addr the
  // bus-word address the arbiter reports (ERR_ADDR), checked for software with mp/rd/wr errors.
  std::string done(int who, uint32_t err, uint32_t err_addr = 0) {
    known_erraddr_last = false;
    Request &r = rq[who];
    if (!r.pending) return std::string("a completion to the ") + who_name(who) + ", which has no request";
    if (!(cur.on && cur.who == who)) {
      // no operation of this requester ran: only a software rejection may complete it
      if (who != kSw) return std::string("the ") + who_name(who) + " completed without any RRAM access";
      if (!(r.static_bad || r.seen_closed))
        return "software completed without an RRAM access although its operation is allowed";
      if (err != kOpErr) return fmt("a rejected software operation completed with ERR_CODE 0x%llx, expected op_err only", err);
      r = Request();
      n_rejected++;
      n_done++;
      n_err_done++;
      return "";
    }
    if (cur.rd_out || cur.wr_out) return std::string("the ") + who_name(who) + "'s operation completed with an RRAM request outstanding";
    std::string why;
    if (cur.op == kRead) {
      if (cur.delivered != cur.n) why = fmt("read completed after %llu of %llu words", cur.delivered, cur.n);
    } else if (cur.op == kWrite) {
      if (!cur.err_seen && cur.wr_answered != cur.n)
        why = fmt("write completed after %llu of %llu words programmed", cur.wr_answered, cur.n);
    } else {
      if (!cur.err_seen && cur.wr_answered != 4)
        why = fmt("Rewrite completed after %llu of 4 words read and %llu written", cur.rd_answered, cur.wr_answered);
    }
    if (why.empty() && err != cur.err)
      why = fmt("completed with ERR_CODE 0x%llx, expected 0x%llx", err, cur.err);
    known_erraddr_last = false;
    if (why.empty() && who == kSw && (cur.err & (kMpErr | kRdErr | kWrErr)) && err_addr != cur.err_addr) {
      uint32_t idx = (cur.err_addr - cur.base) & ((1u << kBusAddrW) - 1);
      // the failing word is the last one: the completion comes in the same cycle as the error
      // (the finding drafted in status/rtl_tickets.md); flagged so the bench can count it
      known_erraddr_last = idx + 1 == cur.n;
      why = fmt("ERR_ADDR (bus word) 0x%llx, the first failing word is 0x%llx (word %llu of %llu)", err_addr,
                cur.err_addr, idx + 1, cur.n);
    }
    n_done++;
    if (err) n_err_done++;
    cur.on = false;
    r = Request();
    if (!why.empty()) return std::string(who_name(who)) + ": " + why;
    return "";
  }

  // At the end of the run: what is still open.
  std::string end_check() {
    std::string s;
    // rram_disable blocks every request ("all host and controller requests are blocked"): an
    // operation caught by it is counted, not judged (whether it must complete is not documented).
    if (cur.on && disabled) {
      n_unanswered_after_dis++;
      rq[cur.who].pending = false;
    } else if (cur.on)
      s += std::string("an operation of the ") + who_name(cur.who) +
           fmt(" never completed (%llu of %llu words); ", cur.op == kRead ? cur.delivered : cur.wr_answered, cur.n);
    for (int w : {kSw, kOtp, kLc}) {
      if (!rq[w].pending) continue;
      if (rq[w].after_disable) { n_unanswered_after_dis++; continue; }
      if (cur.on && cur.who == w) continue;
      s += std::string("a request of the ") + who_name(w) + " never answered; ";
    }
    return s;
  }
};

}  // namespace eg2_model

#endif  // EG2_RRAM_CTRL_RDWR_CORE_H_
