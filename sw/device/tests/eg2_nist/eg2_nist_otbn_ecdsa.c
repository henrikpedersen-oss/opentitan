// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP ECDSA on OTBN (chip_sw_eg2_nist_otbn_ecdsa, REQ_PBF_11, REQ_PBF_23): FIPS 186-4
 * SigVer.rsp (verify) and SigGen.txt (sign with the vector's k), P-256 and P-384 with SHA-256 and
 * SHA-384, through the crypto library's OTBN drivers (sw/device/lib/crypto/impl/ecc/p256.c,
 * p384.c) in both execution modes, as the OTBN reference checker drives the same programs on the
 * ISS (`make nist-otbn-ref`, eg2_model/nist/nist_otbn_check.py):
 *   - verify: `p<n>_ecdsa_verify_start` with the digest, R, S and Q; the signature is valid only
 *     if `p<n>_ecdsa_verify_finalize` returns OK and `result` true (the driver's rule: OTBN's `ok`
 *     and x_r = r); the verdict must be the file's Result;
 *   - sign: `p<n>_ecdsa_sign_config_k_start` with d and k as the driver's arithmetic shares, and
 *     `p<n>_ecdsa_sign_finalize`; R and S equal the NIST values.
 * The digest (the hash's leftmost bits, as many as n has) and the shares are prepared on the host
 * by gen_nist_vectors.py; OTBN computes every value checked. The driver traps on an instruction
 * count it does not accept, which fails the run.
 *
 * Knobs: `per_group` vectors of each group (curve, operation, expected result) drawn with the
 * seed among the boundary vectors; `random_vectors` more from the pool; `sign` 0 leaves the
 * signing vectors out (reported skipped); `model_gaps` 1 in eg2sim, which has no OTBN.
 *
 * With EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail
 * ("NIST ... mismatch").
 */

#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/impl/ecc/p256.h"
#include "sw/device/lib/crypto/impl/ecc/p384.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"
#include "sw/device/tests/eg2_nist/eg2_nist_otbn.h"

EG2_TEST_CONFIG();

#define NIST_OTBN_ECDSA_PARAMS(X) \
  X(per_group, 1, 0, 64)          \
  X(random_vectors, 0, 0, 64)     \
  X(sign, 1, 0, 1)                \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(NIST_OTBN_ECDSA_PARAMS);

/** The verdict as a byte, so eg2_nist_expect compares (and can corrupt) it. */
static eg2_nist_result_t verdict(const eg2_nist_vec_t *v, bool valid, bool corrupt) {
  uint8_t got = valid ? 1 : 0;
  // The expected verdict as the vector says: 1 valid, 0 invalid, compared against a byte that
  // the generator does not provide, so it is built here from the FAIL flag.
  uint8_t exp = (v->flags & kEg2NistFail) ? 0 : 1;
  if (corrupt) {
    exp ^= 1;
  }
  if (got != exp) {
    return eg2_nist_result(eg2_nist_error(
        v, corrupt ? "verdict differs from the expected one (corrupted: -D..._INJECT_FAIL)"
                   : valid ? "invalid signature accepted" : "valid signature refused"));
  }
  return kEg2NistPassed;
}

/**
 * The digest as the drivers take it: the hash's leftmost bytes in order (big-endian;
 * p256.c/p384.c `set_message_digest` reverse them into OTBN's little-endian `msg`). The table
 * holds e little-endian, as the OTBN reference checker writes `msg` straight into DMEM; passed
 * as it was, the driver reversed it into another e (regr-vlt 2026-10-26: every S wrong with R
 * right, every valid signature refused, ECDH and RSA passing). 2026-10-26.
 */
static void digest_words(const eg2_nist_vec_t *v, uint32_t *digest, uint32_t nwords) {
  uint32_t le[kP384ScalarWords];
  eg2_nist_otbn_words(v, kEg2NistEcdsaE, le, nwords);
  for (uint32_t i = 0; i < nwords; ++i) {
    digest[i] = __builtin_bswap32(le[nwords - 1 - i]);
  }
}

static eg2_nist_result_t run_p256(const eg2_nist_vec_t *v, bool corrupt) {
  uint32_t digest[kP256ScalarWords];
  digest_words(v, digest, kP256ScalarWords);
  if (v->flags & kEg2NistSign) {
    p256_masked_scalar_t d, k;
    eg2_nist_otbn_words(v, kEg2NistEcdsaD0, d.share0, kP256MaskedScalarShareWords);
    eg2_nist_otbn_words(v, kEg2NistEcdsaD1, d.share1, kP256MaskedScalarShareWords);
    eg2_nist_otbn_words(v, kEg2NistEcdsaK0, k.share0, kP256MaskedScalarShareWords);
    eg2_nist_otbn_words(v, kEg2NistEcdsaK1, k.share1, kP256MaskedScalarShareWords);
    d.checksum = p256_masked_scalar_checksum(&d);
    k.checksum = p256_masked_scalar_checksum(&k);
    p256_ecdsa_signature_t sig;
    if (!status_ok(p256_ecdsa_sign_config_k_start(digest, &d, &k)) ||
        !status_ok(p256_ecdsa_sign_finalize(&sig))) {
      return eg2_nist_result(eg2_nist_error(v, "P-256 signing returned an error"));
    }
    bool ok = eg2_nist_expect(v, kEg2NistEcdsaR, (const uint8_t *)sig.r, sizeof(sig.r), corrupt);
    return eg2_nist_result(
        eg2_nist_expect(v, kEg2NistEcdsaS, (const uint8_t *)sig.s, sizeof(sig.s), false) && ok);
  }
  p256_ecdsa_signature_t sig;
  p256_point_t q;
  eg2_nist_otbn_words(v, kEg2NistEcdsaR, sig.r, kP256ScalarWords);
  eg2_nist_otbn_words(v, kEg2NistEcdsaS, sig.s, kP256ScalarWords);
  eg2_nist_otbn_words(v, kEg2NistEcdsaQx, q.x, kP256CoordWords);
  eg2_nist_otbn_words(v, kEg2NistEcdsaQy, q.y, kP256CoordWords);
  hardened_bool_t result = kHardenedBoolFalse;
  bool ok = status_ok(p256_ecdsa_verify_start(&sig, digest, &q)) &&
            status_ok(p256_ecdsa_verify_finalize(&sig, &result));
  return verdict(v, ok && result == kHardenedBoolTrue, corrupt);
}

static eg2_nist_result_t run_p384(const eg2_nist_vec_t *v, bool corrupt) {
  uint32_t digest[kP384ScalarWords];
  digest_words(v, digest, kP384ScalarWords);
  if (v->flags & kEg2NistSign) {
    p384_masked_scalar_t d, k;
    eg2_nist_otbn_words(v, kEg2NistEcdsaD0, d.share0, kP384MaskedScalarShareWords);
    eg2_nist_otbn_words(v, kEg2NistEcdsaD1, d.share1, kP384MaskedScalarShareWords);
    eg2_nist_otbn_words(v, kEg2NistEcdsaK0, k.share0, kP384MaskedScalarShareWords);
    eg2_nist_otbn_words(v, kEg2NistEcdsaK1, k.share1, kP384MaskedScalarShareWords);
    d.checksum = p384_masked_scalar_checksum(&d);
    k.checksum = p384_masked_scalar_checksum(&k);
    p384_ecdsa_signature_t sig;
    if (!status_ok(p384_ecdsa_sign_config_k_start(digest, &d, &k)) ||
        !status_ok(p384_ecdsa_sign_finalize(&sig))) {
      return eg2_nist_result(eg2_nist_error(v, "P-384 signing returned an error"));
    }
    bool ok = eg2_nist_expect(v, kEg2NistEcdsaR, (const uint8_t *)sig.r, sizeof(sig.r), corrupt);
    return eg2_nist_result(
        eg2_nist_expect(v, kEg2NistEcdsaS, (const uint8_t *)sig.s, sizeof(sig.s), false) && ok);
  }
  p384_ecdsa_signature_t sig;
  p384_point_t q;
  eg2_nist_otbn_words(v, kEg2NistEcdsaR, sig.r, kP384ScalarWords);
  eg2_nist_otbn_words(v, kEg2NistEcdsaS, sig.s, kP384ScalarWords);
  eg2_nist_otbn_words(v, kEg2NistEcdsaQx, q.x, kP384CoordWords);
  eg2_nist_otbn_words(v, kEg2NistEcdsaQy, q.y, kP384CoordWords);
  hardened_bool_t result = kHardenedBoolFalse;
  bool ok = status_ok(p384_ecdsa_verify_start(&sig, digest, &q)) &&
            status_ok(p384_ecdsa_verify_finalize(&sig, &result));
  return verdict(v, ok && result == kHardenedBoolTrue, corrupt);
}

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  if (EG2_PARAM(model_gaps)) {
    return eg2_nist_otbn_skip(v, true);
  }
  if ((v->flags & kEg2NistSign) && !EG2_PARAM(sign)) {
    return eg2_nist_skip(v, "signing off (knob sign = 0)");
  }
  if (!eg2_nist_otbn_take(v)) {
    return eg2_nist_otbn_skip(v, false);
  }
  return v->param == 256 ? run_p256(v, corrupt) : run_p384(v, corrupt);
}

bool eg2_test_main(void) {
  // The crypto library's OTBN and hardening code take random words from rv_core_ibex's RND: the
  // entropy complex in auto mode first, as eg2_nist_aes_kw.c, not the test ROM's one boot-time EDN
  // generate, which more vectors can exhaust (review 2026-10-26). Not in eg2sim (model_gaps).
  if (!EG2_PARAM(model_gaps) && !status_ok(entropy_testutils_auto_mode_init())) {
    eg2_puts("NIST otbn_ecdsa: entropy complex not in auto mode\r\n");
    return false;
  }
  eg2_nist_otbn_budget_init(EG2_PARAM(per_group));
  return eg2_nist_run("otbn_ecdsa", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
