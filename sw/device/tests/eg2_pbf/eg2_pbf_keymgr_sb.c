// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_keymgr_sb (EG2 verification spec REQ_PBF_15, "Post-boot features in CHERIoT
 * mode"): sealing and versioned keys from keymgr_dpe after the boot stages hand over, behind
 * secure boot (ROM_EXT left the key manager advanced; the test advances to the owner key) or the
 * test ROM (the test provisions the secrets and advances), in both modes.
 *
 * The key manager reaches an owner-stage key in slot 1 with a maximum key version (MAX_KEY_VER)
 * of 0xaa (keymgr_dpe_testutils). Then, from keymgr_dpe's theory of operation:
 *   - WORKING_STATE is Available;
 *   - a software output generated twice with the same inputs is the same key, and a sealing key
 *     (key_type 0: version 0) and a versioned key (key_type 1) are both generated from the slot;
 *   - one bit of the salt changed gives another key ("diversification");
 *   - versions: below and at the maximum generate; above it the operation is refused
 *     (OP_STATUS DONE_ERROR, ERR_CODE.INVALID_OP: "Versioned Key Generation") and the
 *     recoverable operation alert is raised (declared to the status module);
 *   - both shares of a generated key are not all zero.
 * Knobs: key_type (0 sealing, 1 versioned), version_class (0 below, 1 at, 2 above the maximum,
 * 3 drawn), salt_class (0 zero, 1 random, 2 all ones).
 * The keys' values against the key-manager reference over the KMAC model are the chip bench's
 * (gap: software cannot compute them, and eg2sim's keys are stand-ins).
 *
 * With EG2_PBF_INJECT_FAIL the first value check (WORKING_STATE) expects bit 0 flipped:
 * `PBF FAIL`.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

EG2_TEST_CONFIG();

#define KEYMGR_SB_PARAMS(X) \
  X(key_type, 1, 0, 1)      \
  X(version_class, 3, 0, 3) \
  X(salt_class, 1, 0, 2)
EG2_RM_PARAMS(KEYMGR_SB_PARAMS)

static bool nonzero(const uint32_t *k) {
  uint32_t a = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    a |= k[i];
  }
  return a != 0;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("keymgr", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  (void)eg2_pbf_boot_index();
  dif_keymgr_dpe_t km;
  uint32_t max = 0;
  ok = eg2_pbf_step("kmac init for keymgr", eg2_pbf_kmac_init(true)) && ok;
  ok = eg2_pbf_step("keymgr_dpe up to an owner-stage key in slot 1", eg2_pbf_keymgr_up(&km, &max)) &&
       ok;
  ok = eg2_pbf_check("keymgr_dpe WORKING_STATE Available", 1, eg2_pbf_keymgr_state()) && ok;

  uint32_t salt[8];
  uint32_t sc = EG2_PARAM(salt_class);
  for (uint32_t i = 0; i < 8; ++i) {
    salt[i] = sc == 0 ? 0 : sc == 1 ? eg2_rand() : 0xffffffff;
  }
  uint32_t vc = EG2_PARAM(version_class) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(version_class);
  uint32_t version = EG2_PARAM(key_type) == 0 ? 0
                     : vc == 0                 ? eg2_rand_range(0, max - 1)
                     : vc == 1                 ? max
                                               : max + eg2_rand_range(1, 16);
  eg2_pbf_note("key type (0 sealing, 1 versioned)", EG2_PARAM(key_type));
  eg2_pbf_note("version", version);
  eg2_pbf_note("maximum version", max);

  uint32_t a0[8], a1[8], b0[8], b1[8], c0[8], c1[8];
  bool op = false;
  if (version > max) {
    int alert = eg2_status_alert_find("keymgr_dpe_recov_operation_err");
    if (alert >= 0) {
      eg2_status_expect_alert((size_t)alert);
    }
    ok = eg2_pbf_step("keymgr generate above the maximum version completes",
                      eg2_pbf_keymgr_generate(&km, kDifKeymgrDpeKeyDestNone, false, salt, version,
                                              &op, a0, a1)) &&
         ok;
    ok = eg2_pbf_step("keymgr refuses a version above the maximum", !op) && ok;
    // OP_STATUS DONE_ERROR with ERR_CODE.INVALID_OP, as the DIF reads them (and clears them).
    // keymgr_dpe's theory of operation says only that the version comparison decides whether the
    // request is granted; it names no error code for a refusal. INVALID_OP is the one ERR_CODE
    // describes for an operation refused; the RTL sets INVALID_KMAC_INPUT as well (regr-vlt
    // 2026-10-25), which no document states or rules out (GAP-DOC-12): that bit is not checked.
    ok = eg2_pbf_check("keymgr status: idle with INVALID_OP",
                       kDifKeymgrDpeStatusCodeIdle | kDifKeymgrDpeStatusCodeInvalidOperation,
                       eg2_pbf_keymgr_last_codes() & ~(uint32_t)kDifKeymgrDpeStatusCodeInvalidKmacInput) &&
         ok;
    ok = eg2_pbf_step("keymgr_dpe_recov_operation_err reached the alert handler",
                      alert >= 0 && eg2_status_wait_alert((size_t)alert, 10000)) &&
         ok;
    version = max;
  }
  ok = eg2_pbf_step("keymgr generates the key",
                    eg2_pbf_keymgr_generate(&km, kDifKeymgrDpeKeyDestNone, false, salt, version,
                                            &op, a0, a1) &&
                        op) &&
       ok;
  ok = eg2_pbf_step("keymgr generates it again",
                    eg2_pbf_keymgr_generate(&km, kDifKeymgrDpeKeyDestNone, false, salt, version,
                                            &op, b0, b1) &&
                        op) &&
       ok;
  ok = eg2_pbf_step("same inputs, same key",
                    memcmp(a0, b0, 32) == 0 && memcmp(a1, b1, 32) == 0) &&
       ok;
  ok = eg2_pbf_step("the key's shares are not zero", nonzero(a0) || nonzero(a1)) && ok;
  salt[eg2_rand_range(0, 7)] ^= 1u << eg2_rand_range(0, 31);
  ok = eg2_pbf_step("keymgr generates with one salt bit changed",
                    eg2_pbf_keymgr_generate(&km, kDifKeymgrDpeKeyDestNone, false, salt, version,
                                            &op, c0, c1) &&
                        op) &&
       ok;
  uint32_t ka[8], kc[8];
  for (uint32_t i = 0; i < 8; ++i) {
    ka[i] = a0[i] ^ a1[i];
    kc[i] = c0[i] ^ c1[i];
  }
  ok = eg2_pbf_step("one salt bit changes the key", memcmp(ka, kc, 32) != 0) && ok;
  eg2_pbf_activity("keymgr generations", 3);
  eg2_pbf_gap("keys against the key-manager reference over the KMAC model",
              "software cannot compute them (OTP and RRAM secrets, netlist constants); the chip "
              "bench's reference is not built yet");
  return ok;
}
