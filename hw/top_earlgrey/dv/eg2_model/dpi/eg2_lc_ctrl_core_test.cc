// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The life cycle controller core's self-test (ip/eg2_lc_ctrl_core.h), on an OTP of its own that
// refuses to clear a programmed bit as the real one does. Its centre is the forward-only rule
// (Henrik 2026-11-07: "we cant go back in the state only forward"): no transition goes back, PROD
// never reaches TEST or RAW, the count only increases and SCRAP is final. EG2_LC_TEST_BREAK=1
// expects PROD -> TEST_UNLOCKED0 to succeed: the test must then fail.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I<eg2 checkout> dpi/eg2_lc_ctrl_core_test.cc -o /tmp/lc_core_test
//   /tmp/lc_core_test && ! EG2_LC_TEST_BREAK=1 /tmp/lc_core_test > /dev/null

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "../ip/eg2_lc_ctrl_core.h"

namespace {

using C = Eg2LcCtrlCore;
int fails = 0, checks = 0;
const bool kBreak = std::getenv("EG2_LC_TEST_BREAK") != nullptr;

void check(bool ok, const std::string &what) {
  ++checks;
  if (!ok) {
    ++fails;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

const std::vector<uint8_t> kUnlock = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
const std::vector<uint8_t> kExit = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
const std::vector<uint8_t> kRma = {0xa5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0x5a};
const std::vector<uint8_t> kRaw = {0x11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0x22};

struct Port : Eg2LcCtrlPort {
  uint16_t otp[44] = {};  // count words, then state words
  bool secret0 = true, secret2 = true, prog_fail = false, nvm_ok = true;
  int programs = 0, outputs = 0, transitions = 0;
  std::map<std::string, int> alerts;
  Eg2LcOtpInput otp_input() override {
    Eg2LcOtpInput in;
    in.valid = true;
    for (int j = 0; j < 24; ++j) in.count[j] = otp[j];
    for (int i = 0; i < 20; ++i) in.state[i] = otp[24 + i];
    in.test_tokens_valid = secret0;
    in.rma_token_valid = in.secrets_valid = secret2;
    if (secret0) {
      in.test_unlock_hash = C::hash_token(kUnlock);
      in.test_exit_hash = C::hash_token(kExit);
    }
    if (secret2) in.rma_hash = C::hash_token(kRma);
    in.device_id[0] = 0xd0d0;
    return in;
  }
  bool otp_program(const uint16_t w[44]) override {
    ++programs;
    if (prog_fail && programs > 1) return false;
    for (int i = 0; i < 44; ++i) {
      if (otp[i] & ~w[i]) return false;  // OTP never clears a programmed bit
    }
    for (int i = 0; i < 44; ++i) otp[i] = w[i];
    return true;
  }
  bool nvm_rma() override { return nvm_ok; }
  void fatal_alert(const char *name) override { alerts[name]++; }
  void outputs_changed() override { ++outputs; }
  void transition_done(uint32_t) override { ++transitions; }
  void set(uint32_t state, uint32_t count) {
    for (int j = 0; j < 24; ++j) otp[j] = eg2_lc_count_word(count, j);
    for (int i = 0; i < 20; ++i) otp[24 + i] = eg2_lc_state_word(state, i);
  }
  int otp_state() const {
    for (uint32_t s = 0; s <= kLcScrap; ++s) {
      bool ok = true;
      for (int i = 0; i < 20 && ok; ++i) ok = otp[24 + i] == eg2_lc_state_word(s, i);
      if (ok) return int(s);
    }
    return -1;
  }
  int otp_count() const {
    for (uint32_t n = 0; n <= 24; ++n) {
      bool ok = true;
      for (int j = 0; j < 24 && ok; ++j) ok = otp[j] == eg2_lc_count_word(n, j);
      if (ok) return int(n);
    }
    return -1;
  }
};

struct Dut {
  Port port;
  C core{&port};
  Dut(uint32_t state, uint32_t count) {
    port.set(state, count);
    core.power_up();
  }
  void reboot() { core.power_up(); }
  // Software's transition (PG "Regular Life Cycle Transitions"); returns STATUS.
  uint32_t sw(uint32_t target, const std::vector<uint8_t> &token = {}) {
    core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
    core.csr_write(C::kSw, C::kTarget, eg2_lc_rep5(target));
    for (unsigned w = 0; w < 4; ++w) {
      uint32_t x = 0;
      for (unsigned i = 0; i < 4 && 4 * w + i < token.size(); ++i) {
        x |= uint32_t(token[4 * w + i]) << (8 * i);
      }
      core.csr_write(C::kSw, C::kToken0 + w, x);
    }
    core.csr_write(C::kSw, C::kCmd, 1);
    return core.status();
  }
};

constexpr uint32_t kOk = C::kTransitionSuccessful, kTErr = C::kTransitionError,
                   kTokErr = C::kTokenError;

void test_decode() {
  for (uint32_t s = 0; s <= kLcScrap; ++s) {
    Dut d(s, s == kLcRaw ? 0 : 5);
    check(d.core.state() == s && d.core.csr_read(C::kSw, C::kLcState) == eg2_lc_rep5(s),
          "decode state " + std::to_string(s));
    check((d.core.status() & (C::kInitialized | C::kReady)) == (C::kInitialized | C::kReady),
          "INITIALIZED and READY in state " + std::to_string(s));
  }
  Dut d(kLcProd, 7);
  check(d.core.csr_read(C::kSw, C::kLcCnt) == 7, "LC_TRANSITION_CNT decoded");
  check(d.core.csr_read(C::kSw, C::kHwRev0) == 0x40010010 &&
            d.core.csr_read(C::kSw, C::kHwRev1) == 1 &&
            d.core.csr_read(C::kSw, C::kDeviceId0) == 0xd0d0,
        "HW_REVISION and DEVICE_ID");
  check(d.core.csr_read(C::kSw, C::kIdState) == 0x55555555, "LC_ID_STATE PERSONALIZED");
  Dut b(kLcProd, 7);
  b.port.otp[30] ^= 1;  // a corrupted state word
  b.reboot();
  check(b.core.state() == kLcInvalid && (b.core.status() & C::kStateError) &&
            !(b.core.status() & C::kReady) && b.port.alerts["fatal_state_error"] == 1,
        "corrupted state: INVALID, STATE_ERROR, fatal_state_error");
  check(b.core.csr_read(C::kSw, C::kLcCnt) == 31, "INVALID: the count reads 31");
  Dut c(kLcDev, 3);
  c.port.otp[0] ^= 2;  // a corrupted count word
  c.reboot();
  check(c.core.state() == kLcInvalid, "corrupted count: INVALID");
  Dut e(kLcDev, 24);
  check(e.core.state() == kLcScrap && e.core.count() == 24, "count 24 reads SCRAP");
}

void test_signals() {
  struct Row {
    uint32_t s;
    bool cpu, dbg, dft, nvm, km, esc;
  };
  const Row rows[] = {
      {kLcRaw, 0, 0, 0, 0, 0, 0},   {1, 1, 1, 1, 1, 0, 0},       {2, 0, 0, 0, 0, 0, 0},
      {kLcTestUnlocked7, 1, 1, 1, 0, 0, 0},                        {kLcDev, 1, 1, 0, 0, 1, 0},
      {kLcProd, 1, 0, 0, 0, 1, 0},  {kLcProdEnd, 1, 0, 0, 0, 1, 0}, {kLcRma, 1, 1, 1, 1, 1, 0},
      {kLcScrap, 0, 0, 0, 0, 0, 1},
  };
  for (const Row &r : rows) {
    Dut d(r.s, r.s ? 4 : 0);
    std::string n = " in state " + std::to_string(r.s);
    check(d.core.cpu_en() == r.cpu, "CPU_EN" + n);
    check(d.core.hw_debug_en() == r.dbg, "HW_DEBUG_EN" + n);
    check(d.core.dft_en() == r.dft, "DFT_EN" + n);
    check(d.core.nvm_debug_en() == r.nvm, "NVM_DEBUG_EN (off in TEST_UNLOCKED7)" + n);
    check(d.core.keymgr_en() == r.km, "KEYMGR_EN" + n);
    check(d.core.escalate_en() == r.esc && d.core.hw_debug_clr() == r.esc, "ESCALATE_EN" + n);
  }
  Dut p(kLcProd, 4);
  check(!p.core.creator_seed_sw_rw_en() && p.core.seed_hw_rd_en() && p.core.owner_seed_sw_rw_en() &&
            p.core.iso_part_sw_rd_en() && p.core.iso_part_sw_wr_en() && !p.core.rma_state(),
        "PROD, personalized: access signals");
  p.port.secret2 = false;
  p.reboot();
  check(p.core.creator_seed_sw_rw_en() && !p.core.seed_hw_rd_en(),
        "PROD, not personalized: creator seed writable, not readable by hardware");
  Dut r(kLcRma, 4);
  r.port.secret2 = false;
  r.reboot();
  check(r.core.creator_seed_sw_rw_en() && !r.core.seed_hw_rd_en() && r.core.rma_state(),
        "RMA, not personalized: SEED_HW_RD_EN needs SECRET2 locked (ToO)");
  Dut t(1, 1);
  check(!t.core.iso_part_sw_rd_en() && t.core.iso_part_sw_wr_en() && !t.core.owner_seed_sw_rw_en(),
        "TEST_UNLOCKED: isolated partition write-only, no seeds");
}

void test_mutex() {
  Dut d(1, 1);
  d.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  check(d.core.csr_read(C::kSw, C::kClaim) == C::kMuBi8True &&
            d.core.csr_read(C::kSw, C::kTransRegwen) == 1,
        "software claims the mutex; TRANSITION_REGWEN 1");
  check(d.core.csr_read(C::kTap, C::kClaim) == 0, "the TAP then reads the claim as 0");
  d.core.csr_write(C::kSw, C::kTarget, 0x1234);
  check(d.core.csr_read(C::kSw, C::kTarget) == 0x1234 && d.core.csr_read(C::kTap, C::kTarget) == 0,
        "transition registers read 0 to the side without the mutex");
  d.core.csr_write(C::kTap, C::kClaim, C::kMuBi8True);
  check(d.core.mutex() == C::kSw, "the TAP cannot take a claimed mutex");
  d.core.csr_write(C::kTap, C::kTarget, 0x55);
  check(d.core.csr_read(C::kSw, C::kTarget) == 0x1234, "the side without the mutex cannot write");
  d.core.csr_write(C::kSw, C::kClaim, 0);
  check(d.core.mutex() == C::kNone && d.core.csr_read(C::kSw, C::kClaim) == C::kMuBi8False &&
            d.core.csr_read(C::kSw, C::kTransRegwen) == 0,
        "release: the claim reads 0x69 and TRANSITION_REGWEN 0");
  d.core.csr_write(C::kSw, C::kClaimRegwen, 0);
  d.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  check(d.core.mutex() == C::kNone, "CLAIM_TRANSITION_IF_REGWEN 0: software cannot claim");
  d.core.csr_write(C::kTap, C::kClaim, C::kMuBi8True);
  check(d.core.mutex() == C::kTap, "the TAP still can");
  int programs = d.port.programs;
  d.core.csr_write(C::kSw, C::kCmd, 1);
  check(d.port.programs == programs, "software's START without the mutex does nothing");
}

void test_transitions() {
  // Unconditional TEST_UNLOCKED0 -> TEST_LOCKED0.
  Dut d(1, 1);
  check(d.sw(2) == (C::kInitialized | C::kReady | kOk), "TU0 -> TL0 succeeds");
  check(d.port.otp_state() == 2 && d.port.otp_count() == 2, "OTP: state TL0, count 2");
  check(d.core.state() == kLcPostTransition && d.core.csr_read(C::kSw, C::kLcCnt) == 31 &&
            !d.core.cpu_en() && d.core.hw_debug_clr() && d.core.check_byp_en(),
        "POST_TRANSITION: count reads 31, CPU off, HW_DEBUG_CLR, CHECK_BYP_EN");
  check(d.core.csr_read(C::kSw, C::kTransRegwen) == 0, "TRANSITION_REGWEN 0 after a transition");
  int programs = d.port.programs;
  d.core.csr_write(C::kSw, C::kCmd, 1);
  check(d.port.programs == programs, "no second transition before reset");
  d.reboot();
  check(d.core.state() == 2 && d.core.count() == 2, "after reset: TL0, count 2");
  // Conditional: TL0 -> TU1 with the TEST_UNLOCK token.
  check(d.sw(3, kExit) & kTokErr, "TL0 -> TU1 with a wrong token: TOKEN_ERROR");
  check(d.port.otp_count() == 3 && d.port.otp_state() == 2, "the failed attempt still counted");
  d.reboot();
  check(d.sw(3, kUnlock) & kOk, "TL0 -> TU1 with the TEST_UNLOCK token");
  d.reboot();
  check(d.core.state() == 3, "TU1 after reset");
  // TEST_EXIT needs SECRET0 locked.
  Dut e(3, 4);
  e.port.secret0 = false;
  e.reboot();
  check(e.sw(kLcProd, kExit) & kTokErr, "TEST_EXIT with SECRET0 unlocked: TOKEN_ERROR");
  e.port.secret0 = true;
  e.reboot();
  check(e.sw(kLcProd, kExit) & kOk, "TU1 -> PROD with the TEST_EXIT token");
  // RAW unlock.
  Dut r(kLcRaw, 0);
  check(r.sw(1, kRaw) & kTokErr, "RAW -> TU0 without the RAW token configured: TOKEN_ERROR");
  r.core.config().raw_unlock_hash = C::hash_token(kRaw);
  r.reboot();
  check(r.sw(1, kRaw) & kOk, "RAW -> TU0 with the RAW unlock token");
  // RMA: token from DEV/PROD, none from TEST_UNLOCKED, NVM wipe.
  Dut m(kLcDev, 5);
  check(m.sw(kLcRma, kExit) & kTokErr, "DEV -> RMA with a wrong token: TOKEN_ERROR");
  m.reboot();
  m.port.nvm_ok = false;
  check(m.sw(kLcRma, kRma) & C::kNvmRmaError, "DEV -> RMA, NVM wipe fails: NVM_RMA_ERROR");
  check(m.port.otp_state() == int(kLcDev), "state unchanged after NVM_RMA_ERROR");
  m.port.nvm_ok = true;
  m.reboot();
  check(m.sw(kLcRma, kRma) & kOk, "DEV -> RMA with the RMA token");
  Dut u(5, 3);
  check(u.sw(kLcRma) & kOk, "TU2 -> RMA unconditional");
  // OTP programming error.
  Dut o(kLcProd, 2);
  o.port.prog_fail = true;
  check((o.sw(kLcScrap) & C::kOtpError) && o.port.alerts["fatal_prog_error"] == 1,
        "OTP error: OTP_ERROR and fatal_prog_error");
  check(o.port.otp_state() == int(kLcProd) && o.core.state() == kLcPostTransition,
        "OTP error: OTP state unchanged, POST_TRANSITION");
  // Malformed target encoding.
  Dut f(1, 1);
  f.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  f.core.csr_write(C::kSw, C::kTarget, 2);  // TL0 not replicated 6x
  f.core.csr_write(C::kSw, C::kCmd, 1);
  check(f.core.status() & kTErr, "a target that is not 6x replicated: TRANSITION_ERROR");
}

// The forward-only property, exhaustively and by attempt.
void test_forward_only() {
  // 1. The table never allows a transition whose encoding would clear a programmed bit, never
  //    allows RAW as target, and allows nothing from SCRAP.
  int legal = 0;
  for (uint32_t from = 0; from <= kLcScrap; ++from) {
    for (uint32_t to = 0; to <= kLcScrap; ++to) {
      if (C::rule(from, to) == C::kIllegal) continue;
      ++legal;
      bool fwd = true;
      for (int i = 0; i < 20; ++i) {
        fwd = fwd && (eg2_lc_state_word(from, i) & ~eg2_lc_state_word(to, i)) == 0;
      }
      check(fwd && from != to, "legal " + std::to_string(from) + " -> " + std::to_string(to) +
                                   " moves forward in the OTP encoding");
      check(to != kLcRaw, "RAW is never a target");
      check(from != kLcScrap, "nothing leaves SCRAP");
    }
  }
  check(legal > 0, "the table has legal transitions");
  // 2. Live: from PROD, every backward target is refused, OTP keeps PROD, the count goes up by one
  //    per attempt.
  Dut d(kLcProd, 1);
  uint32_t n = 1;
  for (uint32_t to = 0; to <= kLcScrap; ++to) {
    if (to == kLcProd || to == kLcRma || to == kLcScrap) continue;
    uint32_t st = d.sw(to);
    bool refused = (st & kTErr) && !(st & kOk);
    if (kBreak && to == kLcTestUnlocked0) refused = !refused;
    check(refused, "PROD -> " + std::to_string(to) + " refused");
    check(d.port.otp_state() == int(kLcProd), "OTP still PROD after the attempt at " +
                                                  std::to_string(to));
    check(d.port.otp_count() == int(++n), "the count went up by one");
    d.reboot();
    check(d.core.state() == kLcProd && d.core.count() == n, "PROD again after the reset");
  }
  // PROD -> RMA (token) -> back to PROD refused -> SCRAP -> nothing, not even SCRAP.
  check(d.sw(kLcRma, kRma) & kOk, "PROD -> RMA with the token");
  d.reboot();
  check(d.sw(kLcProd) & kTErr, "RMA -> PROD refused");
  d.reboot();
  check(d.sw(kLcDev) & kTErr, "RMA -> DEV refused");
  d.reboot();
  check(d.sw(kLcScrap) & kOk, "RMA -> SCRAP");
  d.reboot();
  check(d.core.state() == kLcScrap, "SCRAP after reset");
  Dut s(kLcScrap, 2);  // a fresh count: 21 attempts stay below the limit of 24
  for (uint32_t to = 0; to <= kLcScrap; ++to) {
    int before = s.port.otp_count();
    check(s.sw(to, kRma) & kTErr, "SCRAP -> " + std::to_string(to) + " refused");
    check(s.port.otp_state() == int(kLcScrap) && s.port.otp_count() == before + 1,
          "SCRAP stays in OTP, the count still increases");
    s.reboot();
  }
  // Other backward steps.
  Dut e(kLcDev, 2);
  check(e.sw(kLcProd, kExit) & kTErr, "DEV -> PROD refused");
  Dut f(6, 2);
  check(f.sw(3, kUnlock) & kTErr, "TL2 -> TU1 (backwards) refused");
  Dut g(7, 2);
  check(g.sw(4) & kTErr, "TU3 -> TL1 (backwards) refused");
  Dut h(kLcProdEnd, 2);
  check(h.sw(kLcRma, kRma) & kTErr, "PROD_END -> RMA refused");
  // 3. The count limit: 23 -> one attempt -> 24 reads SCRAP and refuses with COUNT_ERROR, no write.
  Dut c(1, 23);
  check(c.sw(2) & kOk, "the 24th attempt still runs");
  c.reboot();
  check(c.core.state() == kLcScrap && c.core.count() == 24, "count 24: SCRAP");
  int programs = c.port.programs;
  check(c.sw(kLcScrap) & C::kTransitionCountError, "count 24: TRANSITION_COUNT_ERROR");
  check(c.port.programs == programs, "no OTP write at the limit");
}

void test_volatile_and_escalation() {
  Dut d(kLcRaw, 0);
  d.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  d.core.csr_write(C::kSw, C::kCtrl, 2);
  check(d.core.csr_read(C::kSw, C::kCtrl) == 0, "VOLATILE_RAW_UNLOCK reads 0 when not available");
  Dut v(kLcRaw, 0);
  v.core.config().volatile_raw_unlock_en = true;
  v.core.config().raw_unlock_hash = C::hash_token(kRaw);
  v.reboot();
  v.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  v.core.csr_write(C::kSw, C::kCtrl, 2);
  check(v.core.csr_read(C::kSw, C::kCtrl) == 2, "VOLATILE_RAW_UNLOCK reads back 1 when available");
  check(v.sw(1, kRaw) & kTokErr, "volatile unlock with the unhashed token: TOKEN_ERROR (no KMAC)");
  check(v.core.csr_read(C::kSw, C::kTransRegwen) == 0, "the error is terminal");
  v.reboot();
  v.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  v.core.csr_write(C::kSw, C::kCtrl, 2);
  int programs = v.port.programs;
  check(v.sw(1, C::hash_token(kRaw)) & kOk, "volatile unlock with the hashed token");
  check(v.core.state() == 1 && v.core.cpu_en() && v.port.programs == programs &&
            v.core.csr_read(C::kSw, C::kLcCnt) == 0,
        "volatile: TEST_UNLOCKED0 at once, no OTP write, count unchanged, CPU on");
  check(v.core.csr_read(C::kSw, C::kTransRegwen) == 1, "volatile: further commands accepted");
  v.core.csr_write(C::kSw, C::kCtrl, 0, 2);
  check(v.sw(2) & kOk, "after the volatile unlock, TU0 -> TL0 programs OTP over RAW");
  check(v.port.otp_state() == 2 && v.port.otp_count() == 1, "OTP TL0, count 1");
  // External clock.
  Dut x(1, 1);
  x.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  x.core.csr_write(C::kSw, C::kCtrl, 1);
  check((x.core.status() & C::kExtClockSwitched) && x.core.clk_byp_req(),
        "EXT_CLOCK_EN in TEST_UNLOCKED: CLK_BYP_REQ, EXT_CLOCK_SWITCHED");
  Dut y(kLcProd, 1);
  y.core.csr_write(C::kSw, C::kClaim, C::kMuBi8True);
  y.core.csr_write(C::kSw, C::kCtrl, 1);
  check(!(y.core.status() & C::kExtClockSwitched) && !y.core.clk_byp_req(),
        "EXT_CLOCK_EN in PROD is ignored");
  // Escalation.
  Dut e(kLcDev, 3);
  e.core.escalate();
  check(e.core.state() == kLcEscalate && e.core.escalate_en() && !e.core.cpu_en() &&
            !e.core.hw_debug_en() && !e.core.keymgr_en(),
        "escalation: ESCALATE, ESCALATE_EN, every function off");
  programs = e.port.programs;
  e.core.tap_transition(eg2_lc_rep5(kLcRma), kRma, false);
  check(e.port.programs == programs, "escalation: no transition, no OTP write");
  e.reboot();
  check(e.core.state() == kLcDev, "escalation clears with the reset");
}

}  // namespace

int main() {
  test_decode();
  test_signals();
  test_mutex();
  test_transitions();
  test_forward_only();
  test_volatile_and_escalation();
  std::printf("lc_ctrl core self-test: %d checks, %d failed%s\n", checks, fails,
              kBreak ? " (EG2_LC_TEST_BREAK: must fail)" : "");
  return fails ? 1 : 0;
}
