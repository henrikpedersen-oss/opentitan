// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The SRAM controller core's self-test (ip/eg2_sram_ctrl_core.h): each documented behaviour step
// by step, on the four top_earlgrey instances, with the meta SRAM's own checks (zero INIT, key
// renewal, escalation, 38 KiB depth, no address scrambling). EG2_SRAM_TEST_BREAK=1 expects one
// word of the meta SRAM to be non-zero after its zero INIT: the test must then fail.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror dpi/eg2_sram_ctrl_core_test.cc -o /tmp/sram_core_test
//   /tmp/sram_core_test && ! EG2_SRAM_TEST_BREAK=1 /tmp/sram_core_test > /dev/null

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../ip/eg2_sram_ctrl_core.h"

namespace {

using C = Eg2SramCtrlCore;
int fails = 0, checks = 0;
const bool kBreak = std::getenv("EG2_SRAM_TEST_BREAK") != nullptr;

void check(bool ok, const std::string &what) {
  ++checks;
  if (!ok) {
    ++fails;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// A port that counts and answers key requests at once (eg2sim) or later (answer = false).
struct Port : Eg2SramCtrlPort {
  C *core = nullptr;
  bool answer = true, seed = true;
  int requests = 0, fatals = 0, tests = 0;
  void key_request() override {
    ++requests;
    if (answer && core) core->key_response(seed);
  }
  void fatal_alert() override { ++fatals; }
  void alert_test() override { ++tests; }
};

struct Dut {
  Port port;
  C core;
  explicit Dut(const std::string &inst) : core(C::instance(inst), &port) { port.core = &core; }
  uint32_t status() { return core.csr_read(C::kStatus); }
};

bool all_words(C &c, uint32_t value, C::ByteState st) {
  for (uint32_t off = 0; off < c.params().bytes; off += 4) {
    C::Read r = c.mem_read(off);
    if (r.acc != C::Access::kOk || c.byte_state(off) != st) return false;
    if (st == C::kKnown && r.data != value) return false;
  }
  return true;
}
bool any_garbage(C &c, uint32_t lo, uint32_t hi) {
  for (uint32_t off = lo; off < hi; off += 4) {
    if (c.mem_read(off).garbage) return true;
  }
  return false;
}

void reset_values() {
  Dut d("sram_ctrl_main");
  C &c = d.core;
  check(c.csr_read(C::kAlertTest) == 0x80000000u, "ALERT_TEST resets to 0x80000000");
  check(d.status() == 0, "STATUS resets to 0");
  check(c.csr_read(C::kExecRegwen) == 1 && c.csr_read(C::kCtrlRegwen) == 1 &&
            c.csr_read(C::kReadbackRegwen) == 1,
        "the REGWENs reset to 1");
  check(c.csr_read(C::kExec) == C::kMuBi4False && c.csr_read(C::kReadback) == C::kMuBi4False &&
            c.csr_read(C::kScrKeyRotated) == C::kMuBi4False,
        "EXEC, READBACK, SCR_KEY_ROTATED reset to MuBi4False (0x9)");
  check(c.csr_read(C::kCtrl) == 0, "CTRL is write-only");
  check(c.mem_read(0).unknown == 0xffffffffu, "power-up contents are not predicted");
}

void depth_and_range() {
  Dut m("sram_ctrl_meta");
  C &c = m.core;
  check(c.params().bytes == 38 * 1024 && c.words() == 9728, "meta: 9728 words, 38 KiB");
  check(c.decoded_bytes() == 0x10000, "meta: decoded range 64 KiB (the next power of two)");
  check(c.mem_write(0x97fc, 0x11223344) == C::Access::kOk && c.mem_read(0x97fc).data == 0x11223344,
        "meta: the last word is there");
  check(c.mem_write(0x9800, 1) == C::Access::kError && c.mem_read(0x9800).acc == C::Access::kError &&
            c.mem_read(0xfffc).acc == C::Access::kError,
        "meta: beyond 38 KiB inside the decoded range: a TL-UL error");
  check(c.physical_word(0) == 0 && c.physical_word(9727) == 9727 && c.physical_word(9728) == -1,
        "meta: NumAddrScrRounds 0, logical word = physical word");
  Dut mn("sram_ctrl_main");
  check(mn.core.decoded_bytes() == 0x20000 && mn.core.physical_word(5) == -1,
        "main: 128 KiB decoded as it is, address scrambling on (map not modelled)");
  check(C::instance("sram_ctrl_sec").bytes == 0x10000 && C::instance("sram_ctrl_ret").bytes == 0x2000,
        "sec 64 KiB, ret 8 KiB");
  // Out of range answers even while a key request is pending (ToO).
  m.port.answer = false;
  c.csr_write(C::kCtrl, 1);
  check(c.mem_read(0).acc == C::Access::kStall && c.mem_read(0x9800).acc == C::Access::kError,
        "pending key: in range stalls, out of range answers an error");
}

void meta_zero_init() {
  Dut m("sram_ctrl_meta");
  C &c = m.core;
  c.csr_write(C::kCtrl, 3);  // RENEW_SCR_KEY | INIT, as boot code must
  check(m.port.requests == 1, "RENEW|INIT: one key request");
  uint32_t s = m.status();
  check((s & C::kScrKeyValid) && (s & C::kScrKeySeedValid) && (s & C::kInitDone),
        "meta: key valid, seed valid, INIT done");
  check(c.csr_read(C::kScrKeyRotated) == C::kMuBi4True, "SCR_KEY_ROTATED = MuBi4True");
  check(all_words(c, 0, C::kKnown), "meta: every word reads 0 after INIT (SecZeroInit)");
  // The meta SRAM map (eg2_mpact_cpu.cc): revocation bitmap 0x0..0xc00, RRAM tags 0xc00..0x8c00,
  // main+sec SRAM tags 0x8c00..0x9800.
  bool zero = true;
  for (uint32_t off : {0x0u, 0xbfcu, 0xc00u, 0x8bfcu, 0x8c00u, 0x97fcu}) {
    zero = zero && c.mem_read(off).data == 0 && c.mem_read(off).unknown == 0;
  }
  if (kBreak) zero = zero && c.mem_read(0x8c00).data != 0;
  check(zero, "meta: bitmap and both tag regions are 0");

  // Key renewal without INIT: every tag and bitmap word is garbage.
  c.mem_write(0x8c00, 0xff);
  c.csr_write(C::kCtrl, 1);
  check(m.port.requests == 2 && any_garbage(c, 0, 0xc00) && any_garbage(c, 0xc00, 0x8c00) &&
            any_garbage(c, 0x8c00, 0x9800) && c.mem_read(0x8c00).unknown == 0xffffffffu,
        "meta: a key renewal without INIT leaves bitmap and tags as garbage");
  check(m.status() & C::kInitDone, "INIT_DONE kept over a renewal (the core's guess)");
  c.csr_write(C::kCtrl, 2);
  check(all_words(c, 0, C::kKnown), "meta: INIT alone zeroes again");

  // A reset after a renewal: the predefined key again, so the zeros are garbage until INIT.
  c.reset();
  check(m.status() == 0 && any_garbage(c, 0, 0x9800) && c.mem_read(0).garbage,
        "meta: after a reset following a renewal the contents are garbage");
  c.csr_write(C::kCtrl, 3);
  check(all_words(c, 0, C::kKnown), "meta: RENEW|INIT after the reset zeroes again");

  // Escalation: blocked, key back to the default, contents garbage after the reset.
  c.set_lc_escalate_en(C::kLcOn);
  check((m.status() & C::kEscalated) && c.mem_read(0).acc == C::Access::kBlocked &&
            c.mem_write(0, 1) == C::Access::kBlocked,
        "meta: escalated, every access blocked");
  c.reset();
  check(c.mem_read(0).acc == C::Access::kOk && c.mem_read(0).garbage,
        "meta: after the escalation and reset the tags are garbage (renewed key lost)");
}

void main_init_and_key() {
  Dut d("sram_ctrl_main");
  C &c = d.core;
  c.csr_write(C::kCtrl, 2);  // INIT under the predefined key
  check(d.port.requests == 0 && (d.status() & C::kInitDone), "INIT alone: done, no key request");
  check(all_words(c, 0, C::kValid), "main: LFSR INIT, every word valid but not predicted");
  bool nonzero = false;
  for (uint32_t off = 0; off < 64; off += 4) nonzero = nonzero || c.mem_read(off).data != 0;
  check(nonzero, "main: LFSR INIT is not all zeros");

  // Data under the predefined key survives a reset (no renewal ever).
  c.mem_write(0x100, 0xcafef00d);
  c.reset();
  check(c.mem_read(0x100).data == 0xcafef00d && c.mem_read(0x100).unknown == 0,
        "predefined key: contents survive a reset");

  // Two-phase key request: the memory stalls until OTP answers.
  d.port.answer = false;
  c.csr_write(C::kCtrl, 1);
  check(c.key_pending() && !(d.status() & C::kScrKeyValid) &&
            c.mem_read(0x100).acc == C::Access::kStall,
        "key pending: SCR_KEY_VALID 0, accesses stall");
  c.csr_write(C::kCtrl, 1);
  c.csr_write(C::kCtrl, 2);
  check(d.port.requests == 1, "RENEW while a key request is pending: no effect");
  check(c.init_pending() && !(d.status() & C::kInitDone), "INIT waits for the key, INIT_DONE 0");
  uint32_t u = 0;
  c.csr_read(C::kStatus, &u);
  check(u & C::kInitDone, "INIT_DONE while an INIT waits behind a key: not predicted");
  c.key_response(false);
  check((d.status() & C::kScrKeyValid) && !(d.status() & C::kScrKeySeedValid) &&
            (d.status() & C::kInitDone) && !c.init_pending(),
        "key without a provisioned seed: SEED_VALID 0; the waiting INIT runs");
  check(c.mem_read(0x100).acc == C::Access::kOk && !c.mem_read(0x100).garbage,
        "after the key and the INIT: valid words");

  // A renewal on its own garbles the contents.
  c.mem_write(0x200, 0x12345678);
  d.port.answer = true;
  c.csr_write(C::kCtrl, 1);
  check(c.mem_read(0x200).garbage && c.mem_read(0x200).data != 0x12345678,
        "a renewal makes old contents unreadable");
  // Sub-word write on a garbage word: the written byte known, the others open (not garbage).
  c.mem_write(0x200, 0xaa, 0x1);
  C::Read r = c.mem_read(0x200);
  check((r.data & 0xff) == 0xaa && r.unknown == 0xffffff00u && !r.garbage,
        "sub-word write (read-modify-write) on a garbage word");

  // SCR_KEY_ROTATED: write MuBi4True to clear.
  c.csr_write(C::kScrKeyRotated, C::kMuBi4True);
  check(c.csr_read(C::kScrKeyRotated) == C::kMuBi4False, "SCR_KEY_ROTATED cleared by MuBi4True");
}

void regwen() {
  Dut d("sram_ctrl_main");
  C &c = d.core;
  c.csr_write(C::kCtrlRegwen, 0);
  c.csr_write(C::kCtrl, 3);
  check(d.port.requests == 0 && c.inits() == 0 && d.status() == 0,
        "CTRL_REGWEN cleared: CTRL has no effect");
  c.csr_write(C::kCtrlRegwen, 1);
  check(c.csr_read(C::kCtrlRegwen) == 0, "CTRL_REGWEN is rw0c");
  c.csr_write(C::kExec, C::kMuBi4True);
  c.csr_write(C::kExecRegwen, 0);
  c.csr_write(C::kExec, C::kMuBi4False);
  check(c.csr_read(C::kExec) == C::kMuBi4True, "EXEC_REGWEN cleared: EXEC kept");
  c.csr_write(C::kReadbackRegwen, 0);
  c.csr_write(C::kReadback, C::kMuBi4True);
  check(c.csr_read(C::kReadback) == C::kMuBi4False && !c.readback_enabled(),
        "READBACK_REGWEN cleared: READBACK kept");
  c.csr_write(C::kAlertTest, 0x80000001u);
  check(d.port.tests == 1 && c.csr_read(C::kAlertTest) == 0x80000000u,
        "ALERT_TEST fatal_error with bit 31 kept: one alert event, regwen still 1");
  c.csr_write(C::kAlertTest, 1);  // bit 31 written 0 (rw0c)
  check(d.port.tests == 2 && c.csr_read(C::kAlertTest) == 0,
        "ALERT_TEST 0x1: the event, then the regwen is cleared");
  c.csr_write(C::kAlertTest, 0x80000001u);
  check(d.port.tests == 2, "ALERT_TEST regwen cleared: no more events");
}

void exec_gating() {
  Dut d("sram_ctrl_main");
  C &c = d.core;
  c.mem_write(0, 0x13);
  check(!c.exec_enabled() && c.mem_read(0, true).acc == C::Access::kRefused,
        "reset: OTP not MuBi8True, HW_DEBUG_EN Off: no execution");
  c.set_lc_hw_debug_en(C::kLcOn);
  check(c.exec_enabled() && c.mem_read(0, true).acc == C::Access::kOk,
        "OTP not MuBi8True, HW_DEBUG_EN On: execution");
  c.set_lc_hw_debug_en(0x0);
  check(!c.exec_enabled(), "HW_DEBUG_EN invalid (not On): no execution");
  c.set_otp_en_sram_ifetch(C::kMuBi8True);
  c.set_lc_hw_debug_en(C::kLcOn);
  check(!c.exec_enabled(), "OTP MuBi8True, EXEC MuBi4False: no execution (HW_DEBUG_EN irrelevant)");
  c.csr_write(C::kExec, C::kMuBi4True);
  check(c.exec_enabled(), "OTP MuBi8True, EXEC MuBi4True: execution");
  c.csr_write(C::kExec, 0xa);
  check(!c.exec_enabled(), "EXEC not MuBi4True: no execution");
  for (const char *inst : {"sram_ctrl_meta", "sram_ctrl_ret"}) {
    Dut n(inst);
    n.core.set_otp_en_sram_ifetch(C::kMuBi8True);
    n.core.csr_write(C::kExec, C::kMuBi4True);
    n.core.set_lc_hw_debug_en(C::kLcOn);
    bool no = !n.core.exec_enabled();
    n.core.set_otp_en_sram_ifetch(0x69);
    check(no && !n.core.exec_enabled(), std::string(inst) + ": InstrExec 0, never execution");
  }
  c.csr_write(C::kExec, C::kMuBi4True);
  c.set_lc_escalate_en(C::kLcOn);
  check(c.mem_read(0, true).acc == C::Access::kBlocked, "escalated: fetches blocked too");
}

void escalation_and_faults() {
  {
    Dut d("sram_ctrl_main");
    C &c = d.core;
    c.mem_write(0x40, 0x55aa55aa);
    c.set_lc_escalate_en(C::kLcOff);
    check(!c.escalated(), "lc_escalate_en Off: no escalation");
    c.set_lc_escalate_en(0x0);  // invalid encoding: not Off, so On
    check(c.escalated() && c.blocked() && c.mem_read(0x40).acc == C::Access::kBlocked,
          "lc_escalate_en invalid (not Off): escalated, memory blocked");
    c.set_lc_escalate_en(C::kLcOff);
    check(c.escalated(), "ESCALATED is terminal");
    uint32_t u = 0;
    c.csr_read(C::kStatus, &u);
    check((u & C::kScrKeyValid) && !(u & C::kEscalated), "after escalation the key bits are open");
    c.csr_write(C::kCtrl, 3);
    check(d.port.requests == 0 && c.inits() == 0, "CTRL after an escalation: no key request, no INIT");
    c.reset();
    check(!c.escalated() && c.mem_read(0x40).data == 0x55aa55aa && !c.mem_read(0x40).garbage,
          "no renewal before the escalation: the default key again, contents readable after reset");
  }
  {
    Dut d("sram_ctrl_main");
    C &c = d.core;
    c.csr_write(C::kCtrl, 1);
    c.mem_write(0x40, 0x55aa55aa);
    c.set_lc_escalate_en(C::kLcOn);
    check(c.csr_read(C::kScrKeyRotated) == C::kMuBi4False, "escalation clears SCR_KEY_ROTATED");
    c.reset();
    check(c.mem_read(0x40).garbage, "renewed key lost on escalation: contents garbage");
  }
  {
    Dut d("sram_ctrl_sec");
    C &c = d.core;
    c.bus_integrity_error();
    check((d.status() & C::kBusIntegError) && c.fatal_alert() && d.port.fatals == 1 &&
              c.mem_read(0).acc == C::Access::kBlocked && !c.escalated(),
          "bus integrity error: STATUS bit, fatal alert, memory blocked (local escalation)");
    uint32_t u = 0;
    c.csr_read(C::kStatus, &u);
    check((u & C::kEscalated) && !(u & C::kBusIntegError),
          "local escalation: ESCALATED not predicted, BUS_INTEG_ERROR predicted");
    c.reset();
    check(d.status() == 0 && !c.fatal_alert() && c.mem_read(0).acc == C::Access::kOk,
          "reset clears the bus integrity error");
    c.init_counter_error();
    check((d.status() & C::kInitError) && c.blocked(), "init counter error: local escalation");
  }
  {
    Dut d("sram_ctrl_ret");
    C &c = d.core;
    c.ram_encoding_error();
    check((d.status() & C::kSramAlert) && c.fatal_alert() && !c.blocked(),
          "RAM encoding error: SRAM_ALERT and fatal alert, not documented to block");
    c.reset();
    c.readback_mismatch();
    check(!(d.status() & C::kReadbackError) && !c.fatal_alert(),
          "readback check off (MuBi4False): a mismatch does nothing");
    c.csr_write(C::kReadback, 0x3);  // any value other than MuBi4False enables it
    c.readback_mismatch();
    check((d.status() & C::kReadbackError) && c.fatal_alert() && d.port.fatals == 2,
          "readback check on: READBACK_ERROR and fatal alert");
  }
}

}  // namespace

int main() {
  reset_values();
  depth_and_range();
  meta_zero_init();
  main_init_and_key();
  regwen();
  exec_gating();
  escalation_and_faults();
  std::printf("sram_ctrl model self-test: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
