// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_keymgr_dpe_diversification (EG2 verification spec REQ_SEC_09, "Security in RISC-V
 * mode"): every derived key depends on its stage's inputs; one input changed per run changes the
 * key, and the same inputs give the same key. Both modes.
 *
 * Oracle: keymgr_dpe's theory of operation, "Advance" (NumBootStages = 3): the advance from
 * stage 0 consumes SW_CDI_INPUT (SW_BINDING), the device identifier, the health state measurement
 * (one constant per life-cycle group) , the ROM digest and the hardware revision seed; the
 * advance from stage 1 consumes SW_CDI_INPUT and the creator seed. The versioned key
 * (`KDF(parent_key, KEY_VERSION || SALT || ...)`) of a slot changes when its key changes.
 *
 * Each derivation is the chain of `eg2_kmgr_chain`: stage-1 key K1, stage-2 key K2, the same salt.
 *
 * `input` (the one input changed in this run):
 *   0 sw_binding   in one boot: chain A with binding B, chain B with B again (K1, K2 equal),
 *                  chain C with B and `bits` flipped (K1 and K2 differ);
 *   1 device_id    across a reset: the bench changes the device identifier (HW_CFG0, backdoor)
 *                  while the software resets; K1 and K2 differ after it;
 *   2 lc_state     the same, the bench moves the life-cycle state to another group (RMA -> DEV);
 *                  K1 and K2 differ;
 *   3 creator_seed the software writes a new creator seed (RRAM info page 5, `CreatorInfoPage`,
 *                  through the controller: lc_ctrl's CREATOR_SEED_SW_RW_EN allows it in RMA) and
 *                  resets; K1 is the same, K2 differs;
 *   4 none         a reset with nothing changed: K1 and K2 equal (same inputs, same key).
 * `bits`: how many bits of the binding or seed change (1, or 2-32 random).
 * The keys of the first boot are kept in the retention SRAM's owner area across the reset.
 *
 * Not changed by this test (reported, see the spec's table): the ROM digest (the ROM check must
 * pass, so the bench cannot load another ROM in one run), the hardware revision seed (a netlist
 * constant), and the owner seed (consumed by an advance from stage 2, which the keymgr_dpe
 * documentation refuses: `boot_stage + 1 < NumBootStages`; ticket F16).
 *
 * Built with EG2_KEYMGR_DPE_DIVERSIFICATION_INJECT_FAIL the changed input is expected to leave
 * K1 unchanged: the run must fail with `EG2CHK FAIL K1 changed with the input`.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "hw/top/rram_ctrl_regs.h"   // Generated.
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define KEYMGR_DIV_PARAMS(X) \
  X(input, 0, 0, 4)          \
  X(bits, 1, 1, 32)
EG2_RM_PARAMS(KEYMGR_DIV_PARAMS)

enum {
  kInputBinding = 0,
  kInputDeviceId = 1,
  kInputLcState = 2,
  kInputCreatorSeed = 3,
  kInputNone = 4,
  kCreatorPage = 5,
  // State in the retention SRAM: data[0..7] K1, data[8..15] K2, data[16..23] binding,
  // data[24..31] salt, of the first boot.
  kSaved = 0,
};

/** Flips `n` distinct bits of an 8-word value. */
static void flip_bits(uint32_t v[8], uint32_t n) {
  uint32_t done = 0;
  uint32_t used[8] = {0};
  while (done < n) {
    uint32_t b = eg2_rand_range(0, 255);
    if (used[b / 32] & (1u << (b % 32))) {
      continue;
    }
    used[b / 32] |= 1u << (b % 32);
    v[b / 32] ^= 1u << (b % 32);
    ++done;
  }
}

static bool expect_change(const char *what, bool changed, const uint32_t a[8],
                          const volatile uint32_t b[8]) {
  bool eq = eg2_boot_key_eq(a, b);
  return eg2_boot_expect(what, changed ? !eq : eq, a[0]);
}

static bool binding_in_one_boot(const uint32_t binding[8], const uint32_t salt[8]) {
  uint32_t a1[8], a2[8], b1[8], b2[8], c1[8], c2[8], other[8];
  bool ok = eg2_kmgr_chain(0, binding, salt, a1, a2);
  ok = eg2_kmgr_chain(1, binding, salt, b1, b2) && ok;
  for (uint32_t i = 0; i < 8; ++i) {
    other[i] = binding[i];
  }
  flip_bits(other, EG2_PARAM(bits));
  ok = eg2_kmgr_chain(2, other, salt, c1, c2) && ok;
  eg2_boot_note_key("K1 binding", a1);
  eg2_boot_note_key("K1 same binding", b1);
  eg2_boot_note_key("K1 changed binding", c1);
  ok = expect_change("same inputs, same K1", false, a1, b1) && ok;
  ok = expect_change("same inputs, same K2", false, a2, b2) && ok;
#ifdef EG2_KEYMGR_DPE_DIVERSIFICATION_INJECT_FAIL
  ok = expect_change("K1 changed with the input", false, a1, c1) && ok;
#else
  ok = expect_change("K1 changed with the input", true, a1, c1) && ok;
#endif
  ok = expect_change("K2 changed with the input", true, a2, c2) && ok;
  return ok;
}

/** Writes a new creator seed into info page 5 through the controller. */
static bool write_creator_seed(void) {
  uint32_t seed[8];
  for (uint32_t i = 0; i < 8; ++i) {
    seed[i] = eg2_rand();
  }
  // Scrambled and ECC-protected, as the boot code provisions seeds (nvm_testutils
  // kPageScrambleCfg).
  eg2_rram_info_cfg(kCreatorPage, true, true, true, true);
  eg2_rram_res_t r = eg2_rram_op(kEg2RramOpWrite, kEg2RramPartInfo,
                                 kCreatorPage * RRAM_CTRL_PARAM_BYTES_PER_PAGE, 8, seed);
  bool ok = eg2_boot_check("creator seed written (RMA: CREATOR_SEED_SW_RW_EN)", 0, r.err_code);
  uint32_t back[8] = {0};
  r = eg2_rram_op(kEg2RramOpRead, kEg2RramPartInfo, kCreatorPage * RRAM_CTRL_PARAM_BYTES_PER_PAGE,
                  8, back);
  ok = eg2_boot_check("creator seed read back", seed[0], back[0]) && ok;
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  uint32_t input = EG2_PARAM(input);
  volatile eg2_boot_state_t *st = eg2_boot_state();
  eg2_boot_note("life-cycle state", eg2_lc_state_decoded());

  if (input == kInputBinding) {
    uint32_t binding[8], salt[8];
    for (uint32_t i = 0; i < 8; ++i) {
      binding[i] = eg2_rand();
      salt[i] = eg2_rand();
    }
    ok = binding_in_one_boot(binding, salt) && ok;
    return eg2_boot_summary() && ok;
  }

  if (st->step == 0) {
    // First boot: derive, keep the keys, change the input (or let the bench change it), reset.
    uint32_t binding[8], salt[8], k1[8], k2[8];
    for (uint32_t i = 0; i < 8; ++i) {
      binding[i] = eg2_rand();
      salt[i] = eg2_rand();
    }
    ok = eg2_kmgr_chain(0, binding, salt, k1, k2) && ok;
    eg2_boot_note_key("K1 before", k1);
    eg2_boot_note_key("K2 before", k2);
    for (uint32_t i = 0; i < 8; ++i) {
      st->data[kSaved + i] = k1[i];
      st->data[kSaved + 8 + i] = k2[i];
      st->data[kSaved + 16 + i] = binding[i];
      st->data[kSaved + 24 + i] = salt[i];
    }
    if (input == kInputCreatorSeed) {
      ok = write_creator_seed() && ok;
    }
    st->fails = ok ? 0 : 1;
    st->step = 1;
    // The bench (chip_sw_eg2_keymgr_dpe_diversification_vseq) changes the device identifier or
    // the life-cycle state while this reset is in progress.
    eg2_puts("EG2 DIVERSIFICATION RESET\r\n");
    eg2_boot_sw_reset();
  }

  // Second boot: the same derivation, with the input changed (or not).
  ok = eg2_boot_check("first boot's checks", 0, st->fails) && ok;
  uint32_t binding[8], salt[8], k1[8], k2[8];
  for (uint32_t i = 0; i < 8; ++i) {
    binding[i] = st->data[kSaved + 16 + i];
    salt[i] = st->data[kSaved + 24 + i];
  }
  ok = eg2_kmgr_chain(0, binding, salt, k1, k2) && ok;
  eg2_boot_note_key("K1 after", k1);
  eg2_boot_note_key("K2 after", k2);
  bool k1_changes = input == kInputDeviceId || input == kInputLcState;
  bool k2_changes = input != kInputNone;
#ifdef EG2_KEYMGR_DPE_DIVERSIFICATION_INJECT_FAIL
  k1_changes = !k1_changes;
#endif
  ok = expect_change(k1_changes ? "K1 changed with the input" : "K1 unchanged", k1_changes, k1,
                     &st->data[kSaved]) &&
       ok;
  ok = expect_change(k2_changes ? "K2 changed with the input" : "K2 unchanged", k2_changes, k2,
                     &st->data[kSaved + 8]) &&
       ok;
  st->magic = 0;  // the next power-on starts again
  return eg2_boot_summary() && ok;
}
