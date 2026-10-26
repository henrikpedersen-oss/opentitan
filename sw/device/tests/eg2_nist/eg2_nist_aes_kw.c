// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP AES key wrap with padding on the AES engine through the crypto library
 * (chip_sw_eg2_nist_aes_kw, REQ_PBF_10, REQ_PBF_23): SP 800-38F KWP-AE (wrap) and KWP-AD (unwrap),
 * AES-128/192/256, with the crypto library's `aes_kwp_wrap` and `aes_kwp_unwrap`
 * (sw/device/lib/crypto/impl/aes_kwp), in both execution modes.
 *
 * A wrap's ciphertext and an unwrap's plaintext equal the NIST values; an unwrap of a vector the
 * file marks FAIL is refused (`success` false). Every boundary vector runs (the first and last of
 * each plaintext length, and the first FAIL of each), then `random_vectors` more drawn with the
 * seed.
 *
 * Not run, and reported as such by the runner (`NIST not run ...`): KW without padding (the
 * crypto library has KWP only), the TDEA files, the inverse-cipher files (`_inv`: SP 800-38F 5.1,
 * the crypto library wraps with the forward cipher), and plaintexts of at most 64 bits, which
 * `aes_kwp` refuses although SP 800-38F wraps them with a single block-cipher call (a finding
 * for the crypto library owners).
 *
 * `model_gaps` 1 (eg2sim only) skips every vector: the crypto library hardens its AES driver
 * with `ibex_rnd32_read`, which waits for rv_core_ibex's RND_STATUS.RND_DATA_VALID, and the
 * SystemC model never sets it (the EDN-fed random word of rv_core_ibex is not modelled). The
 * model run then checks nothing of this test: the chip runs and their `_inject_fail` builds are
 * its evidence.
 *
 * With EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail
 * ("NIST ... mismatch").
 */

#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/drivers/aes.h"
#include "sw/device/lib/crypto/impl/aes_kwp/aes_kwp.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_AES_KW_PARAMS(X) \
  X(random_vectors, 8, 0, 512) \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(NIST_AES_KW_PARAMS);

enum {
  // The longest vector: 4096-bit plaintexts, 520-byte ciphertexts.
  kMaxBytes = 520,
  kMaxWords = kMaxBytes / 4,
};

static const uint32_t kZeroShare[8] = {0};

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  if (EG2_PARAM(model_gaps)) {
    return eg2_nist_skip(v, "model_gap: rv_core_ibex RND_STATUS never valid in the SystemC model");
  }
  uint32_t klen = eg2_nist_len(v, kEg2NistKey);
  uint32_t nin = eg2_nist_len(v, kEg2NistIn), nout = eg2_nist_len(v, kEg2NistOut);
  bool unwrap = (v->flags & kEg2NistDecrypt) != 0;
  if (klen % 4 != 0 || klen > 32 || nin > kMaxBytes || nout > kMaxBytes) {
    return eg2_nist_result(eg2_nist_error(v, "unsupported length"));
  }
  uint32_t key[8], in[kMaxWords + 1], out[kMaxWords + 1];
  memset(key, 0, sizeof(key));
  memset(in, 0, sizeof(in));
  memcpy(key, eg2_nist_data(v, kEg2NistKey), klen);
  memcpy(in, eg2_nist_data(v, kEg2NistIn), nin);
  // A sentinel after the longest output, to see that nothing is written past it.
  memset(out, 0xa5, sizeof(out));
  aes_key_t kek = {
      .mode = kAesCipherModeEcb,
      .sideload = kHardenedBoolFalse,
      .key_len = klen / 4,
      .key_shares = {key, kZeroShare},
  };
  kek.checksum = aes_key_integrity_checksum(&kek);

  if (!unwrap) {
    if (!status_ok(aes_kwp_wrap(kek, in, nin, out))) {
      return eg2_nist_result(eg2_nist_error(v, "aes_kwp_wrap returned an error"));
    }
    bool ok = eg2_nist_expect(v, kEg2NistOut, (const uint8_t *)out, nout, corrupt);
    if (((const uint8_t *)out)[nout] != 0xa5) {
      ok = eg2_nist_error(v, "aes_kwp_wrap wrote past the ciphertext");
    }
    return eg2_nist_result(ok);
  }
  hardened_bool_t success = kHardenedBoolFalse;
  if (!status_ok(aes_kwp_unwrap(kek, in, nin, &success, out))) {
    return eg2_nist_result(eg2_nist_error(v, "aes_kwp_unwrap returned an error"));
  }
  if (v->flags & kEg2NistFail) {
    // The vector must not authenticate. With `corrupt` the expectation is reversed, so the
    // inject-fail build fails on a FAIL vector too.
    bool refused = success != kHardenedBoolTrue;
    if (refused == corrupt) {
      return eg2_nist_result(eg2_nist_error(
          v, corrupt ? "FAIL vector refused (expectation reversed, -D..._INJECT_FAIL)"
                     : "FAIL vector authenticated: aes_kwp_unwrap accepted it"));
    }
    return kEg2NistPassed;
  }
  if (success != kHardenedBoolTrue) {
    return eg2_nist_result(eg2_nist_error(v, "valid vector refused by aes_kwp_unwrap"));
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistOut, (const uint8_t *)out, nout, corrupt));
}

bool eg2_test_main(void) {
  // The crypto library's AES driver takes masks and random orders from rv_core_ibex's RND
  // (drivers/aes.c: "the caller must ensure the entropy complex is up"; upstream's functests call
  // otcrypto_init). The test ROM leaves EDN0 in boot-time mode, one generate of 4095 blocks
  // (edn.hjson BOOT_GEN_CMD) and then no more words, so wait_rnd_valid can spin for ever: the
  // 2026-10-26 runs (regr-vlt, Xcelium 8 h) printed nothing after the parameters. The SystemC
  // model skips every vector (model_gaps) and models no entropy complex.
  if (!EG2_PARAM(model_gaps) && !status_ok(entropy_testutils_auto_mode_init())) {
    eg2_puts("NIST aes_kw: entropy complex not in auto mode\r\n");
    return false;
  }
  return eg2_nist_run("aes_kw", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
