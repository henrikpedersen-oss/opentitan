// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_ctrl_clock_freqs (EG2 verification spec REQ_RRAM_27, "RRAM controller in RISC-V
 * mode"): nothing reaches RRAM before the PHY's init_done; software operations get op_err until
 * INIT and keys_valid; controller and host accesses are correct with clock jitter on and off, with
 * `clk_otp` = `io_div4`.
 *
 * Expected behaviour: programmer's guide "Initialization" (PHY_STATUS.init_done; "Until
 * STATUS.init_done is set, all software-initiated RRAM operations are rejected by the arbiter with
 * OP_STATUS.err and ERR_CODE.op_err set"); RFC RRAM §4.4 p10. The clocks: the bench draws the
 * jittery clock at boot (OTP CREATOR_SW_CFG_JITTER_EN, which the test ROM applies), and the test
 * switches clkmgr's JITTER_ENABLE (MuBi4) between rounds; clk_otp is io_div4 by the top's clock
 * connections, which the bench's connectivity checks own.
 *
 * Each round: a controller write of a drawn length, a controller read-back, host reads of a
 * sample, and a Rewrite, all without an error; STATUS.init_done and keys_valid and
 * PHY_STATUS.init_done stay set.
 *
 * Not reachable from software here: "op_err until INIT" and "nothing before init_done". The test
 * ROM (and the ROM) start the controller's initialisation and wait for it before the test runs
 * (nvm_testutils_rom_init), and no software reset of rram_ctrl alone exists, so the test can only
 * check that initialisation is complete when it starts. The pre-INIT window is the block-level
 * bench's (rram_ctrl_testplan) or needs a boot ROM that skips INIT.
 *
 * Knobs:
 *   n_rounds      rounds
 *   jitter_sw     0 leave the jitter as booted, 1 toggle JITTER_ENABLE before each round
 *   max_words     the longest write of a round (a multiple of 4 is drawn up to it)
 *   model_gaps    skips what the SystemC model does not model: nothing (clkmgr's JITTER_ENABLE is
 *                 storage there)
 *
 * Built with EG2_RRAM_CTRL_CLOCK_FREQS_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `RRAM FAIL`.
 */

#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/clkmgr_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define CLOCK_FREQS_PARAMS(X) \
  X(n_rounds, 6, 1, 64)       \
  X(jitter_sw, 1, 0, 1)       \
  X(max_words, 256, 4, 1024)  \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(CLOCK_FREQS_PARAMS)

enum {
  kPolls = 200000,
  kPages = 16,
  kSecBase = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
};

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool ok = true;
  const uint32_t ready = (1u << RRAM_CTRL_STATUS_INIT_DONE_BIT) |
                         (1u << RRAM_CTRL_STATUS_KEYS_VALID_BIT);
  ok = eg2_chk_eq("PHY_STATUS.init_done at the start", 1,
                  (eg2_rram_reg(RRAM_CTRL_PHY_STATUS_REG_OFFSET) >> 1) & 1) &&
       ok;
  ok = eg2_chk_eq("STATUS init_done and keys_valid at the start", ready,
                  eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & ready) &&
       ok;
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  uint32_t first = eg2_rand_range(kEg2RramScratchFirstPage,
                                  kEg2RramScratchLastPage - kPages + 1);
  eg2_rram_region_set(
      region, first, kPages,
      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc | kEg2RramAttrScr);
  uint32_t base = first * kEg2RramPageBytes;
  volatile uint32_t *data =
      eg2_chk_ptr(kSecBase);  // what each round writes (up to 4 KiB)
  bool jitter_lockable =
      (eg2_dev_read32(kEg2DevClkmgr, CLKMGR_JITTER_REGWEN_REG_OFFSET) & 1) != 0;
  uint32_t jitter =
      eg2_dev_read32(kEg2DevClkmgr, CLKMGR_JITTER_ENABLE_REG_OFFSET);
  eg2_chk_step("jitter at boot", jitter);
  if (EG2_PARAM(jitter_sw) && !jitter_lockable) {
    eg2_chk_step("JITTER_REGWEN closed: jitter left as booted", 0);
  }

  uint32_t rounds = 0;
  for (uint32_t i = 0; i < EG2_PARAM(n_rounds); ++i) {
    if (EG2_PARAM(jitter_sw) && jitter_lockable) {
      jitter = jitter == kMultiBitBool4True ? kMultiBitBool4False
                                            : kMultiBitBool4True;
      eg2_dev_write32(kEg2DevClkmgr, CLKMGR_JITTER_ENABLE_REG_OFFSET, jitter);
      eg2_chk_step("jitter", jitter);
    }
    uint32_t n = 4 * eg2_rand_range(1, EG2_PARAM(max_words) / 4);
    uint32_t a =
        base + kEg2RramWordBytes *
                   eg2_rand_range(0, (kPages * kEg2RramPageBytes - 4 * n) /
                                         kEg2RramWordBytes);
    eg2_chk_step("write at", kEg2RramHostBase + a);
    eg2_chk_step("words", n);
    for (uint32_t j = 0; j < n; ++j) {
      data[j] = eg2_rand();
    }
    eg2_rram_result_t r;
    ok = eg2_chk_true("write done",
                      eg2_rram_write(kEg2RramPartData, a,
                                     (const uint32_t *)data, n, &r)) &&
         ok;
    ok = eg2_chk_eq("write OP_STATUS", 1, r.op_status) && ok;
    ok = eg2_chk_eq("write ERR_CODE", 0, r.err_code) && ok;
    // Read back 16 words at a time.
    uint32_t buf[16];
    uint32_t bad = 0;
    for (uint32_t j = 0; j < n; j += 16) {
      uint32_t m = n - j < 16 ? n - j : 16;
      ok = eg2_chk_true("read done", eg2_rram_read(kEg2RramPartData, a + 4 * j,
                                                   buf, m, &r)) &&
           ok;
      ok = eg2_chk_eq("read ERR_CODE", 0, r.err_code) && ok;
      for (uint32_t k = 0; k < m; ++k) {
        bad += buf[k] != data[j + k];
      }
    }
    ok = eg2_chk_eq("controller read-back words that differ", 0, bad) && ok;
    for (uint32_t k = 0; k < 8; ++k) {
      uint32_t j = eg2_rand_range(0, n - 1);
      ok = eg2_chk_eq("host read", data[j], *eg2_rram_host(a + 4 * j)) && ok;
    }
    uint32_t rw = a + 4 * eg2_rand_range(0, n - 1);
    ok = eg2_chk_true("rewrite done",
                      eg2_rram_rewrite(kEg2RramPartData, rw, &r)) &&
         ok;
    ok = eg2_chk_eq("rewrite ERR_CODE", 0, r.err_code) && ok;
    ok = eg2_chk_eq("word after the rewrite", data[(rw - a) / 4],
                    *eg2_rram_host(rw)) &&
         ok;
    ok = eg2_chk_eq("STATUS init_done and keys_valid", ready,
                    eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & ready) &&
         ok;
    ++rounds;
  }
  eg2_rram_region_off(region);
  ok = eg2_chk_eq("rounds done", EG2_PARAM(n_rounds), rounds) && ok;
  return eg2_chk_summary(2 + 9 * EG2_PARAM(n_rounds)) && ok;
}
