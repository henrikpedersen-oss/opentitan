// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_otp_rsw (EG2 verification spec REQ_RRAM_17): OTP's
 * read-set-write semantics on the RRAM: a write only sets bits; a write that
 * would clear a set bit gives MacroWriteBlankError; an integrity mismatch gives
 * MacroEccUncorrError. Through otp_ctrl's direct access interface (DAI), which
 * reaches the RRAM through `rram_ctrl_otp`.
 *
 * Documents: RFC RRAM §4.3 p9-10; rram_ctrl theory_of_operation.md "OTP
 * Hardware Plug" (OtpWrite: "a write can only set bits, never clear them. If a
 * write attempts to clear a bit that is already set, MacroWriteBlankError is
 * returned") and "Integrity Scheme" (OtpRead: "If the stored and recomputed
 * values differ, MacroEccUncorrError is returned"); otp_ctrl's ERR_CODE
 * encoding (otp_ctrl.hjson: 3 MacroEccUncorrError, 4 MacroWriteBlankError) and
 * DAI programmer's guide.
 *
 * Boot 0, n_words words of VENDOR_TEST's SCRATCH (an unlocked, 32-bit-granule
 * partition), each:
 *   1. DAI read: the current value `cur`;
 *   2. DAI write of `cur | set` (`set` new bits, drawn): no error; read back
 * `cur | set`;
 *   3. DAI write of a value with one of the set bits cleared (and others
 * drawn): ERR_CODE_11 is MacroWriteBlankError; read back: every bit of `cur |
 * set` still set. Then, with `intg` (chip only), on the last word: `EG2 RRAM
 * otp_rsw intg off=0x<o>`: the vseq flips one bit of that word's integrity byte
 * in the RRAM's OTP integrity page (the row's macro ECC kept consistent); a DAI
 * read gives MacroEccUncorrError. The vseq restores the byte and resets the
 * chip (boot_phase = 1) after `EG2 RRAM otp_rsw done`: boot 1 reads every word
 * back as boot 0 left it (OTP is non-volatile) without an error.
 *
 * Knobs:
 *   n_words     words of the scratch area, 1-14
 *   set_class   0 one new bit, 1 a few, 2 random new bits
 *   intg        1: the integrity step and the second boot (chip only)
 *   boot_phase  written by the vseq before the reset
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1 | Why |
 * |-----------------------------|-------------------------------------------------------------|
 * | the integrity step          | no bench; eg2sim's otp_ctrl models no ECC
 * (ip/eg2_mem_otp.h)|
 *
 * Built with EG2_RRAM_OTP_RSW_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/otp_ctrl_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define OTP_RSW_PARAMS(X) \
  X(n_words, 4, 1, 14)    \
  X(set_class, 2, 0, 2)   \
  X(intg, 0, 0, 1)        \
  X(boot_phase, 0, 0, 1)  \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(OTP_RSW_PARAMS)

enum {
  kPolls = 100000,
  kResetWait = 4000000,
  kDaiErr = OTP_CTRL_ERR_CODE_0_REG_OFFSET +
            4 * OTP_CTRL_PARAM_NUM_PART,  // ERR_CODE_11
  kBlank = OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_MACRO_WRITE_BLANK_ERROR,
  kUncorr = OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_MACRO_ECC_UNCORR_ERROR,
  kScratch = OTP_CTRL_PARAM_SCRATCH_OFFSET,
};

// An RRAM scratch page holds boot 0's final values for boot 1 (16 words: a
// whole number of RRAM words).
enum {
  kRecordPage = kEg2RramScratchFirstPage + 24,
  kRecord = kRecordPage * kEg2RramPageBytes,
};
static uint32_t record[16];

static uint32_t otp_rd(uint32_t off) {
  return eg2_dev_read32(kEg2DevOtpCtrlCore, off);
}
static void otp_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevOtpCtrlCore, off, v);
}

static bool dai_wait_idle(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (otp_rd(OTP_CTRL_STATUS_REG_OFFSET) &
        (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
      return true;
    }
  }
  return false;
}

/** A DAI command at `addr`; returns ERR_CODE_11, 0xff on a timeout. */
static uint32_t dai_cmd(uint32_t addr, uint32_t cmd_bit, uint32_t wdata,
                        uint32_t *rdata) {
  if (!dai_wait_idle()) {
    return 0xff;
  }
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET, wdata);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_WDATA_1_REG_OFFSET, 0);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET, 1u << cmd_bit);
  if (!dai_wait_idle()) {
    return 0xff;
  }
  if (rdata != NULL) {
    *rdata = otp_rd(OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  }
  return otp_rd(kDaiErr) & OTP_CTRL_ERR_CODE_0_ERR_CODE_0_MASK;
}

static uint32_t dai_read(uint32_t addr, uint32_t *v) {
  return dai_cmd(addr, OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT, 0, v);
}
static uint32_t dai_write(uint32_t addr, uint32_t v) {
  return dai_cmd(addr, OTP_CTRL_DIRECT_ACCESS_CMD_WR_BIT, v, NULL);
}

/** New bits to set, none of `cur`'s; 0 if every bit is already set. */
static uint32_t draw_set(uint32_t cur) {
  uint32_t free_bits = ~cur;
  if (free_bits == 0) {
    return 0;
  }
  uint32_t r;
  switch (EG2_PARAM(set_class)) {
    case 0:
      r = 0;
      break;
    case 1:
      r = eg2_rand() & eg2_rand() & eg2_rand();
      break;
    default:
      r = eg2_rand();
      break;
  }
  r &= free_bits;
  if (r == 0) {
    // At least one: the lowest free bit at or above a drawn position.
    uint32_t b = eg2_rand_range(0, 31);
    for (uint32_t i = 0; i < 32; ++i) {
      uint32_t bit = 1u << ((b + i) % 32);
      if (free_bits & bit) {
        return bit;
      }
    }
  }
  return r;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = true;
  uint32_t n = EG2_PARAM(n_words);
  // The words: a drawn start in SCRATCH, the same in both boots (the seed's
  // sequence).
  uint32_t first = eg2_rand_range(0, 14 - n);
  eg2_chk_step("boot phase", EG2_PARAM(boot_phase));
  eg2_chk_step("first scratch word", first);
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  eg2_rram_region_set(region, kRecordPage, 1,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);

  if (EG2_PARAM(boot_phase) == 1) {
    // The values boot 0 left, against the record boot 0 wrote into an RRAM
    // scratch page.
    eg2_rram_result_t r;
    ok =
        eg2_chk_true("record read done",
                     eg2_rram_read(kEg2RramPartData, kRecord, record, n, &r)) &&
        ok;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t v = 0;
      ok = eg2_chk_eq("read after the reset: error code", 0,
                      dai_read(kScratch + 4 * (first + i), &v)) &&
           ok;
      ok = eg2_chk_eq("the word as boot 0 left it", record[i], v) && ok;
    }
    eg2_rram_region_off(region);
    return eg2_chk_summary(2 * n) && ok;
  }

  uint32_t last = 0, last_val = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t addr = kScratch + 4 * (first + i);
    uint32_t cur = 0, v = 0;
    ok = eg2_chk_eq("read error code", 0, dai_read(addr, &cur)) && ok;
    uint32_t set = draw_set(cur);
    uint32_t val = cur | set;
    eg2_chk_step("word", addr);
    eg2_chk_step("current", cur);
    eg2_chk_step("written", val);
    ok =
        eg2_chk_eq("write setting bits: error code", 0, dai_write(addr, val)) &&
        ok;
    ok = eg2_chk_eq("write setting bits: error code of the read back", 0,
                    dai_read(addr, &v)) &&
         ok;
    ok = eg2_chk_eq("read back after setting bits", val, v) && ok;
    if (val != 0) {
      // Clear one set bit, set or clear the others at random.
      uint32_t setbits = val;
      uint32_t k = eg2_rand_range(0, (uint32_t)__builtin_popcount(setbits) - 1);
      uint32_t clr = 0;
      for (uint32_t b = 0; b < 32; ++b) {
        if ((setbits >> b) & 1) {
          if (k-- == 0) {
            clr = 1u << b;
            break;
          }
        }
      }
      uint32_t bad = (val | eg2_rand()) & ~clr;
      ok = eg2_chk_eq("write clearing a bit: MacroWriteBlankError", kBlank,
                      dai_write(addr, bad)) &&
           ok;
      ok = eg2_chk_eq("read after the refused write: error code", 0,
                      dai_read(addr, &v)) &&
           ok;
      ok = eg2_chk_eq("the set bits stay set", val, v & val) && ok;
    }
    record[i] = v;
    last = addr;
    last_val = val;
  }
  eg2_rram_result_t rr;
  ok = eg2_chk_true(
           "record write done",
           eg2_rram_write(kEg2RramPartData, kRecord, record, 16, &rr)) &&
       ok;
  eg2_rram_region_off(region);
  // The blank errors raised otp_error; the reads since cleared the DAI's error
  // code.
  otp_wr(OTP_CTRL_INTR_STATE_REG_OFFSET,
         1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT);

  if (!EG2_PARAM(intg) || gaps) {
    if (gaps) {
      eg2_chk_skip(
          "the integrity mismatch (no bench, no OTP ECC in the model)");
    }
    return eg2_chk_summary(4 * n) && ok;
  }
  LOG_INFO("EG2 RRAM otp_rsw intg off=0x%x", last);
  eg2_chk_delay(2000);
  uint32_t v = 0;
  ok = eg2_chk_eq("read with a corrupted integrity byte: MacroEccUncorrError",
                  kUncorr, dai_read(last, &v)) &&
       ok;
  eg2_chk_step("value read", v);
  eg2_chk_step("value written", last_val);
  ok = eg2_chk_summary(4 * n) && ok;
  if (!ok) {
    return false;
  }
  EG2_CHK_SYNC("EG2 RRAM otp_rsw done");
  eg2_chk_delay(kResetWait);
  return eg2_chk_true("the bench reset the chip", false);
}
