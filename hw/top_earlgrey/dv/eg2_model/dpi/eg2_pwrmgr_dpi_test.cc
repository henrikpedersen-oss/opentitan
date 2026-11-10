// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the power manager model through its DPI functions (no simulator):
//   make -C hw/top_earlgrey/dv/eg2_model pwrmgr_test
// EG2_PWR_TEST_BREAK=1 inverts one expectation: the run must then fail.

#include <cstdio>
#include <cstdlib>

extern "C" {
void *eg2_pwr_create();
void eg2_pwr_release(void *h);
void eg2_pwr_reset(void *h);
void eg2_pwr_csr_write(void *h, unsigned int addr, unsigned int data, int quiet);
void eg2_pwr_csr_read(void *h, unsigned int addr, int quiet, unsigned int *lo, unsigned int *hi);
void eg2_pwr_intr(void *h, int quiet, unsigned int *lo, unsigned int *hi);
void eg2_pwr_inputs(void *h, unsigned int wake, unsigned int rst, int esc, int sleeping,
                    int sw_rst, int main_pok, int idle, int active, int pd_n_req);
void eg2_pwr_settled(void *h);
unsigned int eg2_pwr_csr_value(void *h, unsigned int addr);
unsigned int eg2_pwr_resets(void *h);
unsigned int eg2_pwr_entries(void *h);
int eg2_pwr_core_low_power(void *h);
}

namespace {

int failures = 0, checks = 0;

void check(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

enum : unsigned {
  kIntrState = 0x00, kIntrEnable = 0x04, kIntrTest = 0x08, kCtrlCfgRegwen = 0x10, kControl = 0x14,
  kCfgCdcSync = 0x18, kWakeupEnRegwen = 0x1c, kWakeupEn = 0x20, kWakeStatus = 0x24,
  kResetEnRegwen = 0x28, kResetEn = 0x2c, kResetStatus = 0x30, kEscResetStatus = 0x34,
  kCaptureDis = 0x38, kWakeInfo = 0x3c, kFaultStatus = 0x40,
};

void *h;

// Port levels the test drives.
struct Ports {
  unsigned wake = 0, rst = 0;
  int esc = 0, sleeping = 0, sw_rst = 0, main_pok = 1, idle = 1, active = 1, pd_n = 1;
} p;

void drive() {
  eg2_pwr_inputs(h, p.wake, p.rst, p.esc, p.sleeping, p.sw_rst, p.main_pok, p.idle, p.active,
                 p.pd_n);
}

// The exact value of a settled read; ~0u if the model does not predict every bit. During a
// low-power state the model's own value (the scoreboard leaves the hardware fields open there).
unsigned rd(unsigned a) {
  unsigned lo, hi;
  eg2_pwr_csr_read(h, a, 1, &lo, &hi);
  if (!p.active || eg2_pwr_core_low_power(h)) return eg2_pwr_csr_value(h, a);
  return lo == hi ? lo : ~0u;
}
void bounds(unsigned a, unsigned *lo, unsigned *hi, int quiet = 1) {
  eg2_pwr_csr_read(h, a, quiet, lo, hi);
}
void wr(unsigned a, unsigned d, int quiet = 1) { eg2_pwr_csr_write(h, a, d, quiet); }
void sync() {
  wr(kCfgCdcSync, 1);
  eg2_pwr_settled(h);
}

}  // namespace

int main() {
  bool brk = std::getenv("EG2_PWR_TEST_BREAK") != nullptr;
  unsigned lo, hi;
  h = eg2_pwr_create();
  drive();

  // Reset values (pwrmgr.hjson).
  check(rd(kCtrlCfgRegwen) == 1 && rd(kControl) == 0x180 && rd(kWakeupEnRegwen) == 1 &&
            rd(kResetEnRegwen) == 1 && rd(kWakeInfo) == 0 && rd(kFaultStatus) == 0,
        "reset values");
  check(rd(kIntrTest) == 0, "INTR_TEST reads 0");

  // INTR_TEST sets INTR_STATE; W1C clears it; the output follows INTR_ENABLE.
  wr(kIntrTest, 1);
  check(rd(kIntrState) == 1, "INTR_TEST sets INTR_STATE");
  eg2_pwr_intr(h, 1, &lo, &hi);
  check(lo == 0 && hi == 0, "interrupt output masked by INTR_ENABLE");
  wr(kIntrEnable, 1);
  eg2_pwr_intr(h, 1, &lo, &hi);
  check(lo == 1 && hi == 1, "interrupt output with INTR_ENABLE");
  wr(kIntrState, 1);
  check(rd(kIntrState) == 0, "INTR_STATE W1C");

  // WAKE_STATUS: the requests post enable mask; the enables count only after a CDC sync.
  p.wake = 0x05;
  drive();
  check(rd(kWakeStatus) == 0, "no wake status without enables");
  wr(kWakeupEn, 0x0f);
  bounds(kWakeStatus, &lo, &hi);
  check(lo == 0 && hi == 0x05, "WAKE_STATUS open before the CDC sync");
  sync();
  check(rd(kWakeStatus) == 0x05, "WAKE_STATUS after the CDC sync");
  check(rd(kCfgCdcSync) == 0, "CFG_CDC_SYNC self-clears");
  bounds(kWakeStatus, &lo, &hi, 0);
  check(lo == 0 && hi == 0xff, "WAKE_STATUS not predicted while the design settles");
  p.wake = 0;
  drive();
  check(rd(kWakeStatus) == 0, "WAKE_STATUS follows the request levels");

  // WAKEUP_EN_REGWEN is rw0c and locks WAKEUP_EN.
  wr(kWakeupEnRegwen, 1);
  check(rd(kWakeupEnRegwen) == 1, "WAKEUP_EN_REGWEN: writing 1 keeps it");
  wr(kWakeupEnRegwen, 0);
  wr(kWakeupEn, 0xff);
  check(rd(kWakeupEnRegwen) == 0 && rd(kWakeupEn) == 0x0f, "WAKEUP_EN locked");

  // Normal sleep: WFI with the hint locks CONTROL and clears the hint; a wake-up records its
  // reason and raises the interrupt; Active again unlocks CONTROL.
  wr(kWakeInfo, 0x3ff);
  wr(kControl, 0x181);  // hint, MAIN_PD_N 1
  sync();
  p.sleeping = 1;
  drive();
  check(eg2_pwr_entries(h) == 1, "WFI with the hint starts an entry");
  check(rd(kCtrlCfgRegwen) == 0 && rd(kControl) == 0x180, "entry: CONTROL locked, hint cleared");
  wr(kControl, 0x1f1);
  check(rd(kControl) == 0x180, "CONTROL write refused while locked");
  p.active = 0;
  drive();
  check(eg2_pwr_core_low_power(h) == 1, "in low power");
  p.wake = 0x02;
  drive();
  check(rd(kWakeInfo) == 0x02 && rd(kIntrState) == 1, "wake-up from normal sleep: reason, interrupt");
  check(eg2_pwr_resets(h) == 0, "no reset from normal sleep");
  p.active = 1;
  drive();
  p.sleeping = 0;
  drive();
  eg2_pwr_settled(h);
  check(rd(kCtrlCfgRegwen) == 1, "Active again: CONTROL unlocked");
  // Capture goes on until disabled: a later wake-up level may be captured.
  p.wake = 0x0a;
  drive();
  bounds(kWakeInfo, &lo, &hi);
  check(lo == 0x02 && hi == 0x0a, "capture continues: a new level may be captured");
  wr(kCaptureDis, 1);
  wr(kWakeInfo, 0x3ff);
  check(rd(kWakeInfo) == 0, "capture disabled, WAKE_INFO cleared");
  p.wake = 0;
  drive();
  wr(kIntrState, 1);

  // Deep sleep: the wake-up resets through a low-power exit.
  wr(kCaptureDis, 0);
  wr(kControl, 0x081);  // hint, MAIN_PD_N 0
  sync();
  p.sleeping = 1;
  drive();
  p.active = 0;
  p.pd_n = 0;
  drive();
  p.main_pok = 0;
  drive();
  check(rd(kFaultStatus) == 0, "power-ok low after the power-down request: no glitch");
  p.wake = 0x01;
  drive();
  check(eg2_pwr_resets(h) == 1 && rd(kWakeInfo) == 0x01 && rd(kIntrState) == 1,
        "deep-sleep wake-up: low-power exit reset, reason, interrupt");
  p.pd_n = 1;
  drive();
  p.main_pok = 1;
  drive();
  p.active = 1;
  p.sleeping = 0;
  p.wake = 0;
  drive();
  eg2_pwr_settled(h);
  wr(kCaptureDis, 1);
  wr(kWakeInfo, 0x3ff);
  wr(kIntrState, 1);

  // Abort: an idle input low throughout the entry.
  wr(kCaptureDis, 0);
  wr(kControl, 0x181);
  sync();
  p.idle = 0;
  drive();
  p.sleeping = 1;
  drive();
  eg2_pwr_settled(h);
  check(rd(kWakeInfo) == 0x200, "abort: WAKE_INFO.ABORT");
  bounds(kIntrState, &lo, &hi);
  check(lo == 0 && hi == 1, "interrupt on an abort not predicted");
  check(rd(kCtrlCfgRegwen) == 1 && eg2_pwr_core_low_power(h) == 0, "aborted: active, unlocked");
  p.idle = 1;
  p.sleeping = 0;
  drive();
  wr(kCaptureDis, 1);
  wr(kWakeInfo, 0x3ff);
  wr(kIntrState, 1);
  check(rd(kWakeInfo) == 0 && rd(kIntrState) == 0, "cleared after the abort");

  // Fall through: WFI drops during the entry.
  wr(kCaptureDis, 0);
  wr(kControl, 0x191);  // hint, CORE_CLK_EN
  sync();
  p.sleeping = 1;
  drive();
  p.sleeping = 0;
  drive();
  eg2_pwr_settled(h);
  bounds(kWakeInfo, &lo, &hi);
  check(lo == 0 && hi == 0x100, "fall through: FALL_THROUGH may be set, nothing else");
  check(rd(kCtrlCfgRegwen) == 1, "fall through: unlocked");
  wr(kCaptureDis, 1);
  wr(kWakeInfo, 0x3ff);
  wr(kIntrState, 1);

  // Reset requests: gated by RESET_EN, clear the hint, RESET_STATUS post enable mask.
  wr(kControl, 0x181);
  sync();
  p.rst = 0x1;
  drive();
  check(eg2_pwr_resets(h) == 1 && rd(kResetStatus) == 0 && rd(kControl) == 0x181,
        "reset request not enabled: ignored");
  p.rst = 0;
  drive();
  wr(kResetEn, 0x1);
  sync();
  p.rst = 0x1;
  drive();
  check(eg2_pwr_resets(h) == 2 && rd(kResetStatus) == 1 && rd(kControl) == 0x180,
        "enabled reset request: reset, status, hint cleared");
  p.rst = 0;
  drive();
  check(rd(kResetStatus) == 0, "RESET_STATUS follows the request");
  wr(kResetEnRegwen, 0);
  wr(kResetEn, 0x3);
  check(rd(kResetEn) == 1, "RESET_EN locked");

  // Software reset request.
  p.sw_rst = 1;
  drive();
  check(eg2_pwr_resets(h) == 3, "software reset request");
  p.sw_rst = 0;
  drive();

  // Escalation: ESCALATE_RESET_STATUS 1 while requested; open afterwards.
  p.esc = 1;
  drive();
  check(eg2_pwr_resets(h) == 4 && rd(kEscResetStatus) == 1, "escalation: reset, status 1");
  p.esc = 0;
  drive();
  bounds(kEscResetStatus, &lo, &hi);
  check(lo == 0 && hi == 1, "ESCALATE_RESET_STATUS after the request: not predicted");

  // Main power glitch: power-ok low while power is requested on.
  p.main_pok = 0;
  drive();
  check((rd(kFaultStatus) == 0x4) != brk && eg2_pwr_resets(h) == 5, "main power glitch");
  p.main_pok = 1;
  drive();
  check(rd(kFaultStatus) == 0x4, "MAIN_PD_GLITCH sticky");

  // A CONTROL write while the lock may be changing is not predicted until the next settled write.
  wr(kControl, 0x1f0, 0);
  bounds(kControl, &lo, &hi);
  check(lo == 0x180 && hi == 0x1f0, "unsettled CONTROL write: the new bits open");
  wr(kControl, 0x180);
  check(rd(kControl) == 0x180, "settled CONTROL write");

  // The bench's reset.
  eg2_pwr_reset(h);
  drive();
  check(rd(kFaultStatus) == 0 && rd(kResetEnRegwen) == 1 && eg2_pwr_resets(h) == 0, "POR");

  eg2_pwr_release(h);
  std::printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
