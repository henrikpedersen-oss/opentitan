// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP HMAC vectors on the hmac block (chip_sw_eg2_nist_hmac, REQ_PBF_10, REQ_PBF_23):
 * HMAC with SHA-256, SHA-384 and SHA-512 (HMAC.rsp, L = 32, 48, 64), every key length class and
 * tag length, in both execution modes.
 *
 * A key is zero-padded to the next supported key length, or hashed first on the engine when it is
 * longer than the block, as the HMAC theory of operation requires. The first Tlen bytes of the MAC
 * are compared with the NIST value; the scoreboard checks every instruction. With
 * EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"
#include "sw/device/tests/eg2_nist/eg2_nist_sha2_engine.h"

EG2_TEST_CONFIG();

#define NIST_HMAC_PARAMS(X)     \
  X(boundary_vectors, 8, 0, 1024) \
  X(random_vectors, 12, 0, 256) \
  X(max_burst, 16, 1, 64)
EG2_TEST_PARAMS(NIST_HMAC_PARAMS);

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  uint8_t mac[64];
  if (!eg2_nist_sha2_run(v->param, eg2_nist_data(v, kEg2NistKey), eg2_nist_len(v, kEg2NistKey),
                         eg2_nist_data(v, kEg2NistMsg), eg2_nist_len(v, kEg2NistMsg),
                         EG2_PARAM(max_burst), mac)) {
    return eg2_nist_result(eg2_nist_error(v, "engine"));
  }
  return eg2_nist_result(
      eg2_nist_expect(v, kEg2NistMac, mac, eg2_nist_len(v, kEg2NistMac), corrupt));
}

bool eg2_test_main(void) {
  eg2_nist_sha2_init();
  return eg2_nist_run("hmac", run_vector, EG2_PARAM(boundary_vectors), EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
