// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the clock manager model through its DPI functions (no simulator), from the
// directory of this file:
//   g++ -std=c++11 eg2_clkmgr_dpi_test.cc eg2_clkmgr_dpi.cc -o /tmp/clk_test && /tmp/clk_test
// (no Makefile target: eg2_model/Makefile belongs to another change). EG2_CLK_TEST_BREAK=1 inverts
// one expectation: the run must then fail.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
void *eg2_clk_create(int);
void eg2_clk_csr_write(void *, uint32_t, uint32_t, int);
void eg2_clk_csr_read(void *, uint32_t, int, uint32_t *, uint32_t *);
void eg2_clk_outputs(void *, int, uint32_t *, uint32_t *);
void eg2_clk_inputs(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t, uint32_t);
void eg2_clk_aon_period(void *, uint32_t, uint32_t, uint32_t, uint32_t);
void eg2_clk_settled(void *);
}

static int fails = 0;
#define CHK(c)                                                  \
  do {                                                          \
    if (!(c)) {                                                 \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);   \
      fails++;                                                  \
    }                                                           \
  } while (0)

int main() {
  const bool brk = std::getenv("EG2_CLK_TEST_BREAK") != nullptr;
  void *h = eg2_clk_create(0);
  uint32_t lo, hi;
  // Shadowed thresholds: reset value, a read leaves the second phase, a matched pair commits, a
  // mismatched pair sets SHADOW_UPDATE_ERR and commits only the field that matched.
  eg2_clk_csr_read(h, 0x2c, 1, &lo, &hi);
  CHK(lo == ((470u << 10) | 490u) && hi == lo);
  eg2_clk_csr_write(h, 0x2c, (10u << 10) | 20, 1);
  eg2_clk_csr_read(h, 0x2c, 1, &lo, &hi);
  CHK(lo == ((470u << 10) | 490u));
  eg2_clk_csr_write(h, 0x2c, (10u << 10) | 20, 1);
  eg2_clk_csr_write(h, 0x2c, (10u << 10) | 20, 1);
  eg2_clk_csr_read(h, 0x2c, 1, &lo, &hi);
  CHK(lo == ((10u << 10) | 20));
  eg2_clk_csr_write(h, 0x2c, (11u << 10) | 20, 1);
  eg2_clk_csr_write(h, 0x2c, (12u << 10) | 20, 1);
  eg2_clk_csr_read(h, 0x48, 1, &lo, &hi);
  CHK(lo == 1);
  eg2_clk_csr_read(h, 0x2c, 1, &lo, &hi);
  CHK(lo == ((10u << 10) | 20));
  // ALERT_TEST: REGWEN (bit 31) reads 1 after reset.
  eg2_clk_csr_read(h, 0x00, 1, &lo, &hi);
  CHK(lo == 0x80000000u && hi == lo);
  // Jitter: false at reset; any other value enables.
  eg2_clk_outputs(h, 1, &lo, &hi);
  CHK(lo & (1u << 7));
  eg2_clk_csr_write(h, 0x14, 0x5, 1);
  eg2_clk_outputs(h, 1, &lo, &hi);
  CHK(lo & (1u << 6));
  // External clock: selected but debug off: no request; debug on: request; another value: no
  // impact; false: deselected.
  eg2_clk_csr_write(h, 0x08, 0x96, 1);
  eg2_clk_outputs(h, 1, &lo, &hi);
  CHK(lo & (1u << 1));
  eg2_clk_inputs(h, 0x6666, 0x9, 0x5, 0xa, 0x9, 0x9, 0x9, 0x6, 7, 7);
  eg2_clk_outputs(h, 1, &lo, &hi);
  CHK(lo & (1u << 0));
  eg2_clk_csr_write(h, 0x08, 0x95, 1);
  eg2_clk_outputs(h, 1, &lo, &hi);
  CHK(lo & (1u << 0));
  eg2_clk_csr_write(h, 0x08, 0x99, 1);
  eg2_clk_outputs(h, 1, &lo, &hi);
  CHK(lo & (1u << 1));
  // Hints: hint 0 and idle: off; busy: on.
  eg2_clk_csr_write(h, 0x1c, 0x0, 1);
  eg2_clk_csr_read(h, 0x20, 1, &lo, &hi);
  CHK(lo == 0 && hi == 0);
  eg2_clk_inputs(h, 0x6669, 0x9, 0x5, 0xa, 0x9, 0x9, 0x9, 0x6, 7, 7);
  eg2_clk_csr_read(h, 0x20, 1, &lo, &hi);
  CHK(lo == (brk ? 0u : 1u) && hi == 1);
  // Measurement: io thresholds 10..20, enabled, 30 io edges per always-on period: too fast.
  eg2_clk_csr_write(h, 0x48, 0x1ff, 1);
  eg2_clk_csr_write(h, 0x28, 0x6, 1);
  for (int k = 0; k < 8; k++) eg2_clk_aon_period(h, 30, 14, 7, 7);
  eg2_clk_csr_read(h, 0x48, 1, &lo, &hi);
  CHK(lo & 2);
  // Calibration lost: the enables cleared, MEASURE_CTRL_REGWEN set.
  eg2_clk_csr_write(h, 0x24, 0, 1);
  eg2_clk_inputs(h, 0x6669, 0x9, 0x5, 0xa, 0x9, 0x9, 0x9, 0x9, 7, 7);
  eg2_clk_settled(h);
  eg2_clk_csr_read(h, 0x28, 1, &lo, &hi);
  CHK(lo == 9 && hi == 9);
  eg2_clk_csr_read(h, 0x24, 1, &lo, &hi);
  CHK(lo == 1);
  std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  return fails != 0;
}
