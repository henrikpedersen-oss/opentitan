// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_device_id (EG2 verification spec REQ_SEC_12, "Security in RISC-V mode"): the
 * 256-bit device identifier is readable by software and over JTAG, and its hardware half ends
 * with a CRC-32 of the bytes before it. Both modes.
 *
 * Oracle: identities and root keys, "Device Identifier": the first 128 bits are the hardware
 * origin information, 16 + 16 + 64 bits and then "CRC-32 IEEE 802.3 covering the previous
 * bytes"; the device identifier is OTP's HW_CFG0.DEVICE_ID, which lc_ctrl also exposes
 * (DEVICE_ID_0..7, lc_ctrl.hjson) to software and over its JTAG TAP.
 *
 * Software reads both copies (lc_ctrl's registers and OTP through the direct access interface,
 * in the order `order` picks), checks they are equal and checks the CRC. It leaves the words in
 * `kEg2DeviceIdSw` (RAM) with a marker; the bench (chip_sw_eg2_sec_device_id_vseq) reads the same
 * registers over the LC TAP and requires them equal to the software's.
 *
 * Built with EG2_SEC_DEVICE_ID_INJECT_FAIL the CRC is expected one bit off: the run must fail with
 * `EG2CHK FAIL hardware half CRC-32`.
 */

#include "hw/top/lc_ctrl_regs.h"   // Generated.
#include "hw/top/otp_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define DEVICE_ID_PARAMS(X) X(order, 0, 0, 1)
EG2_RM_PARAMS(DEVICE_ID_PARAMS)

enum {
  kWords = 8,
  kSwReady = 0x49445344,  // "DSDI": the software's words are in place
};

/** The software's reading, for the bench: 8 words, then kSwReady. */
volatile uint32_t kEg2DeviceIdSw[kWords + 1];

/** CRC-32 IEEE 802.3 (reflected, polynomial 0xEDB88320, initial and final XOR 0xFFFFFFFF). */
static uint32_t crc32_ieee(const uint8_t *data, uint32_t len) {
  uint32_t crc = 0xffffffffu;
  for (uint32_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

static void read_lc(uint32_t id[kWords]) {
  for (uint32_t i = 0; i < kWords; ++i) {
    id[i] = eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_DEVICE_ID_0_REG_OFFSET + 4 * i);
  }
}

static bool read_otp(uint32_t id[kWords]) {
  bool ok = true;
  // HW_CFG0 has 32-bit granules: one word per DAI read.
  for (uint32_t i = 0; i < kWords; ++i) {
    uint32_t r[2];
    ok = eg2_boot_check("DAI read of DEVICE_ID", kEg2OtpErrNone,
                        eg2_otp_dai(kEg2OtpDaiRead, OTP_CTRL_PARAM_DEVICE_ID_OFFSET + 4 * i,
                                    NULL, r)) &&
         ok;
    id[i] = r[0];
  }
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  uint32_t lc[kWords], otp[kWords];
  if (EG2_PARAM(order) == 0) {
    read_lc(lc);
    ok = read_otp(otp) && ok;
  } else {
    ok = read_otp(otp) && ok;
    read_lc(lc);
  }
  for (uint32_t i = 0; i < kWords; ++i) {
    eg2_boot_note("DEVICE_ID word", lc[i]);
  }
  bool same = true;
  for (uint32_t i = 0; i < kWords; ++i) {
    same = same && lc[i] == otp[i];
  }
  ok = eg2_boot_expect("lc_ctrl DEVICE_ID equals OTP HW_CFG0.DEVICE_ID", same, otp[0]) && ok;

  // The hardware half: bytes 0-11, then their CRC-32 in bytes 12-15 (little-endian words).
  uint8_t bytes[12];
  for (uint32_t i = 0; i < 12; ++i) {
    bytes[i] = (uint8_t)(lc[i / 4] >> (8 * (i % 4)));
  }
  uint32_t crc = crc32_ieee(bytes, sizeof(bytes));
#ifdef EG2_SEC_DEVICE_ID_INJECT_FAIL
  crc ^= 1;
#endif
  ok = eg2_boot_check("hardware half CRC-32", crc, lc[3]) && ok;

  for (uint32_t i = 0; i < kWords; ++i) {
    kEg2DeviceIdSw[i] = lc[i];
  }
  // The bench compares after the run (its JTAG reads need no CPU).
  kEg2DeviceIdSw[kWords] = kSwReady;
  return eg2_boot_summary() && ok;
}
