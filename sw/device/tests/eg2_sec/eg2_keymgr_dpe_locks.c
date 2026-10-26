// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_keymgr_dpe_locks (EG2 verification spec REQ_SEC_10, "Security in RISC-V mode"):
 * the software binding and the maximum key version are locked by their REGWEN, and a versioned
 * key above the maximum is refused. Both modes (the `_cheriot` build reaches keymgr_dpe through
 * its window capability).
 *
 * Oracle: keymgr_dpe's documentation (theory of operation, programmer's guide, keymgr_dpe.hjson):
 *  - SW_BINDING_REGWEN, MAX_KEY_VER_REGWEN, SLOT_POLICY_REGWEN are rw0c: once 0, writes to the
 *    register they guard have no effect "until the next advance call";
 *  - the advance copies SW_BINDING into the child's derivation, MAX_KEY_VER into the child's
 *    `max_key_version` and SLOT_POLICY into its `key_policy` (METADATA_LOW / METADATA_HIGH);
 *  - a versioned key is generated only if KEY_VERSION <= the source slot's `max_key_version`;
 *    otherwise the operation completes with DONE_ERROR and ERR_CODE.INVALID_OP.
 *
 * Steps:
 *  1. The first advance latches the UDS into slot 0 (Reset -> Available).
 *  2. SW_BINDING, MAX_KEY_VER = `max_ver` and SLOT_POLICY are written, then their REGWENs cleared.
 *  3. Each locked register (`regs` bit 0 SW_BINDING, bit 1 MAX_KEY_VER, bit 2 SLOT_POLICY) is
 *     written with another value (`wclass` 0: random, 1: all ones, 2: the value with one bit
 *     flipped): it reads back unchanged, and its REGWEN still reads 0.
 *  4. A refused operation (an erase of an empty slot) does not reopen the locks.
 *  5. The advance (slot 0 in place, the UDS policy requires it): slot 0's metadata holds the
 *     locked values, not the ones written after the lock.
 *  6. Versioned SW keys from slot 0 with KEY_VERSION below, at and above `max_ver` (`versions`
 *     bit 0, 1, 2): below and at succeed, above fails with INVALID_OP and leaves no key.
 *
 * The spec's REQ_SEC_10 says "locked ... until reset"; the keymgr_dpe documentation says the
 * advance reopens SW_BINDING_REGWEN and SLOT_POLICY_REGWEN. Step 5 also checks the documented
 * reopening (reported, see the spec's table note).
 *
 * Built with EG2_KEYMGR_DPE_LOCKS_INJECT_FAIL the version above the maximum is expected to
 * succeed: the run must fail with `EG2CHK FAIL generate above max`.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define KEYMGR_LOCKS_PARAMS(X) \
  X(regs, 7, 1, 7)             \
  X(versions, 7, 1, 7)         \
  X(wclass, 0, 0, 2)           \
  X(max_ver, 5, 1, 0xfffffffe)
EG2_RM_PARAMS(KEYMGR_LOCKS_PARAMS)

enum {
  kSlot = 0,
  kPolicy = (1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_CHILD_BIT) |
            (1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_KEYGEN_BIT),
  kInvalidOp = 1u << 0,
};

static uint32_t other_value(uint32_t v) {
  switch (EG2_PARAM(wclass)) {
    case 1:
      return v == 0xffffffffu ? 0 : 0xffffffffu;
    case 2:
      return v ^ (1u << eg2_rand_range(0, 31));
    default: {
      uint32_t r = eg2_rand();
      return r == v ? ~v : r;
    }
  }
}

static bool generate(const char *what, uint32_t version, bool expect_ok) {
  eg2_kmgr_reg_wr(KEYMGR_DPE_KEY_VERSION_REG_OFFSET, version);
  for (uint32_t i = 0; i < 8; ++i) {
    eg2_kmgr_reg_wr(KEYMGR_DPE_SALT_0_REG_OFFSET + 4 * i, eg2_rand());
  }
  uint32_t err = 0;
  uint32_t status = eg2_kmgr_op(kEg2KmgrOpGenSw, 0, kSlot, 0, false, &err);
  uint32_t s0[8], s1[8];
  eg2_kmgr_sw_output(s0, s1);
  uint32_t any = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    any |= s0[i] | s1[i];
  }
  bool ok = eg2_boot_check(what, expect_ok ? kEg2KmgrDone : kEg2KmgrDoneError, status);
  if (expect_ok) {
    ok = eg2_boot_expect("generated key is not zero", any != 0, any) && ok;
  } else {
    ok = eg2_boot_check("refused generate: ERR_CODE.INVALID_OP", kInvalidOp, err & kInvalidOp) &&
         ok;
    ok = eg2_boot_check("refused generate leaves no key", 0, any) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  const uint32_t max_ver = EG2_PARAM(max_ver);
  int alert = eg2_status_alert_find("keymgr_dpe_recov_operation_err");

  // 1. The UDS into slot 0.
  ok = eg2_boot_check("state at start", kEg2KmgrReset, eg2_kmgr_state()) && ok;
  eg2_kmgr_reg_wr_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 0);
  ok = eg2_boot_check("first advance", kEg2KmgrDone,
                      eg2_kmgr_op(kEg2KmgrOpAdvance, 0, 0, kSlot, false, NULL)) &&
       ok;
  ok = eg2_boot_check("state after the first advance", kEg2KmgrAvailable, eg2_kmgr_state()) &&
       ok;

  // 2. The values the advance must use, then the locks.
  uint32_t binding[8];
  for (uint32_t i = 0; i < 8; ++i) {
    binding[i] = eg2_rand();
    eg2_kmgr_reg_wr(KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + 4 * i, binding[i]);
  }
  eg2_kmgr_reg_wr_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, max_ver);
  eg2_kmgr_reg_wr(KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, kPolicy);
  eg2_kmgr_reg_wr(KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET, 0);
  eg2_kmgr_reg_wr(KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET, 0);
  eg2_kmgr_reg_wr(KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET, 0);

  // 3. Writes after the lock.
  if (EG2_PARAM(regs) & 1) {
    for (uint32_t i = 0; i < 8; ++i) {
      eg2_kmgr_reg_wr(KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + 4 * i, other_value(binding[i]));
    }
    bool same = true;
    for (uint32_t i = 0; i < 8; ++i) {
      same = same && eg2_kmgr_reg(KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + 4 * i) == binding[i];
    }
    ok = eg2_boot_expect("SW_BINDING unchanged after its lock", same,
                         eg2_kmgr_reg(KEYMGR_DPE_SW_BINDING_0_REG_OFFSET)) &&
         ok;
    ok = eg2_boot_check("SW_BINDING_REGWEN after the write", 0,
                        eg2_kmgr_reg(KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET)) &&
         ok;
  }
  if (EG2_PARAM(regs) & 2) {
    eg2_kmgr_reg_wr_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, other_value(max_ver));
    ok = eg2_boot_check("MAX_KEY_VER unchanged after its lock", max_ver,
                        eg2_kmgr_reg(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET)) &&
         ok;
    ok = eg2_boot_check("MAX_KEY_VER_REGWEN after the write", 0,
                        eg2_kmgr_reg(KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET)) &&
         ok;
  }
  if (EG2_PARAM(regs) & 4) {
    eg2_kmgr_reg_wr(KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, (~kPolicy) & 0xf);
    ok = eg2_boot_check("SLOT_POLICY unchanged after its lock", kPolicy,
                        eg2_kmgr_reg(KEYMGR_DPE_SLOT_POLICY_REG_OFFSET)) &&
         ok;
  }

  // 4. A refused operation does not reopen the locks.
  if (alert >= 0) {
    eg2_status_expect_alert((size_t)alert);
  }
  uint32_t err = 0;
  ok = eg2_boot_check("erase of an empty slot refused", kEg2KmgrDoneError,
                      eg2_kmgr_op(kEg2KmgrOpErase, 0, 0, kSlot + 1, false, &err)) &&
       ok;
  ok = eg2_boot_check("SW_BINDING_REGWEN after a refused operation", 0,
                      eg2_kmgr_reg(KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET)) &&
       ok;
  ok = eg2_boot_check("MAX_KEY_VER_REGWEN after a refused operation", 0,
                      eg2_kmgr_reg(KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET)) &&
       ok;

  // 5. The advance uses the locked values.
  ok = eg2_boot_check("advance with the locked values", kEg2KmgrDone,
                      eg2_kmgr_op(kEg2KmgrOpAdvance, 0, kSlot, kSlot, false, NULL)) &&
       ok;
  uint32_t hi = eg2_kmgr_meta_hi(kSlot);
  ok = eg2_boot_check("slot valid", 1, hi & 1) && ok;
  ok = eg2_boot_check("slot boot stage", 1,
                      (hi >> KEYMGR_DPE_METADATA_HIGH_0_BOOT_STAGE_0_OFFSET) &
                          KEYMGR_DPE_METADATA_HIGH_0_BOOT_STAGE_0_MASK) &&
       ok;
  ok = eg2_boot_check("slot policy is the locked SLOT_POLICY", kPolicy,
                      (hi >> KEYMGR_DPE_METADATA_HIGH_0_ALLOW_CHILD_POLICY_0_BIT) & 0xf) &&
       ok;
  ok = eg2_boot_check("slot max_key_version is the locked MAX_KEY_VER", max_ver,
                      eg2_kmgr_meta_lo(kSlot)) &&
       ok;
  ok = eg2_boot_check("SW_BINDING_REGWEN reopened by the advance (keymgr_dpe doc)", 1,
                      eg2_kmgr_reg(KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET)) &&
       ok;

  // 6. Versions around the maximum.
  if (EG2_PARAM(versions) & 1) {
    ok = generate("generate below max", max_ver - 1, true) && ok;
  }
  if (EG2_PARAM(versions) & 2) {
    ok = generate("generate at max", max_ver, true) && ok;
  }
  if (EG2_PARAM(versions) & 4) {
#ifdef EG2_KEYMGR_DPE_LOCKS_INJECT_FAIL
    ok = generate("generate above max", max_ver + 1, true) && ok;
#else
    ok = generate("generate above max", max_ver + 1, false) && ok;
#endif
  }
  if (alert >= 0) {
    eg2_status_wait_alert((size_t)alert, 1000);
  }
  return eg2_boot_summary() && ok;
}
