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

#include "hw/top/aes_regs.h"
#include "hw/top/dt/aes.h"
#include "hw/top/dt/rv_core_ibex.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/cheriot_mmio.h"
#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/base/hardened_memory.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/crypto/drivers/aes.h"
#include "sw/device/lib/crypto/impl/aes_kwp/aes_kwp.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/tests/eg2_nist/eg2_nist.h"

EG2_TEST_CONFIG();

#define NIST_AES_KW_PARAMS(X) \
  X(boundary_vectors, 8, 0, 1024) \
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

// The two waits the crypto library's AES driver never bounds, polled here a bounded number of times
// through the same abs_mmio path: rv_core_ibex's RND_STATUS (drivers/rv_core_ibex.c
// wait_rnd_valid, before every masked key write and register clear) and the AES block's STATUS
// (drivers/aes.c spin_until). 2026-10-27: the CHERIoT build printed "NIST vector 46" and then
// nothing for the hour, the RISC-V build passes in 14 minutes; this says which wait never ends.
enum { kWaitPolls = 10000 };

static void put_kv(const char *key, uint32_t value) {
  eg2_puts(key);
  eg2_puts(" 0x");
  eg2_puthex(value);
}

static void report_waits(void) {
  uint32_t ibex = dt_rv_core_ibex_primary_reg_block(kDtRvCoreIbex);
  uint32_t aes = dt_aes_primary_reg_block(kDtAes);
  uint32_t polls = 0, valid = 0;
  for (; polls < kWaitPolls; ++polls) {
    uint32_t sts = abs_mmio_read32(ibex + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
    if (bitfield_bit32_read(sts, RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) {
      valid = 1;
      break;
    }
  }
  put_kv("NIST aes_kw wait: RND_DATA_VALID", valid);
  put_kv(" polls", polls);
  if (valid) {
    // A second word: the driver reads several in a row.
    (void)abs_mmio_read32(ibex + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
    for (polls = 0; polls < kWaitPolls; ++polls) {
      if (bitfield_bit32_read(abs_mmio_read32(ibex + RV_CORE_IBEX_RND_STATUS_REG_OFFSET),
                              RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) {
        break;
      }
    }
    put_kv(" next word polls", polls);
  }
  put_kv(" AES STATUS", abs_mmio_read32(aes + AES_STATUS_REG_OFFSET));
  eg2_puts("\r\n");
}

// aes_begin's steps (drivers/aes.c) one at a time, each wait bounded (2026-10-27: in CHERIoT mode
// aes_decrypt_begin never returned, the first driver call of report_driver): the CTRL write, the
// idle wait, the key shares through hardened_memshred / hardened_memcpy and the AES window's
// capability (OT_MMIO_ADDR_TO_PTR, as the driver's AES_REG_PTR_), the idle wait after the key.
static uint32_t poll_idle(uint32_t aes) {
  uint32_t polls = 0;
  for (; polls < kWaitPolls; ++polls) {
    if (bitfield_bit32_read(abs_mmio_read32(aes + AES_STATUS_REG_OFFSET), AES_STATUS_IDLE_BIT)) {
      break;
    }
  }
  return polls;
}

static void report_begin_steps(void) {
  static const uint32_t kKey[8] = {0};
  uint32_t aes = dt_aes_primary_reg_block(kDtAes);
  uint32_t ctrl = abs_mmio_read32(aes + AES_CTRL_SHADOWED_REG_OFFSET);
  put_kv("NIST aes_kw begin: CTRL", ctrl);
  eg2_puts("\r\n");
  abs_mmio_write32_shadowed(aes + AES_CTRL_SHADOWED_REG_OFFSET, ctrl);
  put_kv("NIST aes_kw begin: CTRL written, idle polls", poll_idle(aes));
  put_kv(" STATUS", abs_mmio_read32(aes + AES_STATUS_REG_OFFSET));
  eg2_puts("\r\n");
  uint32_t *share0 = (uint32_t *)OT_MMIO_ADDR_TO_PTR(aes + AES_KEY_SHARE0_0_REG_OFFSET);
  uint32_t *share1 = (uint32_t *)OT_MMIO_ADDR_TO_PTR(aes + AES_KEY_SHARE1_0_REG_OFFSET);
  put_kv("NIST aes_kw begin: memshred share0", status_ok(hardened_memshred(share0, 8)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw begin: memcpy share0", status_ok(hardened_memcpy(share0, kKey, 6)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw begin: memshred share1", status_ok(hardened_memshred(share1, 8)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw begin: memcpy share1", status_ok(hardened_memcpy(share1, kZeroShare, 6)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw begin: key written, idle polls", poll_idle(aes));
  put_kv(" STATUS", abs_mmio_read32(aes + AES_STATUS_REG_OFFSET));
  eg2_puts("\r\n");
}

// The driver calls of one unwrap step, one at a time with a line after each (2026-10-27: in
// CHERIoT mode both waits above are fine, and the run still stops in the first vector, KWP_AD_192;
// the RISC-V build passes it). A 192-bit all-zero key, like that vector's length.
static void report_driver(void) {
  static const uint32_t kKey[8] = {0};
  aes_key_t kek = {
      .mode = kAesCipherModeEcb,
      .sideload = kHardenedBoolFalse,
      .key_len = 6,
      .key_shares = {kKey, kZeroShare},
  };
  kek.checksum = aes_key_integrity_checksum(&kek);
  aes_block_t block = {.data = {1, 2, 3, 4}};
  put_kv("NIST aes_kw driver: decrypt_begin", status_ok(aes_decrypt_begin(kek, NULL)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw driver: update in", status_ok(aes_update(NULL, &block)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw driver: update out", status_ok(aes_update(&block, NULL)));
  eg2_puts("\r\n");
  put_kv("NIST aes_kw driver: end", status_ok(aes_end(NULL)));
  put_kv(" AES STATUS", abs_mmio_read32(dt_aes_primary_reg_block(kDtAes) + AES_STATUS_REG_OFFSET));
  eg2_puts("\r\n");
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
  if (!EG2_PARAM(model_gaps)) {
    report_waits();
    report_begin_steps();
    report_driver();
  }
  return eg2_nist_run("aes_kw", run_vector, EG2_PARAM(boundary_vectors), EG2_PARAM(random_vectors), EG2_NIST_INJECT);
}
