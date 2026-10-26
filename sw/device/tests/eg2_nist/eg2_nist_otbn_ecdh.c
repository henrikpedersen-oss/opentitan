// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP ECC CDH primitive on OTBN (chip_sw_eg2_nist_otbn_ecdh, REQ_PBF_11, REQ_PBF_23):
 * SP 800-56A KAS_ECC_CDH_PrimitiveTest.txt, P-256 and P-384, through the crypto library's OTBN
 * drivers (`p<n>_ecdh_start` with dIUT as the driver's arithmetic shares and QCAVS, then
 * `p<n>_ecdh_finalize`), in both execution modes. The shared secret is the XOR of the two shares
 * of the x-coordinate, the driver's rule, as the OTBN reference checker reads it
 * (eg2_model/nist/nist_otbn_check.py); it must equal ZIUT.
 *
 * Knobs: `per_group` vectors per curve drawn with the seed among the boundary vectors (the first
 * and last of each curve), `random_vectors` more, `model_gaps` 1 in eg2sim (no OTBN).
 *
 * With EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail
 * ("NIST ... mismatch").
 */

#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/impl/ecc/p256.h"
#include "sw/device/lib/crypto/impl/ecc/p384.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"
#include "sw/device/tests/eg2_nist/eg2_nist_otbn.h"

EG2_TEST_CONFIG();

#define NIST_OTBN_ECDH_PARAMS(X) \
  X(per_group, 1, 0, 64)         \
  X(random_vectors, 0, 0, 64)    \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(NIST_OTBN_ECDH_PARAMS);

static eg2_nist_result_t run_p256(const eg2_nist_vec_t *v, bool corrupt) {
  p256_masked_scalar_t d;
  p256_point_t q;
  eg2_nist_otbn_words(v, kEg2NistEcdhD0, d.share0, kP256MaskedScalarShareWords);
  eg2_nist_otbn_words(v, kEg2NistEcdhD1, d.share1, kP256MaskedScalarShareWords);
  d.checksum = p256_masked_scalar_checksum(&d);
  eg2_nist_otbn_words(v, kEg2NistEcdhQx, q.x, kP256CoordWords);
  eg2_nist_otbn_words(v, kEg2NistEcdhQy, q.y, kP256CoordWords);
  p256_ecdh_shared_key_t key;
  if (!status_ok(p256_ecdh_start(&d, &q)) || !status_ok(p256_ecdh_finalize(&key))) {
    return eg2_nist_result(eg2_nist_error(v, "P-256 ECDH returned an error"));
  }
  uint32_t x[kP256CoordWords];
  for (uint32_t i = 0; i < kP256CoordWords; ++i) {
    x[i] = key.x_share0[i] ^ key.x_share1[i];
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistEcdhZ, (const uint8_t *)x, sizeof(x), corrupt));
}

static eg2_nist_result_t run_p384(const eg2_nist_vec_t *v, bool corrupt) {
  p384_masked_scalar_t d;
  p384_point_t q;
  eg2_nist_otbn_words(v, kEg2NistEcdhD0, d.share0, kP384MaskedScalarShareWords);
  eg2_nist_otbn_words(v, kEg2NistEcdhD1, d.share1, kP384MaskedScalarShareWords);
  d.checksum = p384_masked_scalar_checksum(&d);
  eg2_nist_otbn_words(v, kEg2NistEcdhQx, q.x, kP384CoordWords);
  eg2_nist_otbn_words(v, kEg2NistEcdhQy, q.y, kP384CoordWords);
  p384_ecdh_shared_key_t key;
  if (!status_ok(p384_ecdh_start(&d, &q)) || !status_ok(p384_ecdh_finalize(&key))) {
    return eg2_nist_result(eg2_nist_error(v, "P-384 ECDH returned an error"));
  }
  uint32_t x[kP384CoordWords];
  for (uint32_t i = 0; i < kP384CoordWords; ++i) {
    x[i] = key.x_share0[i] ^ key.x_share1[i];
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistEcdhZ, (const uint8_t *)x, sizeof(x), corrupt));
}

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  if (EG2_PARAM(model_gaps)) {
    return eg2_nist_otbn_skip(v, true);
  }
  if (!eg2_nist_otbn_take(v)) {
    return eg2_nist_otbn_skip(v, false);
  }
  return v->param == 256 ? run_p256(v, corrupt) : run_p384(v, corrupt);
}

bool eg2_test_main(void) {
  eg2_nist_otbn_budget_init(EG2_PARAM(per_group));
  return eg2_nist_run("otbn_ecdh", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
