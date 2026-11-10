// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the alert handler model through its DPI functions (no simulator). From the eg2_model
// directory (the eg2_model Makefile has no target for it yet):
//   g++ -std=c++17 -O1 -I. dpi/eg2_alert_dpi.cc dpi/eg2_alert_dpi_test.cc -o /tmp/eg2_alert_test
//   /tmp/eg2_alert_test                          (must pass)
//   EG2_ALERT_TEST_BREAK=1 /tmp/eg2_alert_test   (inverts one expectation: must fail)
// Every expectation is from alert_handler's documents (theory_of_operation.md "Alert Classification
// and Interrupts", "Escalation Mechanisms", "Programmable Escalation Protocol", "Low-power
// Management of Alert Channels"; programmers_guide.md; alert_handler.hjson's register and field
// descriptions; util/reggen/README.md "Shadow Registers").

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../eg2_model_regs.h"

extern "C" {
void *eg2_alert_create(unsigned int nalerts);
void eg2_alert_release(void *h);
void eg2_alert_reset(void *h, unsigned long long cyc);
void eg2_alert_time(void *h, unsigned long long cyc);
void eg2_alert_csr_write(void *h, unsigned int addr, unsigned int data, unsigned int be_mask);
void eg2_alert_csr_read(void *h, unsigned int addr, int quiet, unsigned int *exp,
                        unsigned int *mask, unsigned int *tol);
unsigned int eg2_alert_csr_value(void *h, unsigned int addr);
void eg2_alert_alert(void *h, unsigned int id);
void eg2_alert_local(void *h, unsigned int idx);
void eg2_alert_lpg(void *h, unsigned int g, int low_power);
void eg2_alert_lpg_map(void *h, unsigned int alert, int g);
int eg2_alert_esc(void *h, unsigned int e);
unsigned int eg2_alert_intr_state(void *h);
unsigned int eg2_alert_class_state(void *h, unsigned int k);
unsigned int eg2_alert_dump_word(void *h, unsigned int i);
int eg2_alert_dump_latched(void *h);
}

namespace {

int failures = 0, checks = 0;
bool brk = false;
void *h;
unsigned long long now = 0;

void check(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

unsigned off(const char *name) {
  for (const eg2_regs::Block &b : eg2_regs::kBlocks) {
    if (std::strcmp(b.ip, "alert_handler") != 0) continue;
    for (unsigned i = 0; i < b.nregs; ++i) {
      if (std::strcmp(b.regs[i].name, name) == 0) return b.regs[i].offset;
    }
  }
  std::printf("no register %s\n", name);
  std::exit(2);
}
void wr(const char *name, unsigned v) { eg2_alert_csr_write(h, off(name), v, 0xffffffffu); }
// A shadowed register: two equal writes (reggen "Shadow Registers").
void wr2(const char *name, unsigned v) {
  wr(name, v);
  wr(name, v);
}
unsigned rd(const char *name) {
  unsigned e, m, t;
  eg2_alert_csr_read(h, off(name), 1, &e, &m, &t);
  return e;
}
void tick(unsigned long long n) {
  now += n;
  eg2_alert_time(h, now);
}

// CLASSx_CTRL_SHADOWED: EN bit 0, LOCK bit 1, EN_E0..3 bits 2..5, MAP_E0..3 2 bits each from 6.
unsigned ctrl(bool lock, unsigned en_e, const unsigned map[4]) {
  unsigned v = 1u | (lock ? 2u : 0u) | (en_e << 2);
  for (unsigned e = 0; e < 4; ++e) v |= (map[e] & 3) << (6 + 2 * e);
  return v;
}

}  // namespace

int main() {
  brk = std::getenv("EG2_ALERT_TEST_BREAK") != nullptr;
  h = eg2_alert_create(69);
  eg2_alert_reset(h, 0);

  // ---- Classification and accumulation (threshold 1: the second alert escalates) ----
  wr2("ALERT_EN_SHADOWED_3", 1);
  wr2("ALERT_CLASS_SHADOWED_3", 0);  // class A
  const unsigned map[4] = {0, 1, 2, 3};
  wr2("CLASSA_ACCUM_THRESH_SHADOWED", 1);
  for (unsigned ph = 0; ph < 4; ++ph) {
    wr2((std::string("CLASSA_PHASE") + char('0' + ph) + "_CYC_SHADOWED").c_str(), 10 * (ph + 1));
  }
  wr("INTR_ENABLE", 1);
  wr2("CLASSA_CTRL_SHADOWED", ctrl(true, 0xf, map));
  check(rd("CLASSA_CTRL_SHADOWED") == ctrl(true, 0xf, map), "shadowed CTRL takes the second write");
  eg2_alert_alert(h, 3);
  check(rd("ALERT_CAUSE_3") == 1, "an enabled alert sets its cause");
  check((eg2_alert_intr_state(h) & 1) == 1, "the class A interrupt fires");
  check(rd("CLASSA_ACCUM_CNT") == 1, "the first alert counts 1");
  check(eg2_alert_class_state(h, 0) == 0, "below the threshold: no escalation");
  tick(5);
  eg2_alert_alert(h, 3);
  check(rd("CLASSA_ACCUM_CNT") == 2, "the second alert counts 2");
  check(eg2_alert_class_state(h, 0) == 4, "the count reached the threshold: Phase0");
  check(eg2_alert_esc(h, 0) == (brk ? 0 : 1), "escalation 0 (MAP_E0 0) in Phase0");
  check(eg2_alert_esc(h, 1) == 0, "escalation 1 (MAP_E1 1) not in Phase0");
  check(rd("CLASSA_CLR_REGWEN") == 0, "CTRL.LOCK clears CLR_REGWEN on escalation");

  // ---- The phases advance by their cycle counts, then Terminal ----
  tick(10);
  check(eg2_alert_class_state(h, 0) == 5 && eg2_alert_esc(h, 1) == 1, "Phase1 after 10 cycles");
  tick(20);
  check(eg2_alert_class_state(h, 0) == 6 && eg2_alert_esc(h, 2) == 1, "Phase2 after 20 more");
  tick(30);
  check(eg2_alert_class_state(h, 0) == 7 && eg2_alert_esc(h, 3) == 1, "Phase3 after 30 more");
  tick(40);
  check(eg2_alert_class_state(h, 0) == 3, "Terminal after Phase3");
  for (unsigned e = 0; e < 4; ++e) check(eg2_alert_esc(h, e) == 0, "no escalation in Terminal");

  // ---- A locked class cannot be cleared ----
  wr2("CLASSA_CLR_SHADOWED", 1);
  check(eg2_alert_class_state(h, 0) == 3, "CLR while CLR_REGWEN is 0: no effect");

  // ---- Interrupt timeout escalates class B ----
  eg2_alert_reset(h, now);
  wr2("ALERT_EN_SHADOWED_5", 1);
  wr2("ALERT_CLASS_SHADOWED_5", 1);  // class B
  wr2("CLASSB_ACCUM_THRESH_SHADOWED", 100);
  wr2("CLASSB_TIMEOUT_CYC_SHADOWED", 50);
  wr("INTR_ENABLE", 2);
  wr2("CLASSB_CTRL_SHADOWED", ctrl(false, 0x1, map));
  eg2_alert_alert(h, 5);
  check(eg2_alert_class_state(h, 1) == 1, "an unhandled class B interrupt starts Timeout");
  tick(49);
  check(eg2_alert_class_state(h, 1) == 1, "still Timeout before TIMEOUT_CYC");
  tick(1);
  check(eg2_alert_class_state(h, 1) == 4, "the timeout reached: Phase0");
  // The interrupt still pending restarts the timeout after a clear; the handler acknowledges the
  // interrupt first, then clears the class.
  wr2("CLASSB_CLR_SHADOWED", 1);
  check(eg2_alert_class_state(h, 1) == 1 && rd("CLASSB_ACCUM_CNT") == 0,
        "CLR with the interrupt still pending: the count is cleared, Timeout again");
  wr("INTR_STATE", 2);
  check(eg2_alert_class_state(h, 1) == 0, "the interrupt acknowledged: Idle");
  wr2("CLASSB_CLR_SHADOWED", 1);
  check(eg2_alert_class_state(h, 1) == 0 && rd("CLASSB_ACCUM_CNT") == 0,
        "CLR (unlocked, interrupt handled) leaves Idle and a zero count");

  // ---- Shadow update error: two different writes ----
  wr("CLASSC_ACCUM_THRESH_SHADOWED", 7);
  wr("CLASSC_ACCUM_THRESH_SHADOWED", 8);
  check(rd("LOC_ALERT_CAUSE_5") == 0, "the local alert needs its enable");
  check(rd("CLASSC_ACCUM_THRESH_SHADOWED") == 0, "a mismatched pair leaves the register");
  wr2("LOC_ALERT_EN_SHADOWED_5", 1);
  wr("CLASSC_ACCUM_THRESH_SHADOWED", 7);
  wr("CLASSC_ACCUM_THRESH_SHADOWED", 8);
  check(rd("LOC_ALERT_CAUSE_5") == 1, "shadow update error: local alert 5");

  // ---- Low-power group: alerts of a group in low power are ignored ----
  eg2_alert_reset(h, now);
  eg2_alert_lpg_map(h, 9, 2);
  wr2("ALERT_EN_SHADOWED_9", 1);
  eg2_alert_lpg(h, 2, 1);
  eg2_alert_alert(h, 9);
  check(rd("ALERT_CAUSE_9") == 0, "an alert of a low-power group is ignored");
  eg2_alert_lpg(h, 2, 0);
  eg2_alert_alert(h, 9);
  check(rd("ALERT_CAUSE_9") == 1, "the same alert counts once the group is up");

  // ---- Crash dump latches at the trigger phase ----
  eg2_alert_reset(h, now);
  wr2("ALERT_EN_SHADOWED_1", 1);
  wr2("CLASSA_CRASHDUMP_TRIGGER_SHADOWED", 1);  // Phase1
  wr2("CLASSA_PHASE0_CYC_SHADOWED", 5);
  wr2("CLASSA_CTRL_SHADOWED", ctrl(false, 0x1, map));
  eg2_alert_alert(h, 1);  // threshold 0: the first alert escalates
  check(eg2_alert_dump_latched(h) == 0, "Phase0: the dump is not latched yet");
  tick(5);
  check(eg2_alert_dump_latched(h) == 1, "Phase1 (the trigger phase): the dump latches");
  unsigned w0 = eg2_alert_dump_word(h, 0);
  tick(100);
  check(eg2_alert_dump_word(h, 0) == w0, "the latched dump holds");

  // ---- Triggers of one class in the same cycle count once ("all triggers per class are ORd") ----
  eg2_alert_reset(h, now);
  wr2("ALERT_EN_SHADOWED_3", 1);
  wr2("ALERT_EN_SHADOWED_4", 1);
  wr2("CLASSA_ACCUM_THRESH_SHADOWED", 100);
  wr2("CLASSA_CTRL_SHADOWED", ctrl(false, 0x1, map));
  eg2_alert_alert(h, 3);
  eg2_alert_alert(h, 4);
  check(rd("CLASSA_ACCUM_CNT") == 1, "two class A alerts in one cycle count once");
  tick(1);
  eg2_alert_alert(h, 4);
  check(rd("CLASSA_ACCUM_CNT") == 2, "an alert in the next cycle counts again");
  check(rd("ALERT_CAUSE_3") == 1 && rd("ALERT_CAUSE_4") == 1, "both causes are recorded");

  // ---- Write-only and rw1c ----
  wr("INTR_STATE", 1);
  check((eg2_alert_intr_state(h) & 1) == 0, "INTR_STATE is rw1c");
  wr("INTR_TEST", 8);
  check((eg2_alert_intr_state(h) & 8) == 8, "INTR_TEST sets INTR_STATE");

  eg2_alert_release(h);
  std::printf("eg2_alert_test: %d checks, %d failures%s\n", checks, failures,
              brk ? " (EG2_ALERT_TEST_BREAK: must fail)" : "");
  return failures ? 1 : 0;
}
