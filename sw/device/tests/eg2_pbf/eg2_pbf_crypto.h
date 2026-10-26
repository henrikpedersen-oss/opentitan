// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_CRYPTO_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_CRYPTO_H_

/**
 * @file
 * @brief What the crypto group of the post-boot feature tests shares (REQ_PBF_10, 11, 12, 15).
 *
 *  - The known answers (eg2_pbf_crypto_kat.h, generated from the NIST CAVP files) and the AES
 *    chains composed from them: ECB, CBC and CFB-128 over 1-4 blocks and single-block OFB and
 *    CTR, every ciphertext block a NIST ciphertext of key 0 (`eg2_pbf_aes_chain`).
 *  - The engines: AES through its DIF (software or sideloaded key), SHA-3 and KMAC through the
 *    KMAC DIF, SHA-2 and HMAC through eg2_nist's register driver, HMAC with a sideloaded key
 *    through the registers.
 *  - A software AES-256 (encryption only) and CTR_DRBG (SP 800-90A 10.2.1, no derivation
 *    function, as csrng implements it): the reference the entropy tests predict csrng with. It
 *    counts as an oracle only after it reproduces a NIST CTR_DRBG vector (`eg2_pbf_drbg_selftest`),
 *    which each test runs first (REQ_PBF_04).
 *  - The key manager brought up to an owner-stage key in slot 1 (keymgr_dpe_testutils, behind
 *    ROM_EXT or the test ROM) and its generate operation.
 *  - An escalation that resets the chip: class A, the given alert, phase 0 and 1 the life-cycle
 *    escalation (signals 1 and 2: every crypto block's lc_escalate_en, the core's fetch off),
 *    phase 2 the escalation reset (signal 3), no NMI (the CHERIoT start-up code ends the test on
 *    any trap); the crash dump latched on entering phase 0. The next boot checks the reason and
 *    the crash dump (`eg2_pbf_escalation_check`).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto_kat.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of the KAT pool at offset `off`. */
static inline const uint8_t *eg2_pbf_kat(uint32_t off) { return &kEg2PbfKatPool[off]; }

/** AES modes of the chains (index into the DIF's mode bits). */
typedef enum eg2_pbf_aes_mode {
  kEg2PbfAesEcb = 0,
  kEg2PbfAesCbc = 1,
  kEg2PbfAesCfb = 2,
  kEg2PbfAesOfb = 3,
  kEg2PbfAesCtr = 4,
  kEg2PbfAesModes = 5,
} eg2_pbf_aes_mode_t;

enum {
  kEg2PbfAesMaxBlocks = 4,
};

/** A chain: key 0 of `key_bits`, IV, plaintext and the NIST ciphertext, `nblocks` blocks. */
typedef struct eg2_pbf_aes_chain {
  uint32_t mode, key_bits, nblocks;
  uint8_t iv[16];
  uint8_t pt[16 * kEg2PbfAesMaxBlocks];
  uint8_t ct[16 * kEg2PbfAesMaxBlocks];
} eg2_pbf_aes_chain_t;

/**
 * Composes a chain from kEg2PbfKatAes (key index 0/1/2 for 128/192/256). OFB and CTR take one
 * block; the others 1-4. `r` is random filler for the free plaintext block.
 */
void eg2_pbf_aes_chain(uint32_t key_idx, eg2_pbf_aes_mode_t mode, uint32_t nblocks,
                       eg2_pbf_aes_chain_t *c);

/** Initialises the AES handle and resets the unit. */
bool eg2_pbf_aes_init(void);

/**
 * One AES operation over `nblocks` blocks through the DIF. `key` NULL takes the sideloaded key
 * (AES-256). `manual`: manual operation (a START trigger per block). Writes nblocks * 16 bytes.
 * Returns false after printing why on an engine error or timeout.
 */
bool eg2_pbf_aes_run(eg2_pbf_aes_mode_t mode, uint32_t key_bits, const uint8_t *key,
                     bool decrypt, bool manual, const uint8_t *iv, const uint8_t *in,
                     uint32_t nblocks, uint8_t *out);

/** The same, stopping after `stop_after` blocks have gone in (the fault tests); no dif_aes_end. */
bool eg2_pbf_aes_partial(uint32_t key_bits, const uint8_t *key, const uint8_t *in,
                         uint32_t stop_after, uint8_t *out);

/** The AES unit's STATUS register. */
uint32_t eg2_pbf_aes_status(void);

/** KMAC with software entropy (and the sideloaded key with `sideload`). */
bool eg2_pbf_kmac_init(bool sideload);
dif_kmac_t *eg2_pbf_kmac_handle(void);

/** SHA3-`bits` of `msg` on the KMAC engine. */
bool eg2_pbf_sha3(uint32_t bits, const uint8_t *msg, uint32_t len, uint8_t *out);

/**
 * KMAC128 of `msg` with `key` (NULL: the sideloaded key), empty customisation string, `nwords`
 * output words (the state window's little-endian order: output byte 0 in bits 7:0 of word 0).
 */
bool eg2_pbf_kmac128(const dif_kmac_key_t *key, const uint8_t *msg, uint32_t len,
                     uint32_t *out, uint32_t nwords);

/** HMAC-SHA256 of `msg` with the sideloaded key (CFG.KEY_SWAP off), 8 words of digest. */
bool eg2_pbf_hmac_sideload(const uint8_t *msg, uint32_t len, uint32_t *out);

/** Software AES-256 encryption of one block (FIPS 197). */
void eg2_pbf_sw_aes256(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);

/** The software CTR_DRBG (AES-256, no df), its state in NIST byte order. */
typedef struct eg2_pbf_drbg {
  uint8_t key[32];
  uint8_t v[16];
} eg2_pbf_drbg_t;

/** csrng seed or additional data: `nwords` words, least significant first (zero-extended). */
void eg2_pbf_drbg_words_to_bytes(const uint32_t *w, uint32_t nwords, uint8_t out[48]);
void eg2_pbf_drbg_instantiate(eg2_pbf_drbg_t *d, const uint8_t seed[48]);
void eg2_pbf_drbg_update(eg2_pbf_drbg_t *d, const uint8_t provided[48]);
/** Generate `nblocks` blocks; `adata` NULL = none. Output as csrng gives it: 4 words per block,
 * least significant first. */
void eg2_pbf_drbg_generate(eg2_pbf_drbg_t *d, const uint8_t *adata, uint32_t nblocks,
                           uint32_t *out);

/** Checks the software CTR_DRBG against the NIST vector (instantiate, generate twice). */
bool eg2_pbf_drbg_selftest(void);

/** NIST byte string of 48 bytes -> csrng words (least significant first). */
uint32_t eg2_pbf_nist_word(const uint8_t *b, uint32_t nbytes, uint32_t i);

/**
 * Brings the key manager to an owner-stage key in slot 1 (keymgr_dpe_testutils_initialize: from
 * the test ROM it writes the creator and owner secrets and resets once on the first boot).
 * Returns false after printing why. `max_version` is the slot's maximum key version.
 */
bool eg2_pbf_keymgr_up(dif_keymgr_dpe_t *km, uint32_t *max_version);

/** Generate from slot 1; sideload to `dest` or SW output (`dest` none). OP_STATUS result in *ok. */
bool eg2_pbf_keymgr_generate(const dif_keymgr_dpe_t *km, dif_keymgr_dpe_key_dest_t dest,
                             bool sideload, const uint32_t salt[8], uint32_t version,
                             bool *op_ok, uint32_t share0[8], uint32_t share1[8]);

/**
 * The last generate's status codes (dif_keymgr_dpe_get_status_codes: OP_STATUS and ERR_CODE,
 * which reading through the DIF clears): kDifKeymgrDpeStatusCodeIdle alone on success.
 */
uint32_t eg2_pbf_keymgr_last_codes(void);

/** The key manager's WORKING_STATE (0 Reset, 1 Available, 2 Disabled, 3 Invalid). */
uint32_t eg2_pbf_keymgr_state(void);

/**
 * Escalates `alert` (its name in the top) through class A as described above, then raises it.
 * Records its index in retention word `ret_word`. Never returns.
 */
void eg2_pbf_escalate_now(const char *alert, uint32_t ret_word) __attribute__((noreturn));

/**
 * On the boot after eg2_pbf_escalate_now: the reset reason is the escalation's, and the alert
 * crash dump names the alert recorded in retention word `ret_word` in class A. Returns the
 * checks' verdict.
 */
bool eg2_pbf_escalation_check(uint32_t ret_word);

/** The entropy complex in auto mode (entropy_testutils), as the crypto library needs it. */
bool eg2_pbf_entropy_up(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_CRYPTO_H_
