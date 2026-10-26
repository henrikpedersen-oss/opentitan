// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_otp_layout (EG2 verification spec REQ_RRAM_18): the OTP
 * partition layout in the RRAM matches the RFC: each partition at its offset
 * inside the OTP pages, the integrity page holding one Hamming(72,64) byte per
 * 64 bits of OTP data, no scrambling and no address combination in the OTP
 * region.
 *
 * Documents: RFC RRAM p8-9; rram_ctrl README "Address Map" (OTP integrity page
 * 4090 at 0x1FF400, OTP pages 4091-4095 from OtpStartAddr 0x1FF600),
 * theory_of_operation.md "OTP Hardware Plug", "Integrity Scheme", "Address-XOR"
 * (disabled for the OTP region) and "RRAM Scrambling"
 * (`rram_ctrl_otp` bypasses scrambling); otp_ctrl's memory map
 * (otp_ctrl_mmap.hjson, through the generated
 * OTP_CTRL_PARAM_<PARTITION>_OFFSET/_SIZE).
 *
 * For each partition, in the drawn order, the software reads its first granule
 * (32 bits; 64 bits in the secret partitions) and its last, the 64-bit digest,
 * through otp_ctrl's direct access interface (DAI), and prints `EG2 RRAM
 * otp_layout off=0x<o> lo=0x<l> hi=0x<h> w=<4|8> err=<e>`. A non-secret
 * partition must read without an error; there the value also equals the word
 * otp_ctrl's software config window shows (for the partitions the window
 * holds). A read-locked secret partition answers with an access error (no
 * value). The vseq, for every line: the RRAM row at 0x1FF600 + off holds the
 * value's bytes unchanged (no scrambling, no address XOR), and the integrity
 * byte at 0x1FF400 + off / 8 is the Hamming(72,64) check byte of the 64-bit
 * chunk; for an access-error line the integrity byte alone. It also checks the
 * partition table's last byte lies below the top of the RRAM.
 *
 * Knobs:
 *   order       0 address order, 1 reverse, 2 random
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1           | Why |
 * |---------------------------------------|----------------------------------------------------|
 * | (nothing: the model run reads every   | the RRAM rows are the vseq's (no
 * bench in eg2sim)  | |   partition through the DAI)          | |
 *
 * Built with EG2_RRAM_OTP_LAYOUT_INJECT_FAIL the first value check expects bit
 * 0 flipped: the run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/otp_ctrl_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define OTP_LAYOUT_PARAMS(X) \
  X(order, 2, 0, 2)          \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(OTP_LAYOUT_PARAMS)

enum {
  kPolls = 100000,
  kParts = OTP_CTRL_PARAM_NUM_PART,
  kDaiErr = OTP_CTRL_ERR_CODE_0_REG_OFFSET +
            4 * OTP_CTRL_PARAM_NUM_PART,  // ERR_CODE_11
  kAccessError = OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
};

// The partitions: offset, size, secret (64-bit granule), in the software config
// window.
static const uint16_t kOff[kParts] = {
    OTP_CTRL_PARAM_VENDOR_TEST_OFFSET,
    OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
    OTP_CTRL_PARAM_OWNER_SW_CFG_OFFSET,
    OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_OFFSET,
    OTP_CTRL_PARAM_ROT_CREATOR_AUTH_STATE_OFFSET,
    OTP_CTRL_PARAM_HW_CFG0_OFFSET,
    OTP_CTRL_PARAM_HW_CFG1_OFFSET,
    OTP_CTRL_PARAM_SECRET0_OFFSET,
    OTP_CTRL_PARAM_SECRET1_OFFSET,
    OTP_CTRL_PARAM_SECRET2_OFFSET,
    OTP_CTRL_PARAM_LIFE_CYCLE_OFFSET,
};
static const uint16_t kSize[kParts] = {
    OTP_CTRL_PARAM_VENDOR_TEST_SIZE,
    OTP_CTRL_PARAM_CREATOR_SW_CFG_SIZE,
    OTP_CTRL_PARAM_OWNER_SW_CFG_SIZE,
    OTP_CTRL_PARAM_ROT_CREATOR_AUTH_CODESIGN_SIZE,
    OTP_CTRL_PARAM_ROT_CREATOR_AUTH_STATE_SIZE,
    OTP_CTRL_PARAM_HW_CFG0_SIZE,
    OTP_CTRL_PARAM_HW_CFG1_SIZE,
    OTP_CTRL_PARAM_SECRET0_SIZE,
    OTP_CTRL_PARAM_SECRET1_SIZE,
    OTP_CTRL_PARAM_SECRET2_SIZE,
    OTP_CTRL_PARAM_LIFE_CYCLE_SIZE,
};
// 1: secret (64-bit granule); 2: read through the software config window too;
// 4: the DAI cannot read it in this state or ever (the life-cycle partition is
// the life-cycle controller's).
// HW_CFG0/1 through the DAI only: the programmer's guide ("Direct Access Memory Map") makes only
// CREATOR_SW_CFG and OWNER_SW_CFG accessible through the memory-mapped window. A window read at a
// buffered partition's address is never answered (no grant, no error) and hangs the bus, against
// the documented error for a window access out of bounds: F53 (2026-10-26: regr-vlt hung on the
// HW_CFG0 window read in both modes).
static const uint8_t kFlags[kParts] = {2, 2, 2, 2, 2, 0, 0, 1, 1, 1, 4};

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

/** A DAI read; returns ERR_CODE_11 (the DAI's error code), 0xff on a timeout.
 */
static uint32_t dai_read(uint32_t addr, uint32_t *lo, uint32_t *hi) {
  if (!dai_wait_idle()) {
    return 0xff;
  }
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
         1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  if (!dai_wait_idle()) {
    return 0xff;
  }
  *lo = otp_rd(OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  *hi = otp_rd(OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET);
  return otp_rd(kDaiErr) & OTP_CTRL_ERR_CODE_0_ERR_CODE_0_MASK;
}

static bool read_granule(uint32_t part, uint32_t off, uint32_t w) {
  bool ok = true;
  uint32_t lo = 0, hi = 0;
  uint32_t err = dai_read(off, &lo, &hi);
  if (w == 4) {
    hi = 0;
  }
  eg2_chk_step("partition", part);
  LOG_INFO("EG2 RRAM otp_layout off=0x%x lo=0x%x hi=0x%x w=%d err=%d", off, lo,
           hi, (int)w, (int)err);
  if (kFlags[part] & 1) {
    // A secret partition: its value only if it is not read-locked (its digest
    // unset).
    ok = eg2_chk_true("secret partition: no error or an access error",
                      err == 0 || err == kAccessError) &&
         ok;
  } else {
    ok = eg2_chk_eq("DAI read error code", 0, err) && ok;
    if (kFlags[part] & 2) {
      ok = eg2_chk_eq("DAI value equals the config window",
                      otp_rd(OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + off), lo) &&
           ok;
      if (w == 8) {
        ok = eg2_chk_eq("DAI value equals the config window (upper word)",
                        otp_rd(OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + off + 4),
                        hi) &&
             ok;
      }
    }
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  bool ok = true;
  // The access errors the locked partitions answer with are the DAI's otp_error
  // interrupt.
  uint32_t order[kParts];
  for (uint32_t i = 0; i < kParts; ++i) {
    order[i] = EG2_PARAM(order) == 1 ? kParts - 1 - i : i;
  }
  if (EG2_PARAM(order) == 2) {
    for (uint32_t i = kParts - 1; i > 0; --i) {
      uint32_t j = eg2_rand_range(0, i);
      uint32_t t = order[i];
      order[i] = order[j];
      order[j] = t;
    }
  }
  uint32_t checked = 0;
  for (uint32_t k = 0; k < kParts; ++k) {
    uint32_t p = order[k];
    ok = eg2_chk_true(
             "partition inside the OTP region",
             kEg2RramOtpStartAddr + kOff[p] + kSize[p] <= kEg2RramHostSize) &&
         ok;
    if (kFlags[p] & 4) {
      // Not DAI-readable: the vseq checks its rows' integrity bytes from the
      // table.
      LOG_INFO("EG2 RRAM otp_layout part off=0x%x size=0x%x", (uint32_t)kOff[p],
               (uint32_t)kSize[p]);
      continue;
    }
    // The first granule (32 bits; 64 in a secret partition), and the last: the
    // partition's 64-bit digest (otp_ctrl_mmap: every partition here but
    // LIFE_CYCLE ends in its digest).
    ok = read_granule(p, kOff[p], (kFlags[p] & 1) ? 8 : 4) && ok;
    ok = read_granule(p, kOff[p] + kSize[p] - 8, 8) && ok;
    ++checked;
  }
  EG2_CHK_SYNC("EG2 RRAM otp_layout done");
  // A successful read leaves the DAI's error code clear again; the access
  // errors raised otp_error.
  uint32_t lo, hi;
  ok = eg2_chk_eq("DAI error code clear after a good read", 0,
                  dai_read(OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET, &lo, &hi)) &&
       ok;
  otp_wr(OTP_CTRL_INTR_STATE_REG_OFFSET,
         1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT);
  return eg2_chk_summary(2 * checked) && ok;
}
