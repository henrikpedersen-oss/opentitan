// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP SHA-3 and SHAKE vectors on the kmac block (chip_sw_eg2_nist_sha3, REQ_PBF_10,
 * REQ_PBF_23): SHA3-224/256/384/512 and SHAKE128/256, short and long messages, SHAKE variable
 * output lengths, and the first COUNT of each Monte Carlo file (1000 chained operations;
 * `monte` 1), through the KMAC DIF in both execution modes.
 *
 * The digest or output is compared with the NIST value; the scoreboard checks every instruction.
 * With EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail.
 */

#include "hw/top/dt/api.h"        // Generated.
#include "hw/top/dt/kmac.h"       // Generated.
#include "hw/top/kmac_regs.h"     // Generated.
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_SHA3_PARAMS(X)    \
  X(random_vectors, 8, 0, 256) \
  X(monte, 0, 0, 1)
EG2_TEST_PARAMS(NIST_SHA3_PARAMS);

enum {
  kMaxOut = 512,  // bytes: above the longest SHAKE output in the files
};

static dif_kmac_t kmac;

// One SHA3-`bits` or SHAKE`bits` of msg, out_len bytes.
static bool keccak(bool shake, uint32_t bits, const uint8_t *msg, uint32_t len, uint8_t *out,
                   uint32_t out_len) {
  dif_kmac_operation_state_t st;
  dif_result_t r;
  if (shake) {
    r = dif_kmac_mode_shake_start(&kmac, &st,
                                  bits == 128 ? kDifKmacModeShakeLen128 : kDifKmacModeShakeLen256);
  } else {
    r = dif_kmac_mode_sha3_start(&kmac, &st,
                                 bits == 224   ? kDifKmacModeSha3Len224
                                 : bits == 256 ? kDifKmacModeSha3Len256
                                 : bits == 384 ? kDifKmacModeSha3Len384
                                               : kDifKmacModeSha3Len512);
  }
  if (r != kDifOk) {
    return false;
  }
  if (len > 0 && dif_kmac_absorb(&kmac, &st, msg, len, NULL) != kDifOk) {
    return false;
  }
  uint32_t words[kMaxOut / 4];
  uint32_t nwords = (out_len + 3) / 4;
  if (dif_kmac_squeeze(&kmac, &st, words, nwords, NULL, NULL) != kDifOk ||
      dif_kmac_end(&kmac, &st) != kDifOk ||
      dif_kmac_poll_status(&kmac, KMAC_STATUS_SHA3_IDLE_BIT) != kDifOk) {
    return false;
  }
  // The state window is read little-endian: the first output byte in bits 7:0.
  for (uint32_t i = 0; i < out_len; ++i) {
    out[i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
  }
  return true;
}

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  bool shake = v->flags & kEg2NistShake;
  uint32_t bits = v->param;
  uint32_t exp_len = eg2_nist_len(v, kEg2NistMd);
  uint8_t out[kMaxOut];
  if (exp_len > kMaxOut) {
    return eg2_nist_result(eg2_nist_error(v, "output longer than the test's buffer"));
  }
  if (!(v->flags & kEg2NistMonte)) {
    if (!keccak(shake, bits, eg2_nist_data(v, kEg2NistMsg), eg2_nist_len(v, kEg2NistMsg), out,
                exp_len)) {
      return eg2_nist_result(eg2_nist_error(v, "engine"));
    }
    return eg2_nist_result(eg2_nist_expect(v, kEg2NistMd, out, exp_len, corrupt));
  }
  if (!EG2_PARAM(monte)) {
    return eg2_nist_skip(v, "Monte Carlo with monte 0");
  }
  if (!shake) {
    // SHA3VS 6.2.3: MD0 = Seed; MDi = SHA3(MDi-1), 1000 times.
    uint32_t n = bits / 8;
    uint8_t md[64];
    for (uint32_t b = 0; b < n; ++b) {
      md[b] = eg2_nist_data(v, kEg2NistMsg)[b];
    }
    for (uint32_t i = 0; i < 1000; ++i) {
      if (!keccak(false, bits, md, n, out, n)) {
        return eg2_nist_result(eg2_nist_error(v, "engine"));
      }
      for (uint32_t b = 0; b < n; ++b) {
        md[b] = out[b];
      }
    }
    return eg2_nist_result(eg2_nist_expect(v, kEg2NistMd, md, n, corrupt));
  }
  // SHA3VS 6.3.3: Msgi = the leftmost 128 bits of Outputi-1 (zero-padded); Outputi =
  // SHAKE(Msgi, Outputlen); the next Outputlen = min + (rightmost 16 bits of Outputi) mod range.
  uint32_t min_b = v->len[kEg2NistIv], max_b = v->len[kEg2NistAad];
  uint32_t out_len = max_b, prev_len = eg2_nist_len(v, kEg2NistMsg);
  uint8_t prev[kMaxOut];
  for (uint32_t b = 0; b < prev_len && b < kMaxOut; ++b) {
    prev[b] = eg2_nist_data(v, kEg2NistMsg)[b];
  }
  if (max_b > kMaxOut || min_b > max_b) {
    return eg2_nist_result(eg2_nist_error(v, "bad Monte Carlo output bounds"));
  }
  for (uint32_t i = 0; i < 1000; ++i) {
    uint8_t m[16];
    for (uint32_t b = 0; b < 16; ++b) {
      m[b] = b < prev_len ? prev[b] : 0;
    }
    if (!keccak(true, bits, m, 16, out, out_len)) {
      return eg2_nist_result(eg2_nist_error(v, "engine"));
    }
    uint32_t right = ((uint32_t)out[out_len - 2] << 8) | out[out_len - 1];
    for (uint32_t b = 0; b < out_len; ++b) {
      prev[b] = out[b];
    }
    prev_len = out_len;
    out_len = min_b + right % (max_b - min_b + 1);
  }
  if (prev_len != exp_len) {
    return eg2_nist_result(eg2_nist_error(v, "Monte Carlo output length differs from NIST's"));
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistMd, prev, prev_len, corrupt));
}

bool eg2_test_main(void) {
  dif_kmac_config_t config = {
      .entropy_mode = kDifKmacEntropyModeSoftware,
      // Masking only: the digests do not depend on it.
      .entropy_seed = {0xb153e3fe, 0x09596819, 0x3e85a6e8, 0xb6dcdaba, 0x50dc409c, 0x11e1ebd1},
      .entropy_fast_process = kDifToggleEnabled,
  };
  if (dif_kmac_init_from_dt((dt_kmac_t)0, &kmac) != kDifOk ||
      dif_kmac_configure(&kmac, config) != kDifOk) {
    eg2_puts("KMAC init failed\r\n");
    return false;
  }
  return eg2_nist_run("sha3", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
