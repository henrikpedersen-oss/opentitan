// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP AES known-answer vectors on the AES engine (chip_sw_eg2_nist_aes, REQ_PBF_10,
 * REQ_PBF_23): ECB, CBC, CFB-128 and OFB, AES-128/192/256, encryption and decryption, through
 * the AES DIF in both execution modes.
 *
 * Every boundary vector of each KAT file runs, then `random_vectors` more drawn with the seed. A
 * vector runs in automatic or manual operation (`manual_pct` percent manual, drawn per vector);
 * the result is compared with the NIST value, and the chip bench's scoreboard checks every
 * instruction. With EG2_NIST_INJECT_FAIL the first expected value is corrupted and the run must
 * fail ("NIST ... mismatch").
 */

#include "hw/top/dt/api.h"  // Generated.
#include "hw/top/dt/aes.h"  // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_aes.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_AES_PARAMS(X)     \
  X(random_vectors, 16, 0, 512) \
  X(manual_pct, 25, 0, 100)
EG2_TEST_PARAMS(NIST_AES_PARAMS);

enum {
  kPolls = 100000,
  kMaxBlocks = 4,
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

static dif_aes_key_length_t key_len(uint32_t bytes) {
  return bytes == 16 ? kDifAesKey128 : bytes == 24 ? kDifAesKey192 : kDifAesKey256;
}

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  uint32_t klen = eg2_nist_len(v, kEg2NistKey), n = eg2_nist_len(v, kEg2NistIn);
  if (n == 0 || n % 16 != 0 || n / 16 > kMaxBlocks) {
    return eg2_nist_result(eg2_nist_error(v, "unsupported length"));
  }
  bool manual = eg2_rand_range(0, 99) < EG2_PARAM(manual_pct);
  dif_aes_transaction_t txn = {
      .operation = (v->flags & kEg2NistDecrypt) ? kDifAesOperationDecrypt
                                                : kDifAesOperationEncrypt,
      .mode = (dif_aes_mode_t)v->param,
      .key_len = key_len(klen),
      .manual_operation = manual ? kDifAesManualOperationManual : kDifAesManualOperationAuto,
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
  memset(&iv, 0, sizeof(iv));
  memcpy(iv.iv, eg2_nist_data(v, kEg2NistIv), eg2_nist_len(v, kEg2NistIv));

  if (!wait_status(kDifAesStatusIdle) ||
      dif_aes_start(&aes, &txn, &key, txn.mode == kDifAesModeEcb ? NULL : &iv) != kDifOk) {
    return eg2_nist_result(eg2_nist_error(v, "AES did not start"));
  }
  uint8_t out[16 * kMaxBlocks];
  const uint8_t *in = eg2_nist_data(v, kEg2NistIn);
  for (uint32_t b = 0; b < n / 16; ++b) {
    dif_aes_data_t din, dout;
    memcpy(din.data, in + 16 * b, 16);
    if (!wait_status(kDifAesStatusInputReady) || dif_aes_load_data(&aes, din) != kDifOk) {
      return eg2_nist_result(eg2_nist_error(v, "AES input not taken"));
    }
    if (manual && dif_aes_trigger(&aes, kDifAesTriggerStart) != kDifOk) {
      return eg2_nist_result(eg2_nist_error(v, "AES manual start refused"));
    }
    if (!wait_status(kDifAesStatusOutputValid) || dif_aes_read_output(&aes, &dout) != kDifOk) {
      return eg2_nist_result(eg2_nist_error(v, "AES output not valid"));
    }
    memcpy(out + 16 * b, dout.data, 16);
  }
  if (dif_aes_end(&aes) != kDifOk) {
    return eg2_nist_result(eg2_nist_error(v, "AES did not end"));
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistOut, out, n, corrupt));
}

bool eg2_test_main(void) {
  if (dif_aes_init_from_dt(kDtAes, &aes) != kDifOk || dif_aes_reset(&aes) != kDifOk) {
    eg2_puts("AES init failed\r\n");
    return false;
  }
  return eg2_nist_run("aes", run_vector, EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
