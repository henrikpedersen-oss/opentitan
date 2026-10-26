// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP AES-GCM vectors on the AES engine (chip_sw_eg2_nist_aes_gcm, REQ_PBF_10,
 * REQ_PBF_23): encryption and decryption with 96-bit IVs (the hardware's fixed IV length), every
 * plaintext, AAD and tag length the files have, AES-128/192/256, in both execution modes.
 *
 * The phases follow the AES programmer's guide: INIT (dif_aes_start), AAD blocks, then text blocks,
 * each phase entered with 16 valid bytes and set again before a partial last block, every phase
 * write while the unit is idle, then the tag from len(AAD) || len(text). The
 * output and the first Taglen bytes of the tag are compared with the NIST values; a decryption
 * the file marks FAIL must not authenticate. The scoreboard checks every instruction. With
 * EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must fail.
 */

#include "hw/top/aes_regs.h"  // Generated.
#include "hw/top/dt/aes.h"    // Generated.
#include "hw/top/dt/api.h"    // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_aes.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_AES_GCM_PARAMS(X) X(random_vectors, 8, 0, 256)
EG2_TEST_PARAMS(NIST_AES_GCM_PARAMS);

enum {
  kPolls = 100000,
  kMaxText = 64,  // bytes: the longest plaintext in the 96-bit-IV sections is 51
};

static dif_aes_t aes;

static bool wait_status(dif_aes_status_t flag) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    bool set = false;
    if (dif_aes_get_status(&aes, flag, &set) != kDifOk) {
      return false;
    }
    if (set) {
      return true;
    }
  }
  return false;
}

// The phase register takes writes only while the unit is idle (aes.hjson CTRL_GCM_SHADOWED).
static bool set_phase(uint32_t phase, uint32_t valid) {
  return wait_status(kDifAesStatusIdle) && dif_aes_set_gcm_phase(&aes, phase, valid) == kDifOk;
}

// Loads `len` bytes as zero-padded blocks in `phase`; for text, reads each output block. The
// phase is entered with 16 valid bytes, and set again only before a partial last block
// (programmer's guide).
static bool blocks(uint32_t phase, const uint8_t *in, uint32_t len, uint8_t *out) {
  if (len > 0 && !set_phase(phase, 16)) {
    return false;
  }
  for (uint32_t off = 0; off < len; off += 16) {
    uint32_t valid = len - off < 16 ? len - off : 16;
    if (valid < 16 && !set_phase(phase, valid)) {
      return false;
    }
    dif_aes_data_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.data, in + off, valid);
    if (!wait_status(kDifAesStatusInputReady) || dif_aes_load_data(&aes, d) != kDifOk) {
      return false;
    }
    if (out != NULL) {
      dif_aes_data_t o;
      if (!wait_status(kDifAesStatusOutputValid) || dif_aes_read_output(&aes, &o) != kDifOk) {
        return false;
      }
      memcpy(out + off, o.data, valid);
    }
  }
  return true;
}

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  bool dec = v->flags & kEg2NistDecrypt, must_fail = v->flags & kEg2NistFail;
  uint32_t klen = eg2_nist_len(v, kEg2NistKey), n = eg2_nist_len(v, kEg2NistIn);
  uint32_t alen = eg2_nist_len(v, kEg2NistAad), tlen = eg2_nist_len(v, kEg2NistTag);
  if (eg2_nist_len(v, kEg2NistIv) != 12 || n > kMaxText || tlen > 16) {
    return eg2_nist_result(eg2_nist_error(v, "unsupported IV, text or tag length"));
  }
  dif_aes_transaction_t txn = {
      .operation = dec ? kDifAesOperationDecrypt : kDifAesOperationEncrypt,
      .mode = kDifAesModeGcm,
      .key_len = klen == 16 ? kDifAesKey128 : klen == 24 ? kDifAesKey192 : kDifAesKey256,
      .manual_operation = kDifAesManualOperationAuto,
      .key_provider = kDifAesKeySoftwareProvided,
      .mask_reseeding = kDifAesReseedPerBlock,
      .reseed_on_key_change = false,
      .force_masks = false,
      .ctrl_aux_lock = false,
  };
  dif_aes_key_share_t key;
  memset(&key, 0, sizeof(key));
  memcpy(key.share0, eg2_nist_data(v, kEg2NistKey), klen);
  dif_aes_iv_t iv;
  memset(&iv, 0, sizeof(iv));  // IV_3 = 0: the hardware's counter starts there
  memcpy(iv.iv, eg2_nist_data(v, kEg2NistIv), 12);

  uint8_t out[kMaxText];
  if (!wait_status(kDifAesStatusIdle) || dif_aes_start(&aes, &txn, &key, &iv) != kDifOk) {
    return eg2_nist_result(eg2_nist_error(v, "AES did not start"));
  }
  if (!blocks(AES_CTRL_GCM_SHADOWED_PHASE_VALUE_GCM_AAD, eg2_nist_data(v, kEg2NistAad), alen,
              NULL) ||
      !blocks(AES_CTRL_GCM_SHADOWED_PHASE_VALUE_GCM_TEXT, eg2_nist_data(v, kEg2NistIn), n, out)) {
    return eg2_nist_result(eg2_nist_error(v, "AES AAD or text phase"));
  }
  dif_aes_data_t tag;
  if (!set_phase(AES_CTRL_GCM_SHADOWED_PHASE_VALUE_GCM_TAG, 16) ||
      !wait_status(kDifAesStatusInputReady) ||
      dif_aes_load_gcm_tag_len(&aes, (uint64_t)n * 8, (uint64_t)alen * 8) != kDifOk ||
      !wait_status(kDifAesStatusOutputValid) || dif_aes_read_output(&aes, &tag) != kDifOk ||
      dif_aes_end(&aes) != kDifOk) {
    return eg2_nist_result(eg2_nist_error(v, "AES tag phase"));
  }
  const uint8_t *got_tag = (const uint8_t *)tag.data;
  if (must_fail) {
    // The tag over this ciphertext differs from the file's: decryption must not authenticate.
    bool authenticates = memcmp(got_tag, eg2_nist_data(v, kEg2NistTag), tlen) == 0;
    if (authenticates != corrupt) {
      return eg2_nist_result(eg2_nist_error(
          v, corrupt ? "FAIL vector expected to authenticate (-D..._INJECT_FAIL)"
                     : "a FAIL vector authenticated"));
    }
    return kEg2NistPassed;
  }
  bool ok = n == 0 || eg2_nist_expect(v, kEg2NistOut, out, n, corrupt);
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistTag, got_tag, tlen, corrupt && n == 0) && ok);
}

bool eg2_test_main(void) {
  if (dif_aes_init_from_dt(kDtAes, &aes) != kDifOk || dif_aes_reset(&aes) != kDifOk) {
    eg2_puts("AES init failed\r\n");
    return false;
  }
  return eg2_nist_run("aes_gcm", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
