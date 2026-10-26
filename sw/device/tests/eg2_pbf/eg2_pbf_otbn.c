// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_pbf/eg2_pbf_otbn.h"

#include "hw/top/otbn_regs.h"  // Generated.
#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/impl/ecc/p256.h"

// The P-256 group order n, least significant word first.
static const uint32_t kN[8] = {0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad,
                               0xffffffff, 0xffffffff, 0x00000000, 0xffffffff};

static p256_ecdsa_signature_t sig;

uint32_t eg2_pbf_otbn_status(void) {
  return eg2_dev_read32(kEg2DevOtbn, OTBN_STATUS_REG_OFFSET) & 0xff;
}
uint32_t eg2_pbf_otbn_err_bits(void) {
  return eg2_dev_read32(kEg2DevOtbn, OTBN_ERR_BITS_REG_OFFSET);
}
uint32_t eg2_pbf_otbn_fatal_cause(void) {
  return eg2_dev_read32(kEg2DevOtbn, OTBN_FATAL_ALERT_CAUSE_REG_OFFSET);
}
uint32_t eg2_pbf_otbn_dmem_word(uint32_t index) {
  return eg2_dev_read32(kEg2DevOtbn, OTBN_DMEM_REG_OFFSET + 4 * index);
}

bool eg2_pbf_otbn_wait_not(uint32_t busy, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    if (eg2_pbf_otbn_status() != busy) {
      return true;
    }
  }
  return false;
}

/** 32 big-endian bytes -> 8 words, least significant first (the driver's order). */
static void be_to_words(const uint8_t *be, uint32_t *w) {
  for (uint32_t i = 0; i < 8; ++i) {
    const uint8_t *b = be + 28 - 4 * i;
    w[i] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  }
}

/**
 * The digest as the driver takes it: the hash's 32 bytes in order (p256.c `set_message_digest`
 * reverses them into OTBN's little-endian `msg`, as otcrypto's callers pass the hash output).
 * Converted with be_to_words, like the integers, the driver reversed it into another e
 * (regr-vlt 2026-10-26: S wrong with R right, a valid SigVer vector refused). 2026-10-26.
 */
static void digest_words(const uint8_t *hash, uint32_t *w) { memcpy(w, hash, 32); }

static void words_to_be(const uint32_t *w, uint8_t *be) {
  for (uint32_t i = 0; i < 8; ++i) {
    uint8_t *b = be + 28 - 4 * i;
    b[0] = (uint8_t)(w[i] >> 24);
    b[1] = (uint8_t)(w[i] >> 16);
    b[2] = (uint8_t)(w[i] >> 8);
    b[3] = (uint8_t)w[i];
  }
}

/** x = (s0 + s1) mod n with s1 random below n: s0 = x - s1 (+ n on a borrow). */
static void share(const uint8_t *be, p256_masked_scalar_t *m) {
  uint32_t x[8], s1[8], s0[8];
  be_to_words(be, x);
  for (uint32_t i = 0; i < 8; ++i) {
    s1[i] = eg2_rand();
  }
  s1[7] &= 0x7fffffff;  // below n
  uint64_t borrow = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    uint64_t d = (uint64_t)x[i] - s1[i] - borrow;
    s0[i] = (uint32_t)d;
    borrow = (d >> 63) & 1;
  }
  if (borrow) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      uint64_t s = (uint64_t)s0[i] + kN[i] + carry;
      s0[i] = (uint32_t)s;
      carry = s >> 32;
    }
  }
  memset(m, 0, sizeof(*m));
  memcpy(m->share0, s0, sizeof(s0));
  memcpy(m->share1, s1, sizeof(s1));
  m->checksum = p256_masked_scalar_checksum(m);
}

bool eg2_pbf_otbn_verify_start(const eg2_pbf_kat_ecdsa_t *v, bool corrupt) {
  uint32_t digest[8];
  p256_point_t pub;
  digest_words(eg2_pbf_kat(v->digest), digest);
  be_to_words(eg2_pbf_kat(v->qx), pub.x);
  be_to_words(eg2_pbf_kat(v->qy), pub.y);
  be_to_words(eg2_pbf_kat(v->r), sig.r);
  be_to_words(eg2_pbf_kat(v->s), sig.s);
  if (corrupt) {
    sig.s[0] ^= 1;
  }
  return status_ok(p256_ecdsa_verify_start(&sig, digest, &pub));
}

bool eg2_pbf_otbn_verify_end(bool *valid) {
  hardened_bool_t r = kHardenedBoolFalse;
  if (!status_ok(p256_ecdsa_verify_finalize(&sig, &r))) {
    return false;
  }
  *valid = r == kHardenedBoolTrue;
  return true;
}

bool eg2_pbf_otbn_sign(const eg2_pbf_kat_ecdsa_t *v, uint8_t rs[64]) {
  uint32_t digest[8];
  p256_masked_scalar_t d, k;
  digest_words(eg2_pbf_kat(v->digest), digest);
  share(eg2_pbf_kat(v->d), &d);
  share(eg2_pbf_kat(v->k), &k);
  if (!status_ok(p256_ecdsa_sign_config_k_start(digest, &d, &k))) {
    return false;
  }
  p256_ecdsa_signature_t out;
  if (!status_ok(p256_ecdsa_sign_finalize(&out))) {
    return false;
  }
  words_to_be(out.r, rs);
  words_to_be(out.s, rs + 32);
  return true;
}
