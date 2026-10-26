// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP RSA PKCS#1 v1.5 on OTBN (chip_sw_eg2_nist_otbn_rsa, REQ_PBF_11, REQ_PBF_23): FIPS
 * 186-3 SigVer15_186-3.rsp and SigGen15_186-3.txt, 2048 and 3072 bits with SHA-256, SHA-384 and
 * SHA-512 (the files have no 4096-bit PKCS#1 v1.5 vectors), through the crypto library's OTBN
 * driver (sw/device/lib/crypto/impl/rsa/run_rsa.c), in both execution modes, as the OTBN
 * reference checker drives `run_rsa` on the ISS (eg2_model/nist/nist_otbn_check.py):
 *   - verify: `rsa_modexp_vartime_start` with S, e (0, i.e. MODEXP_F4, for e = 65537, else
 *     MODEXP_E with the vector's e: the driver's choice) and n, then `rsa_modexp_finalize_size`;
 *     the signature is valid only if the driver returns OK and s^e mod n equals the
 *     EMSA-PKCS1-v1_5 encoding; the verdict must be the file's Result;
 *   - sign (`sign` = 1 only: about 16.5 million OTBN instructions per 2048-bit signature):
 *     `rsa_modexp_consttime_start` (MODEXP_DE, the blinded constant-time modexp) with the
 *     encoding, d as two Boolean shares, e and n; the result equals S.
 * The encoding and the shares are prepared on the host by gen_nist_vectors.py; OTBN computes
 * every value checked.
 *
 * Knobs: `per_group` vectors of each group (modulus, operation, expected result) drawn with the
 * seed among the boundary vectors; `random_vectors` more; `sign`; `model_gaps` 1 in eg2sim (no
 * OTBN).
 *
 * With EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail
 * ("NIST ... mismatch" or a reversed verdict).
 */

#include "sw/device/lib/base/crc32.h"
#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/impl/rsa/run_rsa.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"
#include "sw/device/tests/eg2_nist/eg2_nist_otbn.h"

EG2_TEST_CONFIG();

#define NIST_OTBN_RSA_PARAMS(X) \
  X(per_group, 1, 0, 64)        \
  X(random_vectors, 0, 0, 64)   \
  X(sign, 0, 0, 1)              \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(NIST_OTBN_RSA_PARAMS);

enum {
  kMaxWords = kRsa3072NumWords,
};

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  if (EG2_PARAM(model_gaps)) {
    return eg2_nist_otbn_skip(v, true);
  }
  if ((v->flags & kEg2NistSign) && !EG2_PARAM(sign)) {
    return eg2_nist_skip(v, "RSA signing off (knob sign = 0; about 8 minutes per signature on "
                            "the ISS, far longer on the chip bench)");
  }
  if (!eg2_nist_otbn_take(v)) {
    return eg2_nist_otbn_skip(v, false);
  }
  rsa_size_t size = v->param == 2048 ? kRsaSize2048 : kRsaSize3072;
  uint32_t nw = v->param == 2048 ? kRsa2048NumWords : kRsa3072NumWords;
  uint32_t n[kMaxWords], base[kMaxWords], result[kMaxWords], e;
  eg2_nist_otbn_words(v, kEg2NistRsaN, n, nw);
  eg2_nist_otbn_words(v, kEg2NistRsaE, &e, 1);
  uint32_t pub_exp = e == 65537 ? 0 : e;
  memset(result, 0, sizeof(result));

  if (v->flags & kEg2NistSign) {
    uint32_t d0[kMaxWords], d1[kMaxWords];
    eg2_nist_otbn_words(v, kEg2NistRsaEm, base, nw);
    eg2_nist_otbn_words(v, kEg2NistRsaD0, d0, nw);
    eg2_nist_otbn_words(v, kEg2NistRsaD1, d1, nw);
    uint32_t checksum = crc32(d0, nw * sizeof(uint32_t));
    if (!status_ok(rsa_modexp_consttime_start(size, base, d0, d1, pub_exp, n, checksum)) ||
        !status_ok(rsa_modexp_finalize_size(size, result))) {
      return eg2_nist_result(eg2_nist_error(v, "RSA signing (MODEXP_DE) returned an error"));
    }
    return eg2_nist_result(
        eg2_nist_expect(v, kEg2NistRsaS, (const uint8_t *)result, nw * sizeof(uint32_t), corrupt));
  }

  eg2_nist_otbn_words(v, kEg2NistRsaS, base, nw);
  bool ran = status_ok(rsa_modexp_vartime_start(size, base, pub_exp, n)) &&
             status_ok(rsa_modexp_finalize_size(size, result));
  const uint8_t *em = eg2_nist_data(v, kEg2NistRsaEm);
  bool valid = ran && memcmp(result, em, nw * sizeof(uint32_t)) == 0;
  bool expect_valid = !(v->flags & kEg2NistFail);
  if (corrupt) {
    expect_valid = !expect_valid;
  }
  if (valid != expect_valid) {
    return eg2_nist_result(eg2_nist_error(
        v, corrupt ? "verdict differs from the expected one (corrupted: -D..._INJECT_FAIL)"
                   : valid ? "invalid signature accepted" : "valid signature refused"));
  }
  return kEg2NistPassed;
}

bool eg2_test_main(void) {
  eg2_nist_otbn_budget_init(EG2_PARAM(per_group));
  return eg2_nist_run("otbn_rsa", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
