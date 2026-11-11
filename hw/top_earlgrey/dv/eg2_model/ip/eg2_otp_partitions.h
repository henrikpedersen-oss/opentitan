// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#ifndef EG2_MODEL_IP_EG2_OTP_PARTITIONS_H_
#define EG2_MODEL_IP_EG2_OTP_PARTITIONS_H_
#include <cstdint>
namespace eg2_model {
// ---- The OTP partitions (otp_ctrl_partitions.md, otp_ctrl_mmap.md) ----
struct Eg2OtpPart {
  const char *name;
  uint32_t off, size;     // byte offset and size of the whole partition, digest and marker included
  bool secret;            // scrambled, 64-bit granule, read-locked by its digest
  bool hw_digest;         // the digest is computed by the DAI DIGEST command, never written
  bool has_digest;        // LIFE_CYCLE has none
  const char *read_lock;  // the READ_LOCK CSR, or nullptr
  bool zer;               // zeroizable: a zeroization marker (8 bytes) after the digest
  bool buffered;
};

// Offsets and sizes: otp_ctrl_mmap.md; no RTL package is an authority here.
inline constexpr Eg2OtpPart kEg2OtpParts[] = {
    {"VENDOR_TEST", 0x000, 64, false, false, true, "VENDOR_TEST_READ_LOCK", false, false},
    {"CREATOR_SW_CFG", 0x040, 400, false, false, true, "CREATOR_SW_CFG_READ_LOCK", false, false},
    {"OWNER_SW_CFG", 0x1D0, 672, false, false, true, "OWNER_SW_CFG_READ_LOCK", false, false},
    {"ROT_CREATOR_AUTH_CODESIGN", 0x470, 472, false, false, true,
     "ROT_CREATOR_AUTH_CODESIGN_READ_LOCK", false, false},
    {"ROT_CREATOR_AUTH_STATE", 0x648, 40, false, false, true, "ROT_CREATOR_AUTH_STATE_READ_LOCK",
     false, false},
    {"HW_CFG0", 0x670, 72, false, true, true, nullptr, false, true},
    {"HW_CFG1", 0x6B8, 16, false, true, true, nullptr, false, true},
    {"SECRET0", 0x6C8, 48, true, true, true, nullptr, true, true},
    {"SECRET1", 0x6F8, 96, true, true, true, nullptr, true, true},
    {"SECRET2", 0x758, 96, true, true, true, nullptr, true, true},
    {"LIFE_CYCLE", 0x7B8, 88, false, false, false, nullptr, false, true},
};
inline constexpr int kEg2OtpNumParts = 11;
inline constexpr int kEg2OtpHwCfg0 = 5, kEg2OtpHwCfg1 = 6;
inline constexpr int kEg2OtpSecret0 = 7, kEg2OtpSecret1 = 8, kEg2OtpSecret2 = 9;
inline constexpr int kEg2OtpLifeCycle = 10;
inline constexpr uint32_t kEg2OtpSize = 0x810;
// Items (otp_ctrl_mmap.md).
inline constexpr uint32_t kEg2OtpDeviceId = 0x670, kEg2OtpManufState = 0x690;
inline constexpr uint32_t kEg2OtpEnSramIfetch = 0x6B8, kEg2OtpDisRvDmLateDebug = 0x6B9;
inline constexpr uint32_t kEg2OtpTestUnlockToken = 0x6C8, kEg2OtpTestExitToken = 0x6D8;
inline constexpr uint32_t kEg2OtpNvmAddrKeySeed = 0x6F8, kEg2OtpNvmDataKeySeed = 0x718;
inline constexpr uint32_t kEg2OtpSramDataKeySeed = 0x738;
inline constexpr uint32_t kEg2OtpRmaToken = 0x758;
inline constexpr uint32_t kEg2OtpCreatorRootKeyShare0 = 0x768, kEg2OtpCreatorRootKeyShare1 = 0x788;
inline constexpr uint32_t kEg2OtpLcTransitionCnt = 0x7B8, kEg2OtpLcState = 0x7E8;
// The digest of a partition: its last 8 bytes, or the 8 before its zeroization marker.
inline constexpr uint32_t eg2_otp_digest_off(const Eg2OtpPart &pt) {
  return pt.off + pt.size - (pt.zer ? 16 : 8);
}
inline constexpr uint32_t eg2_otp_zer_off(const Eg2OtpPart &pt) { return pt.off + pt.size - 8; }

}  // namespace eg2_model
#endif  // EG2_MODEL_IP_EG2_OTP_PARTITIONS_H_
