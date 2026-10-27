// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * NIST CAVP CTR_DRBG vectors on csrng's software instance (chip_sw_eg2_nist_drbg, REQ_PBF_12,
 * REQ_PBF_23): AES-256 without derivation function, as csrng implements it, without reseed, with
 * reseed and with prediction resistance, in both execution modes.
 *
 * Instantiate and reseed take the seed from software only (flag0 set: csrng's known-answer mode):
 * the entropy input XOR the personalization string, or XOR the additional input. A generate takes
 * the additional input; with prediction resistance a reseed with fresh entropy and the additional
 * input precedes each generate, which then has none. The second generate's 512 bits must equal
 * ReturnedBits; the scoreboard checks every instruction. With EG2_NIST_INJECT_FAIL the first
 * expected value is corrupted and the run must fail.
 *
 * Word order (csrng_testutils' KAT, the csrng documentation): seed words least significant first
 * of the 384-bit NIST string; output words least significant first within each 128-bit block, the
 * blocks in order.
 */

#include "hw/top/dt/api.h"    // Generated.
#include "hw/top/dt/csrng.h"  // Generated.
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/dif/dif_csrng_shared.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_DRBG_PARAMS(X) X(boundary_vectors, 8, 0, 1024) X(random_vectors, 3, 0, 64)
EG2_TEST_PARAMS(NIST_DRBG_PARAMS);

enum {
  kSeedBytes = 48,
  kSeedWords = 12,
  kMaxOutWords = 16,
  kPolls = 100000,
};

static dif_csrng_t csrng;

static bool ready(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    dif_csrng_cmd_status_t s;
    if (dif_csrng_get_cmd_interface_status(&csrng, &s) != kDifOk ||
        s.kind == kDifCsrngCmdStatusError) {
      return false;
    }
    if (s.kind == kDifCsrngCmdStatusReady) {
      return true;
    }
  }
  return false;
}

// a XOR b (either may be shorter than 48 bytes or empty: zero-padded), as csrng seed words.
static void seed_xor(const eg2_nist_vec_t *v, int fa, int fb, dif_csrng_seed_material_t *out) {
  uint8_t s[kSeedBytes];
  for (uint32_t i = 0; i < kSeedBytes; ++i) {
    uint8_t a = i < eg2_nist_len(v, fa) ? eg2_nist_data(v, fa)[i] : 0;
    uint8_t b = fb >= 0 && i < eg2_nist_len(v, fb) ? eg2_nist_data(v, fb)[i] : 0;
    s[i] = a ^ b;
  }
  for (uint32_t i = 0; i < kSeedWords; ++i) {
    out->seed_material[i] = eg2_nist_word(s, kSeedBytes, i);
  }
  out->seed_material_len = kSeedWords;
}

static bool cmd(csrng_app_cmd_id_t id, const dif_csrng_seed_material_t *seed) {
  return ready() && csrng_send_app_cmd(csrng.base_addr, kCsrngAppCmdTypeCsrng,
                                       (csrng_app_cmd_t){
                                           .id = id,
                                           .entropy_src_enable = kDifCsrngEntropySrcToggleDisable,
                                           .seed_material = seed,
                                       }) == kDifOk;
}

static bool generate(const eg2_nist_vec_t *v, int add_field, uint32_t *out, uint32_t nwords) {
  dif_csrng_seed_material_t add;
  bool has_add = add_field >= 0 && eg2_nist_len(v, add_field) > 0;
  if (has_add) {
    seed_xor(v, add_field, -1, &add);
  }
  return ready() && dif_csrng_generate_start(&csrng, has_add ? &add : NULL, nwords) == kDifOk &&
         dif_csrng_generate_read(&csrng, out, nwords) == kDifOk;
}

static eg2_nist_result_t run_vector(const eg2_nist_vec_t *v, bool corrupt) {
  uint32_t nbytes = eg2_nist_len(v, kEg2NistReturned), nwords = nbytes / 4;
  if (nwords == 0 || nwords > kMaxOutWords || nbytes % 16 != 0) {
    return eg2_nist_result(eg2_nist_error(v, "unsupported ReturnedBitsLen"));
  }
  dif_csrng_seed_material_t seed;
  if (!ready() || dif_csrng_uninstantiate(&csrng) != kDifOk) {
    return eg2_nist_result(eg2_nist_error(v, "uninstantiate"));
  }
  seed_xor(v, kEg2NistEnt, kEg2NistPers, &seed);
  if (!cmd(kCsrngAppCmdInstantiate, &seed)) {
    return eg2_nist_result(eg2_nist_error(v, "instantiate"));
  }
  if (v->flags & kEg2NistReseed) {
    seed_xor(v, kEg2NistEntReseed, kEg2NistAddReseed, &seed);
    if (!cmd(kCsrngAppCmdReseed, &seed)) {
      return eg2_nist_result(eg2_nist_error(v, "reseed"));
    }
  }
  uint32_t words[kMaxOutWords];
  for (int g = 0; g < 2; ++g) {
    int add = g == 0 ? kEg2NistAdd1 : kEg2NistAdd2;
    if (v->flags & kEg2NistPr) {
      seed_xor(v, g == 0 ? kEg2NistEntPr1 : kEg2NistEntPr2, add, &seed);
      if (!cmd(kCsrngAppCmdReseed, &seed)) {
        return eg2_nist_result(eg2_nist_error(v, "prediction-resistance reseed"));
      }
      add = -1;
    }
    if (!generate(v, add, words, nwords)) {
      return eg2_nist_result(eg2_nist_error(v, "generate"));
    }
  }
  // Back to NIST byte order: block b's words are least significant first.
  uint8_t out[4 * kMaxOutWords];
  for (uint32_t b = 0; b < nbytes / 16; ++b) {
    for (uint32_t w = 0; w < 4; ++w) {
      uint32_t x = words[4 * b + w];
      uint32_t at = 16 * b + 12 - 4 * w;
      out[at] = (uint8_t)(x >> 24);
      out[at + 1] = (uint8_t)(x >> 16);
      out[at + 2] = (uint8_t)(x >> 8);
      out[at + 3] = (uint8_t)x;
    }
  }
  return eg2_nist_result(eg2_nist_expect(v, kEg2NistReturned, out, nbytes, corrupt));
}

bool eg2_test_main(void) {
  if (dif_csrng_init_from_dt((dt_csrng_t)0, &csrng) != kDifOk ||
      dif_csrng_configure(&csrng) != kDifOk) {
    eg2_puts("CSRNG init failed\r\n");
    return false;
  }
  bool ok = eg2_nist_run("drbg", run_vector, EG2_PARAM(boundary_vectors), EG2_PARAM(random_vectors), EG2_NIST_INJECT);
  // Leave the software instance uninstantiated: it held known-answer state.
  return ready() && dif_csrng_uninstantiate(&csrng) == kDifOk && ok;
}
