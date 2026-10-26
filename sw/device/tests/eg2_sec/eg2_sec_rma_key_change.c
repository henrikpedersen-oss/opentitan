// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_rma_key_change (EG2 verification spec REQ_SEC_08, "Security in RISC-V mode"):
 * keys derived before the RMA transition cannot be derived after it. Both modes.
 *
 * Oracle: device life cycle "RMA"; keymgr_dpe's theory of operation: the advance from stage 0
 * consumes the health state measurement, one constant per life-cycle group (DEV, PROD/PROD_END,
 * RMA differ), and the advance from stage 1 the creator seed, which the RMA wipe overwrites
 * (rram_ctrl "RMA Handling": info pages 5-7 and the data partition). So the same chain
 * (`eg2_kmgr_chain`, the same binding and salt) gives a different stage-1 and stage-2 key in RMA.
 *
 * Phases (`phase`, written by the bench, chip_sw_eg2_sec_rma_key_change_vseq):
 *   0  in DEV or PROD (the bench provisions SECRET2 and the RMA token): derive K1 and K2 with
 *      binding and salt from the seed, leave them in `kEg2KeysOut` with a marker, and wait. The
 *      bench reads them, moves the chip to RMA over the LC TAP with the RMA token (the wipe erases
 *      the RRAM, so the bench loads this image again), writes the keys into `kEg2PrevKeys` and
 *      `phase` 1, and resets.
 *   1  in RMA: the same chain; K1 and K2 must differ from `kEg2PrevKeys`. The bench compares too.
 *
 * Built with EG2_SEC_RMA_KEY_CHANGE_INJECT_FAIL K1 is expected unchanged: the run must fail with
 * `EG2CHK FAIL K1 differs across RMA`.
 */

#include "sw/device/lib/base/macros.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define RMA_KEY_PARAMS(X) \
  X(phase, 0, 0, 1)       \
  X(input_seed, 1, 1, 0xffffffff) \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(RMA_KEY_PARAMS)

enum {
  kOutReady = 0x4b455953,  // "SYEK"
};

/** The keys of phase 0 (K1 then K2), written by the bench for phase 1. */
OT_USED OT_SECTION(".rodata") volatile const uint32_t kEg2PrevKeys[16] = {0};

/** This boot's keys (K1, K2) and a marker, for the bench. */
volatile uint32_t kEg2KeysOut[17];

/** The binding and salt, the same in both phases: from `input_seed`, not the run's seed. */
static void inputs(uint32_t binding[8], uint32_t salt[8]) {
  uint32_t x = EG2_PARAM(input_seed);
  for (uint32_t i = 0; i < 16; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (i < 8) {
      binding[i] = x;
    } else {
      salt[i - 8] = x;
    }
  }
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  uint32_t state = eg2_lc_state_decoded();
  eg2_boot_note("life-cycle state", state);
  uint32_t binding[8], salt[8], k1[8], k2[8];
  inputs(binding, salt);
  ok = eg2_kmgr_chain(0, binding, salt, k1, k2) && ok;
  eg2_boot_note_key("K1", k1);
  eg2_boot_note_key("K2", k2);
  for (uint32_t i = 0; i < 8; ++i) {
    kEg2KeysOut[i] = k1[i];
    kEg2KeysOut[8 + i] = k2[i];
  }

  if (EG2_PARAM(phase) == 0 && EG2_PARAM(model_gaps)) {
    // The SystemC model has no LC TAP and no RMA transition (model_gaps stays 0 on the chip): it
    // checks the derivation, and that the same inputs give the same keys again.
    uint32_t again1[8], again2[8];
    ok = eg2_kmgr_chain(1, binding, salt, again1, again2) && ok;
    bool same = eg2_boot_key_eq(k1, again1) && eg2_boot_key_eq(k2, again2);
#ifdef EG2_SEC_RMA_KEY_CHANGE_INJECT_FAIL
    same = !same;
#endif
    ok = eg2_boot_expect("model: same inputs give the same keys", same, again1[0]) && ok;
    return eg2_boot_summary() && ok;
  }
  if (EG2_PARAM(phase) == 0) {
    ok = eg2_boot_expect("phase 0 runs in DEV or PROD", state == kEg2LcDev || state == kEg2LcProd,
                         state) &&
         ok;
    kEg2KeysOut[16] = ok ? kOutReady : 0;
    eg2_puts("EG2 RMA KEYS READY\r\n");
    // The bench takes it from here: the transition, a new image, a reset.
    while (true) {
      __asm__ volatile("wfi");
    }
  }

  ok = eg2_boot_check("phase 1 runs in RMA", kEg2LcRma, state) && ok;
  uint32_t prev1[8], prev2[8];
  for (uint32_t i = 0; i < 8; ++i) {
    prev1[i] = kEg2PrevKeys[i];
    prev2[i] = kEg2PrevKeys[8 + i];
  }
  bool k1_differs = !eg2_boot_key_eq(k1, prev1);
#ifdef EG2_SEC_RMA_KEY_CHANGE_INJECT_FAIL
  k1_differs = !k1_differs;
#endif
  ok = eg2_boot_expect("K1 differs across RMA", k1_differs, k1[0]) && ok;
  ok = eg2_boot_expect("K2 differs across RMA", !eg2_boot_key_eq(k2, prev2), k2[0]) && ok;
  kEg2KeysOut[16] = kOutReady;
  return eg2_boot_summary() && ok;
}
