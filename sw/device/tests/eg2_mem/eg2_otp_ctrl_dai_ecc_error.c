// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_otp_ctrl_dai_ecc_error (EG2 verification spec REQ_OTP_01, REQ_RRAM_17): an integrity
 * error planted by the bench in one OTP word, then read through the direct access interface
 * (DAI), is reported, in both modes.
 *
 * In Earl Grey 2 the OTP is the top pages of the RRAM, reached through rram_ctrl's OTP plug: an
 * OtpRead recomputes the word's Hamming integrity and answers a mismatch with
 * `MacroEccUncorrError` (rram_ctrl theory_of_operation.md "OTP Hardware Plug"; RFC RRAM §4.3
 * p9-10). otp_ctrl reports it (otp_ctrl programmers_guide.md "Error Handling"):
 *   - VENDOR_TEST: the error is collapsed into `MacroEccCorrError` (0x2), recoverable: the
 *     `otp_error` interrupt, ERR_CODE of the DAI 0x2, STATUS.DAI_ERROR, no alert; the DAI takes
 *     the next command, and once the bench has restored the word a read returns it with no error;
 *   - the other unbuffered partitions (CREATOR_SW_CFG, OWNER_SW_CFG, ROT_CREATOR_AUTH_CODESIGN,
 *     ROT_CREATOR_AUTH_STATE): `MacroEccUncorrError` (0x3), not recoverable: `otp_error`, ERR_CODE
 *     0x3, STATUS.DAI_ERROR, the `fatal_macro_error` alert; the DAI stays in its error state
 *     until a reset. The bench then restores the word and resets the chip (boot_phase 1), and the
 *     second boot reads the word through the DAI with no error.
 * Before the bench plants the error, the word is read through the DAI and, for the two SW_CFG
 * partitions, through the SW_CFG window too: both paths agree (and in eg2sim this is the check
 * that runs).
 *
 * The DAI's error code is ERR_CODE_11 (NumPart = 11 partitions first, then the DAI, then the
 * LCI: otp_ctrl.hjson).
 *
 * Knobs: partition (0 VENDOR_TEST, 1 CREATOR_SW_CFG, 2 OWNER_SW_CFG, 3 ROT_CREATOR_AUTH_CODESIGN,
 * 4 ROT_CREATOR_AUTH_STATE, 5 drawn), word (index into the partition's words before its digest,
 * 0xffff: drawn), boot_phase (the bench's), model_gaps.
 *
 * | Skipped with model_gaps = 1               | Why                                            |
 * |-------------------------------------------|------------------------------------------------|
 * | the planted error and everything after it | no bench in eg2sim; its OTP model has no       |
 * |                                           |   integrity page                               |
 *
 * Built with EG2_OTP_CTRL_DAI_ECC_ERROR_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `MEM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 * (2026-10-26: the bench plants the error, restores the word and resets the chip; Verilator has no
 * bench, so the planted-error checks failed there.)
 */

#include "hw/top/otp_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define DAI_ECC_PARAMS(X)    \
  X(partition, 5, 0, 5)      \
  X(word, 0xffff, 0, 0xffff) \
  X(boot_phase, 0, 0, 1)     \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(DAI_ECC_PARAMS)

enum {
  kPolls = 100000,
  kDaiErr = OTP_CTRL_ERR_CODE_11_REG_OFFSET,
  kCorr = 0x2,
  kUncorr = 0x3,
};

// (offset, bytes before the digest) per partition, otp_ctrl_mmap.md.
static uint32_t part_offset(uint32_t p) {
  switch (p) {
    case 0:
      return OTP_CTRL_PARAM_VENDOR_TEST_OFFSET;
    case 1:
      return OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET;
    case 2:
      return OTP_CTRL_PARAM_OWNER_SW_CFG_OFFSET;
    case 3:
      return OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_OFFSET;
    default:
      return OTP_CTRL_PARAM_ROT_CREATOR_AUTH_STATE_OFFSET;
  }
}

static uint32_t part_words(uint32_t p) {
  switch (p) {
    case 0:
      return OTP_CTRL_PARAM_VENDOR_TEST_SIZE / 4 - 2;
    case 1:
      return OTP_CTRL_PARAM_CREATOR_SW_CFG_SIZE / 4 - 2;
    case 2:
      return OTP_CTRL_PARAM_OWNER_SW_CFG_SIZE / 4 - 2;
    case 3:
      return OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_SIZE / 4 - 2;
    default:
      return OTP_CTRL_PARAM_ROT_CREATOR_AUTH_STATE_SIZE / 4 - 2;
  }
}

static uint32_t otp_rd(uint32_t off) { return eg2_dev_read32(kEg2DevOtpCtrlCore, off); }
static void otp_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevOtpCtrlCore, off, v); }

/** One 32-bit DAI read; returns the DAI's ERR_CODE, `*value` the data. */
static uint32_t dai_read(uint32_t addr, uint32_t *value, bool *idle) {
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET, 1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  *idle = false;
  for (uint32_t i = 0; i < kPolls; ++i) {
    uint32_t st = otp_rd(OTP_CTRL_STATUS_REG_OFFSET);
    if (st & ((1u << OTP_CTRL_STATUS_DAI_IDLE_BIT) | (1u << OTP_CTRL_STATUS_DAI_ERROR_BIT))) {
      *idle = (st & (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) != 0;
      break;
    }
  }
  *value = otp_rd(OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  return otp_rd(kDaiErr) & 0x7;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  uint32_t p = EG2_PARAM(partition);
  if (p > 4) {
    p = eg2_rand_range(0, 4);
  }
  uint32_t w = EG2_PARAM(word);
  if (w >= part_words(p)) {
    w = eg2_rand_range(0, part_words(p) - 1);
  }
  uint32_t addr = part_offset(p) + 4 * w;
  eg2_chk_step("partition", p);
  eg2_chk_step("otp address", addr);
  bool ok = true;
  bool idle;
  uint32_t v0 = 0, v1 = 0;

  if (EG2_PARAM(boot_phase) == 1) {
    // After the reset: the restored word reads with no error.
    uint32_t err = dai_read(addr, &v0, &idle);
    ok = eg2_chk_eq("DAI error code after the reset", 0, err) && ok;
    ok = eg2_chk_true("DAI idle after the reset", idle) && ok;
    return eg2_chk_summary(2) && ok;
  }

  uint32_t err = dai_read(addr, &v0, &idle);
  ok = eg2_chk_eq("DAI error code before the error", 0, err) && ok;
  if (p == 1 || p == 2) {
    // The SW_CFG window presents CREATOR_SW_CFG and OWNER_SW_CFG at their OTP offsets.
    v1 = otp_rd(OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + addr);
    ok = eg2_chk_eq("SW_CFG window and DAI agree", v0, v1) && ok;
  }
  if (EG2_PARAM(model_gaps)) {
    eg2_chk_skip("the planted integrity error (no bench, no integrity page in eg2sim)");
    return eg2_chk_summary(1) && ok;
  }

  const bool corr = p == 0;
  int alert = eg2_status_alert_find("otp_ctrl_fatal_macro_error");
  eg2_status_expect_intr(kEg2DevOtpCtrlCore, 1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT);
  if (!corr && alert >= 0) {
    eg2_status_expect_alert((size_t)alert);
  }
  // The bench flips one bit of the word in the RRAM, its integrity left as it was.
  LOG_INFO("EG2 MEM otp plant 0x%08x", addr);
  eg2_chk_delay(2000);
  err = dai_read(addr, &v1, &idle);
  ok = eg2_chk_eq("DAI error code for the planted error", corr ? kCorr : kUncorr, err) && ok;
  ok = eg2_chk_true("STATUS.DAI_ERROR",
                    (otp_rd(OTP_CTRL_STATUS_REG_OFFSET) >> OTP_CTRL_STATUS_DAI_ERROR_BIT) & 1) &&
       ok;
  ok = eg2_chk_true("otp_error interrupt",
                    (otp_rd(OTP_CTRL_INTR_STATE_REG_OFFSET) >> OTP_CTRL_INTR_STATE_OTP_ERROR_BIT) &
                        1) &&
       ok;
  if (!corr) {
    ok = eg2_chk_true("fatal_macro_error alert",
                      alert >= 0 && eg2_status_wait_alert((size_t)alert, 10000)) &&
         ok;
    // Not recoverable: the bench restores the word and resets the chip (boot_phase 1).
    EG2_CHK_SYNC("EG2 MEM otp restore and reset");
    eg2_chk_delay(2000000);
    return eg2_chk_true("the bench's reset after the uncorrectable error", false) && ok;
  }
  // Recoverable: the bench restores the word; the DAI reads it with no error.
  EG2_CHK_SYNC("EG2 MEM otp restore");
  eg2_chk_delay(2000);
  err = dai_read(addr, &v1, &idle);
  ok = eg2_chk_eq("DAI error code after the restore", 0, err) && ok;
  ok = eg2_chk_eq("word after the restore", v0, v1) && ok;
  eg2_status_clear_expected();
  return eg2_chk_summary(5) && ok;
}
