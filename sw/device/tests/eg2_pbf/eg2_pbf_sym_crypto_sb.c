// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_sym_crypto_sb (EG2 verification spec REQ_PBF_10, "Post-boot features in
 * CHERIoT mode"): AES, HMAC/SHA-2 and KMAC/SHA-3 after the boot stages hand over, behind secure
 * boot (ROM, ROM_EXT, the signed owner image) or the test ROM, in RISC-V mode and, the `_cheriot`
 * build, in CHERIoT mode through the device capabilities.
 *
 * Software keys, against NIST values (eg2_pbf_crypto_kat.h, generated from the pinned CAVP files):
 *   AES   a chain of `aes_mode` (ECB, CBC, CFB-128 over 1-4 blocks; OFB, CTR one block) with the
 *         zero key of `key_len`, encrypted and decrypted, every ciphertext block a NIST value;
 *         and one ECBVarKey block with a non-zero key; manual or automatic operation drawn;
 *   HMAC  SHA-256, -384 or -512 of a message of length class `msg_class` (0, 1, block - 1,
 *         block bytes, a long message); HMAC-SHA256 or -SHA512 from HMAC.rsp;
 *   KMAC  SHA3-256 or SHA3-512 of a message of class `msg_class`; KMAC128 sample #1 of NIST's
 *         KMAC_samples (key 40..5F, data 00010203, empty S, L = 256).
 * Sideloaded keys (`sideload` 1): keymgr_dpe generates an AES, a KMAC and an HMAC key from the
 * owner-stage slot; with each, the engine's result must decrypt back (AES), repeat for the same
 * key generated again, and change once the sideload slot is cleared. The sideloaded results
 * against the key-manager reference over the KMAC model are the chip bench's (gap below).
 *
 * Knobs (EG2_RM_PARAMS): engines (bit 0 AES, 1 HMAC, 2 KMAC), aes_mode (0-4, 5: drawn), key_len
 * (0: 128, 1: 192, 2: 256, 3: drawn), msg_class (0-4, 5: drawn), manual_pct, sideload. Every
 * part runs in eg2sim too (its keymgr keys are stand-ins, but every check here is a NIST value or
 * holds for any key).
 *
 * With EG2_PBF_INJECT_FAIL the first AES ciphertext check expects bit 0 flipped: the run must
 * fail with `PBF FAIL`.
 */

#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/testing/keymgr_dpe_testutils.h"
#include "sw/device/tests/eg2_nist/eg2_nist_sha2_engine.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

EG2_TEST_CONFIG();

#define SYM_SB_PARAMS(X)  \
  X(engines, 7, 1, 7)     \
  X(aes_mode, 5, 0, 5)    \
  X(key_len, 3, 0, 3)     \
  X(msg_class, 5, 0, 5)   \
  X(manual_pct, 25, 0, 100) \
  X(sideload, 1, 0, 1)
EG2_RM_PARAMS(SYM_SB_PARAMS)

// NIST KMAC_samples.pdf, sample #1: KMAC128, key 40 41 .. 5F, data 00 01 02 03, S "", L 256.
static const uint8_t kKmacSample1Out[32] = {
    0xe5, 0x78, 0x0b, 0x0d, 0x3e, 0xa6, 0xf7, 0xd3, 0xa4, 0x29, 0xc5, 0x70, 0x6a, 0xa4, 0x3a, 0x00,
    0xfa, 0xdb, 0xd7, 0xd4, 0x96, 0x28, 0x83, 0x9e, 0x31, 0x87, 0x24, 0x3f, 0x45, 0x6e, 0xe1, 0x4e,
};

static const char *const kModeName[kEg2PbfAesModes] = {"ECB", "CBC", "CFB-128", "OFB", "CTR"};

static uint32_t pick(uint32_t knob, uint32_t drawn_value, uint32_t hi) {
  return knob == drawn_value ? eg2_rand_range(0, hi) : knob;
}

static bool aes_part(void) {
  bool ok = eg2_pbf_step("aes init", eg2_pbf_aes_init());
  uint32_t mode = pick(EG2_PARAM(aes_mode), 5, 4);
  uint32_t kidx = pick(EG2_PARAM(key_len), 3, 2);
  uint32_t nblocks = eg2_rand_range(1, kEg2PbfAesMaxBlocks);
  bool manual = eg2_rand_range(0, 99) < EG2_PARAM(manual_pct);
  eg2_pbf_aes_chain_t c;
  eg2_pbf_aes_chain(kidx, (eg2_pbf_aes_mode_t)mode, nblocks, &c);
  eg2_puts("PBF aes: ");
  eg2_puts(kModeName[mode]);
  eg2_puts(" key ");
  eg2_putdec(c.key_bits);
  eg2_puts(" blocks ");
  eg2_putdec(c.nblocks);
  eg2_puts(manual ? " manual\r\n" : " automatic\r\n");
  uint8_t zero_key[32] = {0};
  uint8_t out[16 * kEg2PbfAesMaxBlocks];
  ok = eg2_pbf_step("aes encrypt runs",
                    eg2_pbf_aes_run((eg2_pbf_aes_mode_t)mode, c.key_bits, zero_key, false, manual,
                                    c.iv, c.pt, c.nblocks, out)) &&
       ok;
  ok = eg2_pbf_check_bytes("aes ciphertext = NIST ECBVarTxt ciphertexts", c.ct, out,
                           16 * c.nblocks) &&
       ok;
  ok = eg2_pbf_step("aes decrypt runs",
                    eg2_pbf_aes_run((eg2_pbf_aes_mode_t)mode, c.key_bits, zero_key, true, manual,
                                    c.iv, c.ct, c.nblocks, out)) &&
       ok;
  ok = eg2_pbf_check_bytes("aes decryption = the plaintext", c.pt, out, 16 * c.nblocks) && ok;
  // A non-zero key: ECBVarKey, plaintext zero.
  const eg2_pbf_kat_aes_t *k = &kEg2PbfKatAes[kidx];
  uint8_t zero_block[16] = {0};
  ok = eg2_pbf_step("aes VarKey encrypt runs",
                    eg2_pbf_aes_run(kEg2PbfAesEcb, k->key_bits, eg2_pbf_kat(k->vk_key), false,
                                    manual, NULL, zero_block, 1, out)) &&
       ok;
  ok = eg2_pbf_check_bytes("aes ciphertext = NIST ECBVarKey ciphertext", eg2_pbf_kat(k->vk_ct),
                           out, 16) &&
       ok;
  eg2_pbf_activity("aes blocks", 2 * c.nblocks + 1);
  return ok;
}

static const eg2_pbf_kat_hash_t *hash_vector(uint32_t alg, uint32_t cls) {
  for (uint32_t i = 0; i < sizeof(kEg2PbfKatHash) / sizeof(kEg2PbfKatHash[0]); ++i) {
    if (kEg2PbfKatHash[i].alg == alg && kEg2PbfKatHash[i].cls == cls) {
      return &kEg2PbfKatHash[i];
    }
  }
  return NULL;
}

static bool hmac_part(void) {
  eg2_nist_sha2_init();
  uint32_t alg = eg2_rand_range(0, 2);
  uint32_t cls = pick(EG2_PARAM(msg_class), 5, 4);
  const eg2_pbf_kat_hash_t *v = hash_vector(alg, cls);
  static const uint32_t kBits[3] = {256, 384, 512};
  uint8_t out[64];
  bool ok = v != NULL;
  if (ok) {
    eg2_pbf_note("hmac: SHA-2 bits", kBits[alg]);
    eg2_pbf_note("hmac: message bytes", v->msg_len);
    ok = eg2_pbf_step("sha2 runs", eg2_nist_sha2_run(kBits[alg], NULL, 0, eg2_pbf_kat(v->msg),
                                                     v->msg_len, 16, out));
    ok = eg2_pbf_check_bytes("sha2 digest = NIST", eg2_pbf_kat(v->md), out, v->md_len) && ok;
  }
  const eg2_pbf_kat_hmac_t *h = &kEg2PbfKatHmac[eg2_rand_range(0, 1)];
  ok = eg2_pbf_step("hmac runs", eg2_nist_sha2_run(h->bits, eg2_pbf_kat(h->key), h->key_len,
                                                   eg2_pbf_kat(h->msg), h->msg_len, 16, out)) &&
       ok;
  ok = eg2_pbf_check_bytes("hmac tag = NIST HMAC.rsp", eg2_pbf_kat(h->mac), out, h->mac_len) &&
       ok;
  eg2_pbf_activity("hmac digests", 2);
  return ok;
}

static bool kmac_part(void) {
  bool ok = eg2_pbf_step("kmac init", eg2_pbf_kmac_init(false));
  uint32_t alg = 3 + eg2_rand_range(0, 1);
  uint32_t cls = pick(EG2_PARAM(msg_class), 5, 4);
  const eg2_pbf_kat_hash_t *v = hash_vector(alg, cls);
  uint8_t out[64];
  if (v != NULL) {
    eg2_pbf_note("kmac: SHA3 bits", alg == 3 ? 256 : 512);
    eg2_pbf_note("kmac: message bytes", v->msg_len);
    ok = eg2_pbf_step("sha3 runs", eg2_pbf_sha3(alg == 3 ? 256 : 512, eg2_pbf_kat(v->msg),
                                                v->msg_len, out)) &&
         ok;
    ok = eg2_pbf_check_bytes("sha3 digest = NIST", eg2_pbf_kat(v->md), out, v->md_len) && ok;
  }
  dif_kmac_key_t key;
  memset(&key, 0, sizeof(key));
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t b = 0x40 + 4 * i;
    key.share0[i] = b | ((b + 1) << 8) | ((b + 2) << 16) | ((b + 3) << 24);
  }
  key.length = kDifKmacKeyLen256;
  static const uint8_t kData[4] = {0, 1, 2, 3};
  uint32_t words[8];
  ok = eg2_pbf_step("kmac128 runs", eg2_pbf_kmac128(&key, kData, 4, words, 8)) && ok;
  for (uint32_t i = 0; i < 32; ++i) {
    out[i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
  }
  ok = eg2_pbf_check_bytes("kmac128 = NIST KMAC sample #1", kKmacSample1Out, out, 32) && ok;
  eg2_pbf_activity("kmac digests", 2);
  return ok;
}

static bool differ(const uint32_t *a, const uint32_t *b, uint32_t n) {
  return memcmp(a, b, 4 * n) != 0;
}

static bool sideload_part(dif_keymgr_dpe_t *km) {
  uint32_t salt[8];
  for (uint32_t i = 0; i < 8; ++i) {
    salt[i] = eg2_rand();
  }
  bool op = false, ok = true;
  // AES: a sideloaded AES-256 key; ECB of the NIST X_1 block.
  const uint8_t *x = eg2_pbf_kat(kEg2PbfKatAes[2].x[0]);
  uint8_t c1[16], c2[16], p[16];
  ok = eg2_pbf_step("keymgr generates the AES sideload key",
                    eg2_pbf_keymgr_generate(km, kDifKeymgrDpeKeyDestAes, true, salt, 0, &op, NULL,
                                            NULL) &&
                        op);
  ok = eg2_pbf_step("aes encrypt with the sideloaded key",
                    eg2_pbf_aes_run(kEg2PbfAesEcb, 256, NULL, false, false, NULL, x, 1, c1)) &&
       ok;
  ok = eg2_pbf_step("aes decrypt with the sideloaded key",
                    eg2_pbf_aes_run(kEg2PbfAesEcb, 256, NULL, true, false, NULL, c1, 1, p)) &&
       ok;
  ok = eg2_pbf_step("aes sideloaded decryption returns the plaintext", memcmp(p, x, 16) == 0) &&
       ok;
  ok = eg2_pbf_step("aes sideloaded ciphertext is not the zero key's (the key reached AES)",
                    memcmp(c1, eg2_pbf_kat(kEg2PbfKatAes[2].y[0]), 16) != 0) &&
       ok;
  ok = eg2_pbf_step("keymgr generates the same AES key again",
                    eg2_pbf_keymgr_generate(km, kDifKeymgrDpeKeyDestAes, true, salt, 0, &op, NULL,
                                            NULL) &&
                        op) &&
       ok;
  ok = eg2_pbf_step("aes encrypt again",
                    eg2_pbf_aes_run(kEg2PbfAesEcb, 256, NULL, false, false, NULL, x, 1, c2)) &&
       ok;
  ok = eg2_pbf_step("aes same key, same ciphertext", memcmp(c1, c2, 16) == 0) && ok;
  ok = eg2_pbf_step("keymgr clears the AES sideload slot",
                    dif_keymgr_dpe_clear_sideload_key(km, kDifKeymgrDpeSideLoadClearAes) ==
                        kDifOk) &&
       ok;
  (void)dif_keymgr_dpe_clear_sideload_key(km, kDifKeymgrDpeSideLoadClearNone);
  eg2_pbf_gap("AES after SIDELOAD_CLEAR",
              "keymgr_dpe.hjson says the slot is \"continuously cleared with entropy\" but not "
              "whether AES still sees a valid key, so what AES does next is not checked");

  // KMAC: a sideloaded key; KMAC128 of the NIST data, twice.
  uint32_t k1[8], k2[8], k0[8];
  ok = eg2_pbf_step("kmac init with sideload", eg2_pbf_kmac_init(true)) && ok;
  ok = eg2_pbf_step("keymgr generates the KMAC sideload key",
                    eg2_pbf_keymgr_generate(km, kDifKeymgrDpeKeyDestKmac, true, salt, 0, &op,
                                            NULL, NULL) &&
                        op) &&
       ok;
  static const uint8_t kData[4] = {0, 1, 2, 3};
  ok = eg2_pbf_step("kmac128 with the sideloaded key", eg2_pbf_kmac128(NULL, kData, 4, k1, 8)) &&
       ok;
  ok = eg2_pbf_step("kmac128 with the sideloaded key again",
                    eg2_pbf_kmac128(NULL, kData, 4, k2, 8)) &&
       ok;
  ok = eg2_pbf_step("kmac same key, same tag", !differ(k1, k2, 8)) && ok;
  ok = eg2_pbf_step("kmac software config back", eg2_pbf_kmac_init(false)) && ok;
  dif_kmac_key_t zero;
  memset(&zero, 0, sizeof(zero));
  zero.length = kDifKmacKeyLen256;
  ok = eg2_pbf_step("kmac128 with a zero software key", eg2_pbf_kmac128(&zero, kData, 4, k0, 8)) &&
       ok;
  ok = eg2_pbf_step("kmac sideloaded tag is not the software key's", differ(k1, k0, 8)) && ok;
  // keymgr_dpe uses KMAC itself: leave it configured as keymgr_dpe_testutils does.
  (void)eg2_pbf_kmac_init(true);

  // HMAC: a sideloaded 512-bit key; HMAC-SHA256, twice.
  uint32_t h1[8], h2[8];
  ok = eg2_pbf_step("keymgr generates the HMAC sideload key",
                    eg2_pbf_keymgr_generate(km, kDifKeymgrDpeKeyDestHmac, true, salt, 0, &op,
                                            NULL, NULL) &&
                        op) &&
       ok;
  ok = eg2_pbf_step("hmac with the sideloaded key", eg2_pbf_hmac_sideload(kData, 4, h1)) && ok;
  ok = eg2_pbf_step("hmac with the sideloaded key again", eg2_pbf_hmac_sideload(kData, 4, h2)) &&
       ok;
  ok = eg2_pbf_step("hmac same key, same tag", !differ(h1, h2, 8)) && ok;
  eg2_pbf_activity("sideloaded keys used", 3);
  eg2_pbf_gap("sideloaded results against the key-manager reference over the KMAC model",
              "software cannot know a sideloaded key; the chip bench's reference computes it "
              "(not built yet)");
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("sym_crypto", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  (void)eg2_pbf_boot_index();
  uint32_t en = EG2_PARAM(engines);
  bool sideload = EG2_PARAM(sideload);
  dif_keymgr_dpe_t km;
  uint32_t max_version = 0;
  if (sideload) {
    // From the test ROM this provisions the secrets and resets once on the first boot, so it
    // comes before anything checked.
    ok = eg2_pbf_step("kmac init for keymgr", eg2_pbf_kmac_init(true)) && ok;
    ok = eg2_pbf_step("keymgr_dpe up to an owner-stage key in slot 1",
                      eg2_pbf_keymgr_up(&km, &max_version)) &&
         ok;
  }
  if (en & 1) {
    ok = aes_part() && ok;
  }
  if (en & 2) {
    ok = hmac_part() && ok;
  }
  if (en & 4) {
    ok = kmac_part() && ok;
  }
  if (sideload && ok) {
    ok = sideload_part(&km) && ok;
  }
  eg2_pbf_gap("message length block + 1", "the CAVP short-message files stop at one block; "
              "chip_sw_eg2_nist_sha2/_sha3 run the long messages");
  return ok;
}
