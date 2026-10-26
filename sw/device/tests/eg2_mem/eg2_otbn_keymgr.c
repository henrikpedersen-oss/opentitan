// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_otbn_keymgr (EG2 verification spec REQ_PBF_11; chip_otbn_testplan chip_sw_otbn_keymgr),
 * in both modes: keymgr_dpe sideloads a key to OTBN, and an OTBN program (eg2_otbn_key_read.s)
 * reads it through the KEY_S0_L/H and KEY_S1_L/H WSRs into DMEM. The checks rest on relations
 * the documents state, not on the key's value (which only the key manager's reference model in
 * the bench knows):
 *   - the run ends with ERR_BITS 0 and the key read is not all zero (the testpoint);
 *   - with the sideload slot cleared (SIDELOAD_CLEAR OTBN), reading the key fails with
 *     ERR_BITS.KEY_INVALID and the `otbn_recov` alert (OTBN isa.md: a KEY_* WSR read without a
 *     valid key is a KEY_INVALID software error; theory_of_operation "Recoverable errors");
 *   - the same generation inputs give the same key again, a different salt a different key
 *     (keymgr_dpe theory_of_operation: generation is a KMAC of its inputs).
 *
 * The key manager is brought up by keymgr_dpe_testutils_startup: from the test ROM it writes the
 * creator and owner seeds, locks SECRET2 and resets the chip by software (the test runs twice;
 * the reset reason SW is checked by the library and cleared before the status sweep, which
 * otherwise fails any reset reason but power-on), then loads the UDS and advances to the
 * CreatorRootKey.
 *
 * Knobs: version (0 to the creator root key's maximum, 0x11), salt_flip (which salt word the
 * "different salt" step changes, 0-7), model_gaps.
 *
 * | Skipped with model_gaps = 1           | Why                                                 |
 * |---------------------------------------|-----------------------------------------------------|
 * | the OTBN runs and their key checks    | eg2sim stops on EXECUTE (open decision 6); the      |
 * |                                       |   model run checks the key manager's sideload       |
 * |                                       |   generations complete without error               |
 *
 * Built with EG2_OTBN_KEYMGR_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `MEM FAIL`.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/testing/keymgr_dpe_testutils.h"
#include "sw/device/tests/eg2_mem/eg2_otbn.h"
#include "sw/device/tests/eg2_mem/eg2_otbn_key_read_image.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define OTBN_KEYMGR_PARAMS(X) \
  X(version, 0, 0, 0x11)      \
  X(salt_flip, 0, 0, 7)       \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(OTBN_KEYMGR_PARAMS)


enum {
  kPolls = 2000000,
  kKeyWords = 32,  // four 256-bit WDRs
};

static dif_keymgr_dpe_t keymgr;
static dif_kmac_t kmac;

/** Runs the key-read program; returns ERR_BITS and the 32 words it read. */
static uint32_t read_key(uint32_t key[kKeyWords]) {
  eg2_otbn_app_t app = EG2_OTBN_APP(KeyRead);
  uint32_t at = kEg2OtbnKeyRead_key_out;
  eg2_otbn_load(&app, at + 4 * kKeyWords);
  uint32_t err = eg2_otbn_run(kPolls);
  eg2_otbn_dmem_read(at, key, kKeyWords);
  return err;
}

static bool generate(dif_keymgr_dpe_generate_params_t *p) {
  return eg2_chk_true("keymgr_dpe sideload generation",
                      status_ok(keymgr_dpe_testutils_generate_key(&keymgr, p)));
}

/**
 * Whether two reads hold the same key. The four WDRs are share 0 (KEY_S0_L, KEY_S0_H: words 0-15)
 * and share 1 (KEY_S1_L, KEY_S1_H: words 16-31) of the 512-bit sideload key (OTBN README, "Wide
 * Special Purpose Registers"). The key is the shares' XOR, and no document says the shares repeat:
 * on the RTL two generations from the same inputs gave different shares (regr-vlt 2026-10-25), so a
 * word-by-word comparison of the shares is not a check of the key.
 */
static bool same(const uint32_t *a, const uint32_t *b) {
  for (uint32_t i = 0; i < kKeyWords / 2; ++i) {
    if ((a[i] ^ a[i + kKeyWords / 2]) != (b[i] ^ b[i + kKeyWords / 2])) {
      return false;
    }
  }
  return true;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  bool ok = eg2_chk_true("keymgr_dpe startup",
                         status_ok(keymgr_dpe_testutils_startup(&keymgr, &kmac)));
  // After the UDS and the CreatorRootKey the key manager is Available (keymgr_dpe
  // theory_of_operation "Working State").
  ok = eg2_chk_eq("WORKING_STATE after startup (AVAILABLE)",
                  KEYMGR_DPE_WORKING_STATE_STATE_VALUE_AVAILABLE,
                  eg2_dev_read32(kEg2DevKeymgrDpe, KEYMGR_DPE_WORKING_STATE_REG_OFFSET) & 0x3) &&
       ok;
  // The library's software reset (the first boot) is part of this test: its reason is not an
  // unexpected reset.
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET,
                  eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET));

  dif_keymgr_dpe_generate_params_t p = kKeyVersionedParams;
  p.key_dest = kDifKeymgrDpeKeyDestOtbn;
  p.sideload_key = true;
  p.version = EG2_PARAM(version);
  for (uint32_t i = 0; i < 8; ++i) {
    p.salt[i] ^= eg2_rand();
  }
  ok = generate(&p) && ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_chk_skip("the OTBN runs (eg2sim stops on EXECUTE)");
    p.salt[EG2_PARAM(salt_flip)] = ~p.salt[EG2_PARAM(salt_flip)];
    ok = generate(&p) && ok;
    return eg2_chk_summary(2) && ok;
  }

  uint32_t k0[kKeyWords], k1[kKeyWords];
  ok = eg2_chk_eq("ERR_BITS with the sideloaded key", 0, read_key(k0)) && ok;
  uint32_t nonzero = 0;
  // The key (the shares' XOR), not a share: random shares of a zero key are not zero.
  for (uint32_t i = 0; i < kKeyWords / 2; ++i) {
    nonzero += (k0[i] ^ k0[i + kKeyWords / 2]) != 0;
  }
  ok = eg2_chk_true("the key OTBN read is not zero", nonzero > 0) && ok;

  // Cleared slot: KEY_INVALID and the recoverable alert.
  int recov = eg2_status_alert_find("otbn_recov");
  if (recov >= 0) {
    eg2_status_expect_alert((size_t)recov);
  }
  ok = eg2_chk_true("sideload clear",
                    status_ok(keymgr_dpe_testutils_clear_sideload_key(
                        &keymgr, kDifKeymgrDpeSideLoadClearOtbn))) &&
       ok;
  ok = eg2_chk_eq("ERR_BITS with the slot cleared (KEY_INVALID)",
                  1u << OTBN_ERR_BITS_KEY_INVALID_BIT, read_key(k1)) &&
       ok;
  ok = eg2_chk_true("otbn_recov alert",
                    recov >= 0 && eg2_status_wait_alert((size_t)recov, 10000)) &&
       ok;
  eg2_status_clear_expected();
  eg2_otbn_reg_wr(OTBN_ERR_BITS_REG_OFFSET, 0);
  ok = eg2_chk_true("sideload clear released",
                    status_ok(keymgr_dpe_testutils_clear_sideload_key(
                        &keymgr, kDifKeymgrDpeSideLoadClearNone))) &&
       ok;

  // The same inputs: the same key.
  ok = generate(&p) && ok;
  ok = eg2_chk_eq("ERR_BITS after regeneration", 0, read_key(k1)) && ok;
  ok = eg2_chk_true("same inputs, same key", same(k0, k1)) && ok;
  // A different salt: a different key.
  p.salt[EG2_PARAM(salt_flip)] = ~p.salt[EG2_PARAM(salt_flip)];
  ok = generate(&p) && ok;
  ok = eg2_chk_eq("ERR_BITS after a new salt", 0, read_key(k1)) && ok;
  ok = eg2_chk_true("different salt, different key", !same(k0, k1)) && ok;
  return eg2_chk_summary(8) && ok;
}
