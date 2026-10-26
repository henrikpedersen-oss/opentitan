// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP AES-GCM vectors through the crypto library's software GCM (chip_sw_eg2_nist_aes_gcm,
 * REQ_PBF_10, REQ_PBF_23): SP 800-38D authenticated encryption and decryption with the crypto
 * library's `aes_gcm_encrypt` and `aes_gcm_decrypt` (sw/device/lib/crypto/impl/aes_gcm: GHASH in
 * software, the AES engine in ECB/CTR), AES-128/192/256, in both execution modes.
 *
 * EG2 has GCM in software only: the EG2 High-Level Specification lists "GCM mode through software
 * on Ibex" under AES, and the top builds its AES with `AESGCMEnable: "0"` (top_earlgrey.hjson).
 * The test drove the engine's GCM phases until 2026-10-26; on EG2 that mode resolves to none and
 * every vector waited out its polls (regr-vlt TIMEOUT, Xcelium killed at 8 h).
 *
 * Encryption: the ciphertext and the first Taglen bytes of the 128-bit tag equal the NIST values.
 * Decryption: the plaintext equals the NIST value and the tag authenticates; a vector the file
 * marks FAIL does not. The 96-bit IVs run; the files' 1024- and 8-bit IVs are reported not run:
 * the library takes 96 or 128 bits only (aes_gcm.h), where SP 800-38D allows any length. Decryption with a
 * tag of 104, 112 or 120 bits is skipped and reported: the library compares the tag in whole
 * 32-bit words, so it cannot verify a tag truncated inside a word (SP 800-38D allows them; a
 * finding for the crypto library owners, F52, as aes_kwp's short plaintexts in eg2_nist_aes_kw.c).
 *
 * `model_gaps` 1 (eg2sim only) skips every vector: the crypto library's AES driver waits for
 * rv_core_ibex's RND_STATUS.RND_DATA_VALID, which the SystemC model never sets. With
 * EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail.
 */

#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/drivers/aes.h"
#include "sw/device/lib/crypto/impl/aes_gcm/aes_gcm.h"
#include "sw/device/lib/crypto/include/datatypes.h"
#include "sw/device/lib/crypto/include/integrity.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_AES_GCM_PARAMS(X)  \
  X(random_vectors, 8, 0, 256)  \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(NIST_AES_GCM_PARAMS);

enum {
  kMaxText = 64,     // bytes: the longest plaintext in the files is 408 bits (51 bytes)
  kMaxIvWords = 4,  // 96 or 128 bits: all the library takes
  kTagWords = 4,
};

static const uint32_t kZeroShare[8] = {0};

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  if (EG2_PARAM(model_gaps)) {
    return eg2_nist_skip(v, "model_gap: rv_core_ibex RND_STATUS never valid in the SystemC model");
  }
  bool dec = v->flags & kEg2NistDecrypt, must_fail = v->flags & kEg2NistFail;
  uint32_t klen = eg2_nist_len(v, kEg2NistKey), ivlen = eg2_nist_len(v, kEg2NistIv);
  uint32_t n = eg2_nist_len(v, kEg2NistIn), alen = eg2_nist_len(v, kEg2NistAad);
  uint32_t tlen = eg2_nist_len(v, kEg2NistTag);
  if (klen % 4 != 0 || klen > 32 || ivlen % 4 != 0 || ivlen / 4 > kMaxIvWords || n > kMaxText ||
      tlen == 0 || tlen > 16) {
    return eg2_nist_result(eg2_nist_error(v, "unsupported key, IV, text or tag length"));
  }
  if (dec && tlen % 4 != 0) {
    return eg2_nist_skip(v, "tag of 104/112/120 bits: aes_gcm_decrypt compares whole words");
  }

  uint32_t key[8], iv[kMaxIvWords], tag[kTagWords];
  memset(key, 0, sizeof(key));
  memcpy(key, eg2_nist_data(v, kEg2NistKey), klen);
  memcpy(iv, eg2_nist_data(v, kEg2NistIv), ivlen);
  aes_key_t aes_key = {
      .mode = kAesCipherModeCtr,
      .sideload = kHardenedBoolFalse,
      .key_len = klen / 4,
      .key_shares = {key, kZeroShare},
  };
  aes_key.checksum = aes_key_integrity_checksum(&aes_key);
  otcrypto_const_word32_buf_t iv_buf =
      OTCRYPTO_MAKE_BUF(otcrypto_const_word32_buf_t, iv, ivlen / 4);
  otcrypto_const_byte_buf_t in_buf =
      OTCRYPTO_MAKE_BUF(otcrypto_const_byte_buf_t, eg2_nist_data(v, kEg2NistIn), n);
  otcrypto_const_byte_buf_t aad_buf =
      OTCRYPTO_MAKE_BUF(otcrypto_const_byte_buf_t, eg2_nist_data(v, kEg2NistAad), alen);
  // A sentinel after the text, to see that nothing is written past it.
  uint8_t out[kMaxText + 1];
  memset(out, 0xa5, sizeof(out));
  otcrypto_byte_buf_t out_buf = OTCRYPTO_MAKE_BUF(otcrypto_byte_buf_t, out, n);

  if (!dec) {
    otcrypto_word32_buf_t tag_buf = OTCRYPTO_MAKE_BUF(otcrypto_word32_buf_t, tag, kTagWords);
    if (!status_ok(aes_gcm_encrypt(aes_key, &iv_buf, &in_buf, &aad_buf, &tag_buf,
                                   kOtcryptoKeySecurityLevelLow, &out_buf))) {
      return eg2_nist_result(eg2_nist_error(v, "aes_gcm_encrypt returned an error"));
    }
    bool ok = n == 0 || eg2_nist_expect(v, kEg2NistOut, out, n, corrupt);
    if (out[n] != 0xa5) {
      ok = eg2_nist_error(v, "aes_gcm_encrypt wrote past the ciphertext");
    }
    // NIST's Tag is the leftmost Taglen bits of the 128-bit tag (SP 800-38D 7.1, MSB_t).
    return eg2_nist_result(
        eg2_nist_expect(v, kEg2NistTag, (const uint8_t *)tag, tlen, corrupt && n == 0) && ok);
  }

  memcpy(tag, eg2_nist_data(v, kEg2NistTag), tlen);
  if (corrupt && !must_fail && n == 0) {
    // No plaintext to corrupt the expectation of: corrupt the tag instead, which must then not
    // authenticate ("valid vector did not authenticate" below).
    ((uint8_t *)tag)[0] ^= 1;
  }
  otcrypto_const_word32_buf_t tag_buf =
      OTCRYPTO_MAKE_BUF(otcrypto_const_word32_buf_t, tag, tlen / 4);
  hardened_bool_t success = kHardenedBoolFalse;
  if (!status_ok(aes_gcm_decrypt(aes_key, &iv_buf, &in_buf, &aad_buf, &tag_buf, &out_buf,
                                 kOtcryptoKeySecurityLevelLow, &success))) {
    return eg2_nist_result(eg2_nist_error(v, "aes_gcm_decrypt returned an error"));
  }
  if (must_fail) {
    // The tag over this ciphertext differs from the file's: the decryption must not authenticate.
    // With `corrupt` the expectation is reversed, so the inject-fail build fails here too.
    bool refused = success != kHardenedBoolTrue;
    if (refused == corrupt) {
      return eg2_nist_result(eg2_nist_error(
          v, corrupt ? "FAIL vector refused (expectation reversed, -D..._INJECT_FAIL)"
                     : "a FAIL vector authenticated"));
    }
    return kEg2NistPassed;
  }
  if (success != kHardenedBoolTrue) {
    return eg2_nist_result(eg2_nist_error(v, "valid vector did not authenticate"));
  }
  bool ok = n == 0 || eg2_nist_expect(v, kEg2NistOut, out, n, corrupt);
  if (out[n] != 0xa5) {
    ok = eg2_nist_error(v, "aes_gcm_decrypt wrote past the plaintext");
  }
  return eg2_nist_result(ok);
}

bool eg2_test_main(void) {
  // The crypto library's AES driver takes masks and random orders from rv_core_ibex's RND (as in
  // eg2_nist_aes_kw.c): EDN0 in auto mode first, or wait_rnd_valid can spin for ever.
  if (!EG2_PARAM(model_gaps) && !status_ok(entropy_testutils_auto_mode_init())) {
    eg2_puts("NIST aes_gcm: entropy complex not in auto mode\r\n");
    return false;
  }
  return eg2_nist_run("aes_gcm", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
