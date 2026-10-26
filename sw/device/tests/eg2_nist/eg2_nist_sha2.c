// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP SHA-2 vectors on the hmac block (chip_sw_eg2_nist_sha2, REQ_PBF_10, REQ_PBF_23):
 * SHA-256, SHA-384 and SHA-512 short and long messages, and the first COUNT of each Monte Carlo
 * file (1000 chained hashes; `monte` 1), in both execution modes.
 *
 * The message goes to the FIFO in bursts of 1 to `max_burst` words drawn with the seed. Each
 * digest is compared with the NIST value, and the scoreboard checks every instruction. With
 * EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"
#include "sw/device/tests/eg2_nist/eg2_nist_sha2_engine.h"

EG2_TEST_CONFIG();

#define NIST_SHA2_PARAMS(X)    \
  X(random_vectors, 8, 0, 256) \
  X(max_burst, 16, 1, 64)      \
  X(monte, 0, 0, 1)
EG2_TEST_PARAMS(NIST_SHA2_PARAMS);

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  uint32_t bits = v->param;
  uint8_t md[64];
  if (v->flags & kEg2NistMonte) {
    if (!EG2_PARAM(monte)) {
      return eg2_nist_skip(v, "Monte Carlo with monte 0");
    }
    // SHAVS 6.4: MD0 = MD1 = MD2 = Seed; MDi = SHA(MDi-3 || MDi-2 || MDi-1), i = 3 .. 1002.
    uint32_t n = bits / 8;
    uint8_t m[3 * 64];
    for (uint32_t k = 0; k < 3; ++k) {
      for (uint32_t b = 0; b < n; ++b) {
        m[k * n + b] = eg2_nist_data(v, kEg2NistMsg)[b];
      }
    }
    for (uint32_t i = 3; i < 1003; ++i) {
      if (!eg2_nist_sha2_run(bits, NULL, 0, m, 3 * n, EG2_PARAM(max_burst), md)) {
        return eg2_nist_result(eg2_nist_error(v, "engine"));
      }
      for (uint32_t b = 0; b < 2 * n; ++b) {
        m[b] = m[b + n];
      }
      for (uint32_t b = 0; b < n; ++b) {
        m[2 * n + b] = md[b];
      }
    }
  } else if (!eg2_nist_sha2_run(bits, NULL, 0, eg2_nist_data(v, kEg2NistMsg),
                                eg2_nist_len(v, kEg2NistMsg), EG2_PARAM(max_burst), md)) {
    return eg2_nist_result(eg2_nist_error(v, "engine"));
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistMd, md, bits / 8, corrupt));
}

bool eg2_test_main(void) {
  eg2_nist_sha2_init();
  return eg2_nist_run("sha2", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
