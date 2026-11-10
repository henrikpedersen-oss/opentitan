// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the reset manager reference model through its DPI functions (make rstmgr_test): the
// documented behaviour step by step (eg2_rstmgr_dpi.cc). EG2_RST_TEST_BREAK=1 corrupts the model's
// exact RESET_INFO predictions and dump checks: the test must then fail.

#include <cstdint>
#include <cstdio>

extern "C" {
void *eg2_rst_create();
void eg2_rst_release(void *h);
void eg2_rst_reset(void *h);
void eg2_rst_csr_write(void *h, uint32_t addr, uint32_t data);
void eg2_rst_csr_read(void *h, uint32_t addr, int quiet, uint32_t *lo, uint32_t *hi);
void eg2_rst_inputs(void *h, int por_ast, int por, int local, uint32_t lc, uint32_t sys,
                    uint32_t rstreqs, uint32_t cause, int scan);
void eg2_rst_set_dump(void *h, int alert, uint32_t n, uint32_t idx, uint32_t w);
int eg2_rst_dump_word_ok(void *h, int alert, uint32_t idx, uint32_t v, int quiet);
void eg2_rst_outputs(void *h, uint32_t *lc_lo, uint32_t *lc_hi, uint32_t *sys_lo,
                     uint32_t *sys_hi, uint32_t *sw_lo, uint32_t *sw_hi, uint32_t *req_lo,
                     uint32_t *req_hi);
int eg2_rst_recording(void *h);
}

namespace {
int fails = 0, checks = 0;
void check(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++fails;
    std::printf("FAIL %s\n", what);
  }
}
uint32_t rd(void *h, uint32_t a, uint32_t *hi_out = nullptr) {
  uint32_t lo, hi;
  eg2_rst_csr_read(h, a, 1, &lo, &hi);
  if (hi_out) *hi_out = hi;
  return lo;
}
bool exact(void *h, uint32_t a, uint32_t v) {
  uint32_t hi, lo = rd(h, a, &hi);
  return lo == v && hi == v;
}
// A reset by pwrmgr: request both trees (both domains), hold, release.
void reset(void *h, uint32_t cause, uint32_t rstreqs) {
  eg2_rst_inputs(h, 0, 0, 0, 3, 3, 0, cause, 0);
  eg2_rst_inputs(h, 0, 0, cause != 1, 3, 3, rstreqs, cause, 0);  // rst_ni: lc_io_div4 (always-on)
  eg2_rst_inputs(h, 0, 0, 0, 0, 3, 0, 0, 0);
  eg2_rst_inputs(h, 0, 0, 0, 0, 0, 0, 0, 0);
}
}  // namespace

int main() {
  void *h = eg2_rst_create();
  // Power-on reset, then the POR-time reset pwrmgr makes (category undefined), released.
  eg2_rst_inputs(h, 1, 1, 0, 0, 0, 0, 0, 0);
  check(!eg2_rst_recording(h), "recording silenced after a power-on reset");
  eg2_rst_inputs(h, 0, 0, 0, 3, 3, 0, 3, 0);
  check(exact(h, 0x08, 0x1), "RESET_INFO = POR during the first reset");
  eg2_rst_inputs(h, 0, 0, 0, 0, 0, 0, 0, 0);
  check(eg2_rst_recording(h), "recording allowed after the first processor reset release");
  check(exact(h, 0x08, 0x1), "RESET_INFO = POR alone");
  check(exact(h, 0x04, 0x9), "RESET_REQ resets to false");
  check(exact(h, 0x0c, 0x1) && exact(h, 0x1c, 0x1), "the REGWENs reset to 1");
  for (uint32_t i = 0; i < 10; ++i) {
    check(exact(h, 0x2c + 4 * i, 1) && exact(h, 0x54 + 4 * i, 1), "SW_RST reset values");
  }
  check(exact(h, 0x7c, 0), "ERR_CODE 0");
  { uint32_t h0, l0 = rd(h, 0x00, &h0); check(l0 == 0 && h0 == 0xffffffffu, "ALERT_TEST read not predicted (write-only)"); }

  // A software reset: RESET_REQ true, sw_rst_req_o follows; the reset records SW_RESET and clears
  // RESET_REQ when complete.
  eg2_rst_csr_write(h, 0x04, 0x6);
  uint32_t lc_lo, lc_hi, sys_lo, sys_hi, sw_lo, sw_hi, req_lo, req_hi;
  eg2_rst_outputs(h, &lc_lo, &lc_hi, &sys_lo, &sys_hi, &sw_lo, &sw_hi, &req_lo, &req_hi);
  check(req_lo == 1 && req_hi == 1, "sw_rst_req_o true while RESET_REQ is true");
  reset(h, 2, 0);
  check(exact(h, 0x04, 0x9), "RESET_REQ cleared upon completion of the reset");
  check(exact(h, 0x08, 0x5), "RESET_INFO = POR | SW_RESET");
  eg2_rst_csr_write(h, 0x08, 0xff);
  check(exact(h, 0x08, 0x0), "RESET_INFO cleared by software (rw1c)");

  // Hardware requests: sysrst_ctrl and the escalation.
  reset(h, 2, 0x09);
  check(exact(h, 0x08, (0x09u << 3)), "RESET_INFO.HW_REQ = rstreqs");
  // An NDM request under the hardware-request category: open.
  reset(h, 2, 0x10);
  uint32_t hi, lo = rd(h, 0x08, &hi);
  check(lo == (0x09u << 3) && (hi & 0x80), "NDM under a hardware request is open");
  eg2_rst_csr_write(h, 0x08, 0xff);
  // An NDM reset (pwrmgr triggers no reset): recorded.
  reset(h, 0, 0x10);
  check(exact(h, 0x08, 0x80), "NDM reset recorded with category none");
  eg2_rst_csr_write(h, 0x08, 0xff);

  // Low-power entry: LOW_POWER_EXIT; the EN bits are open afterwards.
  eg2_rst_csr_write(h, 0x10, 0x01);
  reset(h, 1, 0);
  check(exact(h, 0x08, 0x2), "RESET_INFO = LOW_POWER_EXIT");
  lo = rd(h, 0x10, &hi);
  check(lo == 0 && hi == 1, "ALERT_INFO_CTRL.EN open after a low-power entry reset");
  eg2_rst_csr_write(h, 0x08, 0xff);

  // Crash dumps: enable both, a hardware reset captures and clears EN.
  for (uint32_t i = 0; i < 9; ++i) eg2_rst_set_dump(h, 1, 9, i, 0xa0000000u | i);
  for (uint32_t i = 0; i < 8; ++i) eg2_rst_set_dump(h, 0, 8, i, 0xc0000000u | i);
  eg2_rst_csr_write(h, 0x10, 0x31);
  eg2_rst_csr_write(h, 0x20, 0x01);
  reset(h, 2, 0x02);
  check(exact(h, 0x10, 0x30), "ALERT_INFO_CTRL.EN cleared upon reset, INDEX kept");
  check(exact(h, 0x20, 0x00), "CPU_INFO_CTRL.EN cleared upon reset");
  check(exact(h, 0x14, 9) && exact(h, 0x24, 8), "CNT_AVAIL = the dumps' word counts");
  check(eg2_rst_dump_word_ok(h, 1, 3, 0xa0000003u, 1), "alert dump word 3");
  check(eg2_rst_dump_word_ok(h, 1, 3, 0xa0000005u, 1), "alert dump word 3 counted from the end");
  check(!eg2_rst_dump_word_ok(h, 1, 3, 0x12345678u, 1), "a wrong alert dump word is caught");
  check(eg2_rst_dump_word_ok(h, 0, 0, 0xc0000000u, 1), "cpu dump word 0");
  // Dumps change, capture disabled: the captured words stay.
  for (uint32_t i = 0; i < 9; ++i) eg2_rst_set_dump(h, 1, 9, i, 0xb0000000u | i);
  reset(h, 2, 0x02);
  check(eg2_rst_dump_word_ok(h, 1, 1, 0xa0000001u, 1), "no capture with EN 0");

  // REGWEN locks.
  eg2_rst_csr_write(h, 0x0c, 0);
  eg2_rst_csr_write(h, 0x10, 0x01);
  check(exact(h, 0x10, 0x30) && exact(h, 0x0c, 0), "ALERT_REGWEN locks ALERT_INFO_CTRL");
  eg2_rst_csr_write(h, 0x0c, 1);
  check(exact(h, 0x0c, 0), "ALERT_REGWEN is rw0c");

  // Software-controlled resets.
  eg2_rst_csr_write(h, 0x54 + 4 * 3, 0);
  eg2_rst_outputs(h, &lc_lo, &lc_hi, &sys_lo, &sys_hi, &sw_lo, &sw_hi, &req_lo, &req_hi);
  check(sw_lo == (0x3ffu & ~(1u << 3)) && sw_hi == sw_lo, "usbdev's leaf held by SW_RST_CTRL_N_3");
  check(lc_lo == 3 && sys_lo == 3, "trees released");
  eg2_rst_csr_write(h, 0x2c + 4 * 3, 0);
  eg2_rst_csr_write(h, 0x54 + 4 * 3, 1);
  check(exact(h, 0x54 + 4 * 3, 0), "SW_RST_REGWEN_3 locks SW_RST_CTRL_N_3");
  eg2_rst_inputs(h, 0, 0, 0, 2, 2, 0, 1, 0);
  eg2_rst_outputs(h, &lc_lo, &lc_hi, &sys_lo, &sys_hi, &sw_lo, &sw_hi, &req_lo, &req_hi);
  check(lc_lo == 1 && sys_lo == 1 && sw_lo == 0 && sw_hi == 0,
        "main domain in reset: its trees and leaves asserted");
  eg2_rst_inputs(h, 0, 0, 0, 0, 0, 0, 0, 0);

  // A software reset whose rst_ni falls in the same sample as pwrmgr's request: SW_RESET recorded.
  eg2_rst_csr_write(h, 0x08, 0xff);
  eg2_rst_csr_write(h, 0x04, 0x6);
  eg2_rst_inputs(h, 0, 0, 1, 3, 3, 0, 2, 0);
  eg2_rst_inputs(h, 0, 0, 0, 0, 0, 0, 0, 0);
  check(exact(h, 0x08, 0x4) && exact(h, 0x04, 0x9), "SW_RESET recorded when rst_ni falls with the request");
  eg2_rst_csr_write(h, 0x08, 0xff);
  // A life-cycle reset resets the clk_i domain (rst_ni): SW_RST_*, the REGWENs, RESET_REQ; the POR
  // domain keeps RESET_INFO and the crash dump controls.
  reset(h, 2, 0x01);
  check(exact(h, 0x54 + 4 * 3, 1) && exact(h, 0x2c + 4 * 3, 1) && exact(h, 0x0c, 1),
        "a hardware reset resets SW_RST_* and ALERT_REGWEN (clk_i domain)");
  check(exact(h, 0x10, 0x30), "ALERT_INFO_CTRL.INDEX kept through a hardware reset (POR domain)");
  // A power-on reset resets everything, SW_RST included.
  eg2_rst_inputs(h, 1, 1, 0, 0, 0, 0, 0, 0);
  check(exact(h, 0x54 + 4 * 3, 1) && exact(h, 0x2c + 4 * 3, 1), "POR resets SW_RST_*");
  check(exact(h, 0x08, 1), "POR alone after a power-on reset");
  eg2_rst_inputs(h, 0, 0, 0, 0, 0, 0, 0, 0);
  check(eg2_rst_dump_word_ok(h, 1, 0, 0, 1), "no dump after a power-on reset (reads 0)");

  // POR stretched past pwrmgr's release: recording starts when the processor's reset is really
  // released (rst_por_ni and the main rst_lc request both gone).
  eg2_rst_inputs(h, 1, 1, 1, 0, 0, 0, 0, 0);
  eg2_rst_inputs(h, 0, 1, 1, 3, 3, 0, 3, 0);
  eg2_rst_inputs(h, 0, 1, 1, 0, 0, 0, 0, 0);
  check(!eg2_rst_recording(h), "no recording while rst_por_ni still holds the processor");
  eg2_rst_inputs(h, 0, 0, 0, 0, 0, 0, 0, 0);
  check(eg2_rst_recording(h), "recording once rst_por_ni is released");
  reset(h, 2, 0x04);
  check(exact(h, 0x08, 0x21), "RESET_INFO = POR | main power glitch");

  eg2_rst_release(h);
  std::printf("rstmgr model self-test: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
