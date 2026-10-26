// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_boot_keymgr_stage and _cheriot (EG2 verification spec REQ_BOOT_13, "Secure boot in
 * RISC-V mode", "Twins of the RISC-V tests"): before the owner image runs, ROM_EXT has advanced
 * the key manager past the Silicon Creator stage; the owner image cannot recreate a Silicon
 * Creator key, and the key manager's Silicon Creator inputs are locked.
 *
 * Behind the full secure-boot chain (sim_dv_rom_ext). Oracle: secure boot "ROM_EXT" steps 5-6;
 * identities and root keys (CreatorRootKey from the ROM, OwnerIntermediateKey and OwnerRootKey
 * from ROM_EXT); keymgr_dpe's theory of operation and programmer's guide:
 *  - WORKING_STATE is Available (the key manager is usable by the owner);
 *  - every valid slot is at boot stage 2 (Owner) or later: no UDS (stage 0) and no Creator-stage
 *    key (stage 1) is left for the owner (METADATA_HIGH, read in the order `order` picks);
 *  - Load Root Key is locked (LOAD_ROOT_KEY_LOCK set, "unless locked with LOAD_ROOT_KEY_LOCK") and
 *    refused (DONE_ERROR, ERR_CODE.INVALID_OP): the UDS cannot be latched again;
 *  - ENFORCE_SW_BINDING is set ("Normally, this is done by ROM_EXT after deriving OwnerRootKey"):
 *    an advance that would consume the hardware binding values (SW_BINDING_ONLY clear) from a
 *    stage-2 slot is refused;
 *  - a key generation (SW and each sideload destination) from an empty slot is refused.
 * The key values against the key-manager reference model are the bench's (not done: the
 * reference derivation of chip_sw_keymgr_dpe_key_derivation_vseq needs ROM_EXT's bindings).
 *
 * Built with EG2_BOOT_KEYMGR_STAGE_INJECT_FAIL Load Root Key is expected to succeed: the run must
 * fail with `EG2CHK FAIL Load Root Key refused`.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define KEYMGR_STAGE_PARAMS(X) X(order, 0, 0, 1)
EG2_RM_PARAMS(KEYMGR_STAGE_PARAMS)

enum {
  kInvalidOp = 1u << 0,
  kOwnerStage = 2,
};

static uint32_t stage_of(uint32_t hi) {
  return (hi >> KEYMGR_DPE_METADATA_HIGH_0_BOOT_STAGE_0_OFFSET) &
         KEYMGR_DPE_METADATA_HIGH_0_BOOT_STAGE_0_MASK;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA) && ok;
  int alert = eg2_status_alert_find("keymgr_dpe_recov_operation_err");
  if (alert >= 0) {
    eg2_status_expect_alert((size_t)alert);
  }
  ok = eg2_boot_check("working state Available", kEg2KmgrAvailable, eg2_kmgr_state()) && ok;

  // Slot metadata, in either order.
  uint32_t empty = kEg2KmgrSlots, owner_slot = kEg2KmgrSlots, valid = 0;
  for (uint32_t n = 0; n < kEg2KmgrSlots; ++n) {
    uint32_t i = EG2_PARAM(order) == 0 ? n : kEg2KmgrSlots - 1 - n;
    uint32_t hi = eg2_kmgr_meta_hi(i);
    eg2_boot_note("slot", i);
    eg2_boot_note("  METADATA_HIGH", hi);
    eg2_boot_note("  METADATA_LOW", eg2_kmgr_meta_lo(i));
    if (hi & 1) {
      ++valid;
      ok = eg2_boot_expect("valid slot is past the Creator stages (stage >= 2)",
                           stage_of(hi) >= kOwnerStage, hi) &&
           ok;
      if (owner_slot == kEg2KmgrSlots) {
        owner_slot = i;
      }
    } else if (empty == kEg2KmgrSlots) {
      empty = i;
    }
  }
  ok = eg2_boot_expect("ROM_EXT left at least one owner slot", valid > 0, valid) && ok;

  ok = eg2_boot_check("LOAD_ROOT_KEY_LOCK set", 1,
                      eg2_kmgr_reg(KEYMGR_DPE_LOAD_ROOT_KEY_LOCK_REG_OFFSET) & 1) &&
       ok;
  ok = eg2_boot_expect("ENFORCE_SW_BINDING set",
                       (eg2_kmgr_reg(KEYMGR_DPE_ENFORCE_SW_BINDING_REG_OFFSET) & 0xf) !=
                           kMultiBitBool4False,
                       eg2_kmgr_reg(KEYMGR_DPE_ENFORCE_SW_BINDING_REG_OFFSET)) &&
       ok;

  uint32_t err = 0;
  if (empty < kEg2KmgrSlots) {
    uint32_t st = eg2_kmgr_op(kEg2KmgrOpLoadRoot, 0, 0, empty, false, &err);
#ifdef EG2_BOOT_KEYMGR_STAGE_INJECT_FAIL
    ok = eg2_boot_check("Load Root Key refused", kEg2KmgrDone, st) && ok;
#else
    ok = eg2_boot_check("Load Root Key refused", kEg2KmgrDoneError, st) && ok;
    ok = eg2_boot_check("Load Root Key: INVALID_OP", kInvalidOp, err & kInvalidOp) && ok;
#endif
    ok = eg2_boot_check("no slot gained the UDS", 0, eg2_kmgr_meta_hi(empty) & 1) && ok;
    // Key generations from the empty slot, SW and every sideload destination.
    for (uint32_t dest = 0; dest <= 4; ++dest) {
      uint32_t op = dest == 0 ? kEg2KmgrOpGenSw : kEg2KmgrOpGenHw;
      ok = eg2_boot_check("generation from an empty slot refused", kEg2KmgrDoneError,
                          eg2_kmgr_op(op, dest, empty, 0, false, NULL)) &&
           ok;
    }
  } else {
    ok = eg2_boot_expect("a free slot to test Load Root Key", false, 0) && ok;
  }
  if (owner_slot < kEg2KmgrSlots && empty < kEg2KmgrSlots) {
    // An advance consuming the hardware binding values: into a free slot if the owner slot keeps
    // its parent, else in place (the policy's rule). Both ENFORCE_SW_BINDING and the documented
    // stage limit (`boot_stage + 1 < NumBootStages`, ticket F16) refuse it.
    bool retain = (eg2_kmgr_meta_hi(owner_slot) >>
                   KEYMGR_DPE_METADATA_HIGH_0_RETAIN_PARENT_POLICY_0_BIT) & 1;
    eg2_kmgr_reg_wr(KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, 1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_KEYGEN_BIT);
    uint32_t st = eg2_kmgr_op(kEg2KmgrOpAdvance, 0, owner_slot, retain ? empty : owner_slot, false,
                              &err);
    ok = eg2_boot_check("advance from the owner slot with hardware bindings refused",
                        kEg2KmgrDoneError, st) &&
         ok;
  }
  return eg2_boot_summary() && ok;
}
