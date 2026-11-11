// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_OTP_DATA_H_
#define EG2_MODEL_IP_EG2_OTP_DATA_H_

#include <cstdint>
#include "eg2_otp_partitions.h"

namespace eg2_model {

// ---- Life-cycle encodings ----
// The 16-bit OTP words of the life-cycle state (20 words) and transition count (24 words). These
// are netlist constants: the values are copied from the generated hw/ip/lc_ctrl/rtl/
// lc_ctrl_state_pkg.sv (A0..A19, B0..B19, C0..C23, D0..D23), the state and count vectors from its
// lc_state_e / lc_cnt_e (word 0 first). Copy them again if the package is regenerated.
inline constexpr uint16_t kLcA[20] = {0xa352, 0x7181, 0x39ca, 0x406b, 0x7070, 0x6a88, 0x960e,
                                      0x79e0, 0x5cd4, 0x9489, 0xc6e5, 0x97e4, 0xb403, 0x5e90,
                                      0x8c7c, 0xaab0, 0x24cd, 0x5618, 0xdb21, 0xc949};
inline constexpr uint16_t kLcB[20] = {0xb35a, 0xfbd1, 0x3ddb, 0x40ff, 0xfcf2, 0xfb9e, 0xf6ff,
                                      0x7df5, 0xfdd4, 0xb5a9, 0xc7fd, 0xbfed, 0xb763, 0xfe9d,
                                      0xeefc, 0xaffc, 0x75ef, 0x5ebd, 0xdf3f, 0xf97d};
inline constexpr uint16_t kLcC[24] = {0x3399, 0x2e0d, 0x10f5, 0x386a, 0xf4b0, 0x463d,
                                      0x6612, 0x2945, 0x3230, 0x418c, 0x7bb0, 0x11d8,
                                      0x3c40, 0xe92a, 0x14ec, 0x5622, 0xa506, 0x131b,
                                      0x6883, 0x3938, 0x28d2, 0x07c8, 0x4c44, 0x13e9};
inline constexpr uint16_t kLcD[24] = {0x73fb, 0xaf6f, 0xb1fd, 0x387f, 0xfef7, 0xe6bd,
                                      0x7732, 0xe97f, 0xfaf4, 0x59fc, 0x7ff8, 0xd5db,
                                      0xfd68, 0xef7b, 0xb6fe, 0x567b, 0xafc6, 0x9bbf,
                                      0x6bdf, 0x3f3e, 0x3ed7, 0x57cd, 0x6ccc, 0x5fff};
// Decoded states (dec_lc_state_e, the values LC_STATE and TRANSITION_TARGET use).
enum Eg2LcState : uint32_t {
  kLcRaw = 0,
  kLcTestUnlocked0 = 1,  // TEST_UNLOCKEDn = 1 + 2n, TEST_LOCKEDn = 2 + 2n
  kLcTestUnlocked7 = 15,
  kLcDev = 16,
  kLcProd = 17,
  kLcProdEnd = 18,
  kLcRma = 19,
  kLcScrap = 20,
  kLcPostTransition = 21,
  kLcEscalate = 22,
  kLcInvalid = 23,
};
// Each state's 20 words, word 0 first: 0 = zero, A = An, B = Bn.
inline constexpr const char *kLcStateWords[21] = {
    "00000000000000000000", "BAAAAAAAAAAAAAAAAAAA", "BBAAAAAAAAAAAAAAAAAA",
    "BBBAAAAAAAAAAAAAAAAA", "BBBBAAAAAAAAAAAAAAAA", "BBBBBAAAAAAAAAAAAAAA",
    "BBBBBBAAAAAAAAAAAAAA", "BBBBBBBAAAAAAAAAAAAA", "BBBBBBBBAAAAAAAAAAAA",
    "BBBBBBBBBAAAAAAAAAAA", "BBBBBBBBBBAAAAAAAAAA", "BBBBBBBBBBBAAAAAAAAA",
    "BBBBBBBBBBBBAAAAAAAA", "BBBBBBBBBBBBBAAAAAAA", "BBBBBBBBBBBBBBAAAAAA",
    "BBBBBBBBBBBBBBBAAAAA", "BBBBBBBBBBBBBBBBAAAA", "BBBBBBBBBBBBBBBABAAA",
    "BBBBBBBBBBBBBBBAABAA", "BBBBBBBBBBBBBBBBBABB", "BBBBBBBBBBBBBBBBBBBB"};

inline bool eg2_lc_test_unlocked(uint32_t s) { return s >= 1 && s <= 15 && (s & 1); }
inline bool eg2_lc_test_locked(uint32_t s) { return s >= 2 && s <= 14 && !(s & 1); }

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTP_DATA_H_
