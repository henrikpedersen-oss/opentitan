// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

#include "hw/top/aes_regs.h"            // Generated.
#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/dt/aes.h"              // Generated.
#include "hw/top/dt/api.h"              // Generated.
#include "hw/top/dt/kmac.h"             // Generated.
#include "hw/top/hmac_regs.h"           // Generated.
#include "hw/top/keymgr_dpe_regs.h"     // Generated.
#include "hw/top/kmac_regs.h"           // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_aes.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/keymgr_dpe_testutils.h"
#include "sw/device/lib/testing/kmac_testutils.h"

enum {
  kPolls = 200000,
  // The alert crash dump (alert_crashdump_t, from its least significant member).
  kDumpStateBits = 3,
  kDumpEscCntBits = ALERT_HANDLER_PARAM_ESC_CNT_DW,
  kDumpAccumBits = ALERT_HANDLER_PARAM_ACCU_CNT_DW,
  kDumpAccumPos = 4 * kDumpStateBits + 4 * kDumpEscCntBits,
  kDumpLocCausePos = kDumpAccumPos + 4 * kDumpAccumBits,
  kDumpCausePos = kDumpLocCausePos + ALERT_HANDLER_PARAM_N_LOC_ALERT,
  kDumpBits = kDumpCausePos + ALERT_HANDLER_PARAM_N_ALERTS,
  kDumpWords = (kDumpBits + 31) / 32,
  kPhase0State = 0x4,  // CLASSA_STATE Phase0
};

static dif_aes_t aes;
static dif_kmac_t kmac;
static uint32_t keymgr_codes;

// ---------------------------------------------------------------------------------------------
// AES chains from the NIST pairs (X_i, Y_i = E_0(X_i)), key 0.

static void xor16(uint8_t *out, const uint8_t *a, const uint8_t *b) {
  for (uint32_t i = 0; i < 16; ++i) {
    out[i] = a[i] ^ b[i];
  }
}

void eg2_pbf_aes_chain(uint32_t key_idx, eg2_pbf_aes_mode_t mode, uint32_t nblocks,
                       eg2_pbf_aes_chain_t *c) {
  const eg2_pbf_kat_aes_t *k = &kEg2PbfKatAes[key_idx % 3];
  if (nblocks < 1) {
    nblocks = 1;
  }
  if (nblocks > kEg2PbfAesMaxBlocks) {
    nblocks = kEg2PbfAesMaxBlocks;
  }
  if (mode == kEg2PbfAesOfb || mode == kEg2PbfAesCtr) {
    nblocks = 1;
  }
  c->mode = mode;
  c->key_bits = k->key_bits;
  c->nblocks = nblocks;
  memset(c->iv, 0, sizeof(c->iv));
  uint8_t r[16];
  for (uint32_t i = 0; i < 16; ++i) {
    r[i] = (uint8_t)eg2_rand();
  }
  for (uint32_t i = 0; i < nblocks; ++i) {
    const uint8_t *x = eg2_pbf_kat(k->x[i]);
    const uint8_t *y = eg2_pbf_kat(k->y[i]);
    uint8_t *p = &c->pt[16 * i], *q = &c->ct[16 * i];
    switch (mode) {
      case kEg2PbfAesEcb:
        memcpy(p, x, 16);
        memcpy(q, y, 16);
        break;
      case kEg2PbfAesCbc:
        // C_i = E(P_i ^ C_i-1), C_0 = IV: P_i = X_i ^ C_i-1 makes C_i = Y_i.
        if (i == 0) {
          memcpy(c->iv, r, 16);
          xor16(p, x, c->iv);
        } else {
          xor16(p, x, eg2_pbf_kat(k->y[i - 1]));
        }
        memcpy(q, y, 16);
        break;
      case kEg2PbfAesCfb:
        // C_i = P_i ^ E(C_i-1), C_0 = IV = X_1: E(C_i-1) = Y_i when C_i-1 = X_i, so for every
        // block but the last C_i = X_i+1 and P_i = X_i+1 ^ Y_i; the last P_n is free.
        if (i == 0) {
          memcpy(c->iv, x, 16);
        }
        if (i + 1 < nblocks) {
          const uint8_t *xn = eg2_pbf_kat(k->x[i + 1]);
          xor16(p, xn, y);
          memcpy(q, xn, 16);
        } else {
          memcpy(p, r, 16);
          xor16(q, r, y);
        }
        break;
      default:
        // OFB and CTR, one block: C_1 = P_1 ^ E(IV) with IV = X_1.
        memcpy(c->iv, x, 16);
        memcpy(p, r, 16);
        xor16(q, r, y);
        break;
    }
  }
}

bool eg2_pbf_aes_init(void) {
  return dif_aes_init_from_dt(kDtAes, &aes) == kDifOk && dif_aes_reset(&aes) == kDifOk;
}

uint32_t eg2_pbf_aes_status(void) {
  return eg2_dev_read32(kEg2DevAes, AES_STATUS_REG_OFFSET);
}

static bool aes_wait(dif_aes_status_t flag) {
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

static bool aes_fail(const char *what) {
  eg2_puts("PBF aes: ");
  eg2_puts(what);
  eg2_puts(", STATUS 0x");
  eg2_puthex(eg2_pbf_aes_status());
  eg2_puts("\r\n");
  return false;
}

static const dif_aes_mode_t kDifMode[kEg2PbfAesModes] = {
    kDifAesModeEcb, kDifAesModeCbc, kDifAesModeCfb, kDifAesModeOfb, kDifAesModeCtr,
};

static bool aes_start(eg2_pbf_aes_mode_t mode, uint32_t key_bits, const uint8_t *key,
                      bool decrypt, bool manual, const uint8_t *iv) {
  dif_aes_transaction_t txn = {
      .operation = decrypt ? kDifAesOperationDecrypt : kDifAesOperationEncrypt,
      .mode = kDifMode[mode],
      .key_len = key_bits == 128 ? kDifAesKey128 : key_bits == 192 ? kDifAesKey192 : kDifAesKey256,
      .manual_operation = manual ? kDifAesManualOperationManual : kDifAesManualOperationAuto,
      .key_provider = key == NULL ? kDifAesKeySideload : kDifAesKeySoftwareProvided,
      .mask_reseeding = kDifAesReseedPerBlock,
      .reseed_on_key_change = false,
      .force_masks = false,
      .keep_start_trigger_high = false,
      .ctrl_aux_lock = false,
  };
  dif_aes_key_share_t ks;
  memset(&ks, 0, sizeof(ks));
  if (key != NULL) {
    memcpy(ks.share0, key, key_bits / 8);
  }
  dif_aes_iv_t div;
  memset(&div, 0, sizeof(div));
  if (iv != NULL) {
    memcpy(div.iv, iv, 16);
  }
  if (!aes_wait(kDifAesStatusIdle)) {
    return aes_fail("not idle");
  }
  if (dif_aes_start(&aes, &txn, key == NULL ? NULL : &ks,
                    mode == kEg2PbfAesEcb ? NULL : &div) != kDifOk) {
    return aes_fail("did not start");
  }
  return true;
}

static bool aes_block(bool manual, const uint8_t *in, uint8_t *out) {
  dif_aes_data_t din, dout;
  memcpy(din.data, in, 16);
  if (!aes_wait(kDifAesStatusInputReady) || dif_aes_load_data(&aes, din) != kDifOk) {
    return aes_fail("input not taken");
  }
  if (manual && dif_aes_trigger(&aes, kDifAesTriggerStart) != kDifOk) {
    return aes_fail("manual start refused");
  }
  if (!aes_wait(kDifAesStatusOutputValid) || dif_aes_read_output(&aes, &dout) != kDifOk) {
    return aes_fail("output not valid");
  }
  memcpy(out, dout.data, 16);
  return true;
}

bool eg2_pbf_aes_run(eg2_pbf_aes_mode_t mode, uint32_t key_bits, const uint8_t *key,
                     bool decrypt, bool manual, const uint8_t *iv, const uint8_t *in,
                     uint32_t nblocks, uint8_t *out) {
  if (!aes_start(mode, key_bits, key, decrypt, manual, iv)) {
    return false;
  }
  for (uint32_t b = 0; b < nblocks; ++b) {
    if (!aes_block(manual, in + 16 * b, out + 16 * b)) {
      return false;
    }
  }
  return dif_aes_end(&aes) == kDifOk || aes_fail("did not end");
}

bool eg2_pbf_aes_partial(uint32_t key_bits, const uint8_t *key, const uint8_t *in,
                         uint32_t stop_after, uint8_t *out) {
  if (!aes_start(kEg2PbfAesEcb, key_bits, key, false, false, NULL)) {
    return false;
  }
  for (uint32_t b = 0; b < stop_after; ++b) {
    if (!aes_block(false, in + 16 * b, out + 16 * b)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// KMAC engine.

bool eg2_pbf_kmac_init(bool sideload) {
  return dif_kmac_init_from_dt(kDtKmac, &kmac) == kDifOk &&
         status_ok(kmac_testutils_config(&kmac, sideload));
}

dif_kmac_t *eg2_pbf_kmac_handle(void) { return &kmac; }

static bool kmac_finish(dif_kmac_operation_state_t *st, uint32_t *words, uint32_t nwords) {
  return dif_kmac_squeeze(&kmac, st, words, nwords, NULL, NULL) == kDifOk &&
         dif_kmac_end(&kmac, st) == kDifOk &&
         dif_kmac_poll_status(&kmac, KMAC_STATUS_SHA3_IDLE_BIT) == kDifOk;
}

bool eg2_pbf_sha3(uint32_t bits, const uint8_t *msg, uint32_t len, uint8_t *out) {
  dif_kmac_operation_state_t st;
  if (dif_kmac_mode_sha3_start(&kmac, &st,
                               bits == 256 ? kDifKmacModeSha3Len256 : kDifKmacModeSha3Len512) !=
      kDifOk) {
    return false;
  }
  if (len > 0 && dif_kmac_absorb(&kmac, &st, msg, len, NULL) != kDifOk) {
    return false;
  }
  uint32_t words[16];
  if (!kmac_finish(&st, words, bits / 32)) {
    return false;
  }
  for (uint32_t i = 0; i < bits / 8; ++i) {
    out[i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
  }
  return true;
}

bool eg2_pbf_kmac128(const dif_kmac_key_t *key, const uint8_t *msg, uint32_t len,
                     uint32_t *out, uint32_t nwords) {
  dif_kmac_operation_state_t st;
  dif_kmac_key_t zero;
  memset(&zero, 0, sizeof(zero));
  zero.length = kDifKmacKeyLen256;
  // With a sideloaded key the configured one is ignored (CFG.SIDELOAD).
  if (dif_kmac_mode_kmac_start(&kmac, &st, kDifKmacModeKmacLen128, nwords,
                               key != NULL ? key : &zero, NULL) != kDifOk) {
    return false;
  }
  if (len > 0 && dif_kmac_absorb(&kmac, &st, msg, len, NULL) != kDifOk) {
    return false;
  }
  return kmac_finish(&st, out, nwords);
}

// ---------------------------------------------------------------------------------------------
// HMAC with the sideloaded key, through the registers (hmac programmer's guide).

bool eg2_pbf_hmac_sideload(const uint8_t *msg, uint32_t len, uint32_t *out) {
  uint32_t cfg = (1u << HMAC_CFG_HMAC_EN_BIT) | (1u << HMAC_CFG_SIDELOAD_BIT);
  cfg = bitfield_field32_write(cfg, HMAC_CFG_DIGEST_SIZE_FIELD, HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256);
  cfg = bitfield_field32_write(cfg, HMAC_CFG_KEY_LENGTH_FIELD, HMAC_CFG_KEY_LENGTH_VALUE_KEY_512);
  eg2_dev_write32(kEg2DevHmac, HMAC_CFG_REG_OFFSET, cfg);
  eg2_dev_write32(kEg2DevHmac, HMAC_INTR_STATE_REG_OFFSET,
                  (1u << HMAC_INTR_STATE_HMAC_DONE_BIT) | (1u << HMAC_INTR_STATE_HMAC_ERR_BIT));
  eg2_dev_write32(kEg2DevHmac, HMAC_CFG_REG_OFFSET, cfg | (1u << HMAC_CFG_SHA_EN_BIT));
  eg2_dev_write32(kEg2DevHmac, HMAC_CMD_REG_OFFSET, 1u << HMAC_CMD_HASH_START_BIT);
  for (uint32_t i = 0; i < len; i += 4) {
    uint32_t w = 0;
    for (uint32_t b = 0; b < 4; ++b) {
      w |= (uint32_t)(i + b < len ? msg[i + b] : 0) << (8 * b);
    }
    if (len - i >= 4) {
      eg2_dev_write32(kEg2DevHmac, HMAC_MSG_FIFO_REG_OFFSET, w);
    } else {
      for (uint32_t b = 0; b < len - i; ++b) {
        *((volatile uint8_t *)eg2_dev(kEg2DevHmac) + HMAC_MSG_FIFO_REG_OFFSET) = msg[i + b];
      }
    }
  }
  eg2_dev_write32(kEg2DevHmac, HMAC_CMD_REG_OFFSET, 1u << HMAC_CMD_HASH_PROCESS_BIT);
  for (uint32_t p = 0; p < kPolls; ++p) {
    uint32_t intr = eg2_dev_read32(kEg2DevHmac, HMAC_INTR_STATE_REG_OFFSET);
    if (intr & (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) {
      eg2_pbf_note("hmac ERR_CODE", eg2_dev_read32(kEg2DevHmac, HMAC_ERR_CODE_REG_OFFSET));
      return false;
    }
    if (intr & (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)) {
      for (uint32_t i = 0; i < 8; ++i) {
        out[i] = eg2_dev_read32(kEg2DevHmac, HMAC_DIGEST_0_REG_OFFSET + 4 * i);
      }
      eg2_dev_write32(kEg2DevHmac, HMAC_INTR_STATE_REG_OFFSET, 1u << HMAC_INTR_STATE_HMAC_DONE_BIT);
      eg2_dev_write32(kEg2DevHmac, HMAC_CFG_REG_OFFSET, cfg);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Software AES-256 (FIPS 197, encryption only) and CTR_DRBG (SP 800-90A 10.2.1, no df).

static const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

void eg2_pbf_sw_aes256(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
  // Key expansion: 60 words (FIPS 197 5.2), Nk = 8.
  uint8_t w[240];
  memcpy(w, key, 32);
  uint8_t rcon = 1;
  for (uint32_t i = 8; i < 60; ++i) {
    uint8_t t[4];
    memcpy(t, &w[4 * (i - 1)], 4);
    if (i % 8 == 0) {
      uint8_t t0 = t[0];
      t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
      t[1] = kSbox[t[2]];
      t[2] = kSbox[t[3]];
      t[3] = kSbox[t0];
      rcon = xtime(rcon);
    } else if (i % 8 == 4) {
      for (uint32_t j = 0; j < 4; ++j) {
        t[j] = kSbox[t[j]];
      }
    }
    for (uint32_t j = 0; j < 4; ++j) {
      w[4 * i + j] = w[4 * (i - 8) + j] ^ t[j];
    }
  }
  uint8_t s[16];
  for (uint32_t j = 0; j < 16; ++j) {
    s[j] = in[j] ^ w[j];
  }
  for (uint32_t round = 1; round <= 14; ++round) {
    uint8_t u[16];
    // SubBytes and ShiftRows: state column-major, s[4c + r].
    for (uint32_t c = 0; c < 4; ++c) {
      for (uint32_t r = 0; r < 4; ++r) {
        u[4 * c + r] = kSbox[s[4 * ((c + r) % 4) + r]];
      }
    }
    if (round != 14) {
      for (uint32_t c = 0; c < 4; ++c) {
        uint8_t *a = &u[4 * c];
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        uint8_t all = a0 ^ a1 ^ a2 ^ a3;
        a[0] = (uint8_t)(a0 ^ all ^ xtime(a0 ^ a1));
        a[1] = (uint8_t)(a1 ^ all ^ xtime(a1 ^ a2));
        a[2] = (uint8_t)(a2 ^ all ^ xtime(a2 ^ a3));
        a[3] = (uint8_t)(a3 ^ all ^ xtime(a3 ^ a0));
      }
    }
    for (uint32_t j = 0; j < 16; ++j) {
      s[j] = u[j] ^ w[16 * round + j];
    }
  }
  memcpy(out, s, 16);
}

uint32_t eg2_pbf_nist_word(const uint8_t *b, uint32_t nbytes, uint32_t i) {
  uint32_t at = nbytes - 4 * (i + 1);
  return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) | ((uint32_t)b[at + 2] << 8) |
         b[at + 3];
}

void eg2_pbf_drbg_words_to_bytes(const uint32_t *w, uint32_t nwords, uint8_t out[48]) {
  memset(out, 0, 48);
  for (uint32_t i = 0; i < nwords && i < 12; ++i) {
    uint32_t at = 48 - 4 * (i + 1);
    out[at] = (uint8_t)(w[i] >> 24);
    out[at + 1] = (uint8_t)(w[i] >> 16);
    out[at + 2] = (uint8_t)(w[i] >> 8);
    out[at + 3] = (uint8_t)w[i];
  }
}

static void drbg_block(eg2_pbf_drbg_t *d, uint8_t out[16]) {
  for (int i = 15; i >= 0; --i) {
    if (++d->v[i] != 0) {
      break;
    }
  }
  eg2_pbf_sw_aes256(d->key, d->v, out);
}

void eg2_pbf_drbg_update(eg2_pbf_drbg_t *d, const uint8_t provided[48]) {
  uint8_t temp[48];
  for (uint32_t i = 0; i < 48; i += 16) {
    drbg_block(d, &temp[i]);
  }
  for (uint32_t i = 0; i < 48; ++i) {
    temp[i] ^= provided[i];
  }
  memcpy(d->key, temp, 32);
  memcpy(d->v, temp + 32, 16);
}

void eg2_pbf_drbg_instantiate(eg2_pbf_drbg_t *d, const uint8_t seed[48]) {
  memset(d, 0, sizeof(*d));
  eg2_pbf_drbg_update(d, seed);
}

void eg2_pbf_drbg_generate(eg2_pbf_drbg_t *d, const uint8_t *adata, uint32_t nblocks,
                           uint32_t *out) {
  static const uint8_t kZero[48] = {0};
  if (adata != NULL) {
    eg2_pbf_drbg_update(d, adata);
  }
  for (uint32_t b = 0; b < nblocks; ++b) {
    uint8_t blk[16];
    drbg_block(d, blk);
    for (uint32_t w = 0; w < 4; ++w) {
      out[4 * b + w] = eg2_pbf_nist_word(blk, 16, w);
    }
  }
  eg2_pbf_drbg_update(d, adata != NULL ? adata : kZero);
}

bool eg2_pbf_drbg_selftest(void) {
  const eg2_pbf_kat_drbg_t *k = &kEg2PbfKatDrbg;
  uint8_t seed[48], add[48];
  for (uint32_t i = 0; i < 48; ++i) {
    seed[i] = eg2_pbf_kat(k->ent)[i] ^ eg2_pbf_kat(k->pers)[i];
  }
  eg2_pbf_drbg_t d;
  eg2_pbf_drbg_instantiate(&d, seed);
  uint32_t words[16];
  memcpy(add, eg2_pbf_kat(k->add1), 48);
  eg2_pbf_drbg_generate(&d, add, 4, words);
  memcpy(add, eg2_pbf_kat(k->add2), 48);
  eg2_pbf_drbg_generate(&d, add, 4, words);
  uint8_t out[64];
  for (uint32_t b = 0; b < 4; ++b) {
    for (uint32_t w = 0; w < 4; ++w) {
      uint32_t x = words[4 * b + w];
      uint32_t at = 16 * b + 12 - 4 * w;
      out[at] = (uint8_t)(x >> 24);
      out[at + 1] = (uint8_t)(x >> 16);
      out[at + 2] = (uint8_t)(x >> 8);
      out[at + 3] = (uint8_t)x;
    }
  }
  // Not an eg2_pbf_check: the reference is checked against NIST before it is used, and the
  // inject-fail corruption belongs to the hardware's first check.
  return eg2_pbf_step("software CTR_DRBG reproduces NIST CTR_DRBG.rsp AES-256 no df COUNT 0",
                      memcmp(out, eg2_pbf_kat(k->out), 64) == 0);
}

// ---------------------------------------------------------------------------------------------
// Key manager.

uint32_t eg2_pbf_keymgr_last_codes(void) { return keymgr_codes; }

uint32_t eg2_pbf_keymgr_state(void) {
  return eg2_dev_read32(kEg2DevKeymgrDpe, KEYMGR_DPE_WORKING_STATE_REG_OFFSET) & 0x3;
}

bool eg2_pbf_keymgr_up(dif_keymgr_dpe_t *km, uint32_t *max_version) {
  if (!status_ok(keymgr_dpe_testutils_initialize(km, &kmac))) {
    eg2_pbf_note("keymgr_dpe bring-up failed, WORKING_STATE", eg2_pbf_keymgr_state());
    return false;
  }
  // Both paths end in slot 1 with kOwnerIntKeyParams / kOwnerKeyParams' maximum version.
  *max_version = kOwnerIntKeyParams.max_key_version;
  return true;
}

bool eg2_pbf_keymgr_generate(const dif_keymgr_dpe_t *km, dif_keymgr_dpe_key_dest_t dest,
                             bool sideload, const uint32_t salt[8], uint32_t version,
                             bool *op_ok, uint32_t share0[8], uint32_t share1[8]) {
  dif_keymgr_dpe_generate_params_t p = {
      .key_dest = dest,
      .sideload_key = sideload,
      .version = version,
      .slot_src_sel = 1,
  };
  memcpy(p.salt, salt, sizeof(p.salt));
  if (dif_keymgr_dpe_generate(km, &p) != kDifOk) {
    return false;
  }
  dif_keymgr_dpe_status_codes_t sc = 0;
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (dif_keymgr_dpe_get_status_codes(km, &sc) != kDifOk) {
      return false;
    }
    if (sc != 0) {
      break;
    }
  }
  keymgr_codes = sc;
  *op_ok = sc == kDifKeymgrDpeStatusCodeIdle;
  if (!*op_ok) {
    eg2_pbf_note("keymgr_dpe status codes", sc);
  }
  if (*op_ok && !sideload && share0 != NULL) {
    dif_keymgr_dpe_output_t out;
    if (dif_keymgr_dpe_read_output(km, &out) != kDifOk) {
      return false;
    }
    memcpy(share0, out.value[0], 32);
    memcpy(share1, out.value[1], 32);
  }
  return sc != 0;
}

// ---------------------------------------------------------------------------------------------
// Escalation.

void eg2_pbf_escalate_now(const char *alert, uint32_t ret_word) {
  int a = eg2_status_alert_find(alert);
  *eg2_pbf_ret(ret_word) = (uint32_t)a;
  if (a < 0) {
    eg2_pbf_step("alert to escalate exists", false);
    eg2_pbf_sw_reset();
  }
  uint32_t idx = (uint32_t)a;
  eg2_pbf_esc_t esc = {
      .cls = kEg2PbfClassA,
      .accum_thresh = 0,
      .phase_cycles = {2000, 2000, 200, 200},
      .signal_en = 0xe,
      .map = {1, 2, 3, 3},
      .crashdump_phase = 0,
      .lock = true,
  };
  eg2_pbf_escalate(&esc, &idx, 1);
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT);
  eg2_puts("PBF escalate: ");
  eg2_puts(alert);
  eg2_puts(" -> class A: life-cycle escalation, then the escalation reset\r\n");
  eg2_status_raise_alert(idx);
  while (true) {
  }
}

bool eg2_pbf_escalation_check(uint32_t ret_word) {
  uint32_t reason = eg2_pbf_reset_reason();
  bool ok = eg2_pbf_check("reset reason: the escalation reset", kDifRstmgrResetInfoEscalation,
                          reason);
  uint32_t alert = *eg2_pbf_ret(ret_word);
  uint32_t avail = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_ATTR_REG_OFFSET);
  ok = eg2_pbf_check("rstmgr ALERT_INFO_ATTR: crash dump words", kDumpWords, avail) && ok;
  uint32_t dump[kDumpWords];
  for (uint32_t i = 0; i < kDumpWords; ++i) {
    eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                    i << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
    dump[i] = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_REG_OFFSET);
  }
  uint32_t pos = kDumpCausePos + alert;
  ok = eg2_pbf_check("crash dump: class A in Phase0", kPhase0State, dump[0] & 0x7) && ok;
  ok = eg2_pbf_check("crash dump: the escalated alert's cause", 1,
                     alert < ALERT_HANDLER_PARAM_N_ALERTS ? (dump[pos / 32] >> (pos % 32)) & 1
                                                          : 0) &&
       ok;
  return ok;
}

bool eg2_pbf_entropy_up(void) {
  return eg2_pbf_step("entropy complex in auto mode",
                      status_ok(entropy_testutils_auto_mode_init()));
}
