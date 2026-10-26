// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_keymgr_fault (EG2 verification spec REQ_PBF_15, "Post-boot faults"): an
 * escalation during a key-manager advance or generate, in both modes.
 *
 * Boot 0 (and the reset keymgr_dpe_testutils makes when it provisions the secrets from the test
 * ROM): the key manager reaches an owner-stage key in slot 1; a software output is generated and
 * kept (the key before the fault). Then the operation `op` (0 advance slot 1 to slot 2, 1 generate
 * a software output) and the escalation at `fault_point` (0 just before the operation is
 * started, 1 right after it was started, 2 after it completed): class A escalates
 * keymgr_dpe_recov_operation_err (raised through ALERT_TEST) to the life-cycle escalation, which
 * deactivates the key manager (theory of operation "Life Cycle Connection": Invalid, "the
 * internal key, the sideload key slots and the software key are all wiped"), then to the
 * escalation reset.
 * The core stops fetching during the life-cycle escalation, so the wipe itself is the chip
 * bench's check before the reset (chip_sw_eg2_pbf_keymgr_fault_vseq: WORKING_STATE Invalid and
 * the software output registers no longer the key kept).
 * Boot 1: the escalation reset and its crash dump (eg2_pbf_escalation_check), WORKING_STATE Reset
 * and the software output registers at their reset value: no key survives the fault.
 * Knobs: op (0-1, 2 drawn), fault_point (0-2, 3 drawn).
 *
 * With EG2_PBF_INJECT_FAIL the first value check after the fault (the reset reason) expects bit 0
 * flipped: `PBF FAIL`.
 */

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/testing/keymgr_dpe_testutils.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

EG2_TEST_CONFIG();

#define KEYMGR_FAULT_PARAMS(X) \
  X(op, 2, 0, 2)               \
  X(fault_point, 3, 0, 3)
EG2_RM_PARAMS(KEYMGR_FAULT_PARAMS)

enum {
  kRetPhase = 0,  // 0 before the fault, 1 after it was set off
  kRetAlert = 1,
  kRetOp = 2,
  kRetPoint = 3,
  kPhaseFault = 0x46415554,  // "FAUT"
};

static bool before(void) {
  dif_keymgr_dpe_t km;
  uint32_t max = 0;
  bool ok = eg2_pbf_step("kmac init for keymgr", eg2_pbf_kmac_init(true));
  ok = eg2_pbf_step("keymgr_dpe up to an owner-stage key in slot 1", eg2_pbf_keymgr_up(&km, &max)) &&
       ok;
  uint32_t salt[8], k0[8], k1[8];
  for (uint32_t i = 0; i < 8; ++i) {
    salt[i] = eg2_rand();
  }
  bool op_ok = false;
  ok = eg2_pbf_step("keymgr generates a key before the fault",
                    eg2_pbf_keymgr_generate(&km, kDifKeymgrDpeKeyDestNone, false, salt, 0, &op_ok,
                                            k0, k1) &&
                        op_ok) &&
       ok;
  if (!ok) {
    return false;
  }
  uint32_t op = EG2_PARAM(op) == 2 ? eg2_rand_range(0, 1) : EG2_PARAM(op);
  uint32_t point = EG2_PARAM(fault_point) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(fault_point);
  *eg2_pbf_ret(kRetOp) = op;
  *eg2_pbf_ret(kRetPoint) = point;
  *eg2_pbf_ret(kRetPhase) = kPhaseFault;
  eg2_pbf_note(op == 0 ? "fault during an advance, fault_point" : "fault during a generate, "
                                                                  "fault_point",
               point);
  eg2_pbf_activity("keymgr operations before the fault", 1);
  if (point == 0) {
    eg2_pbf_escalate_now("keymgr_dpe_recov_operation_err", kRetAlert);
  }
  if (op == 0) {
    dif_keymgr_dpe_advance_params_t adv = kOwnerKeyParams;
    adv.slot_src_sel = 1;
    adv.slot_dst_sel = 2;
    (void)dif_keymgr_dpe_advance_state(&km, &adv);
  } else {
    dif_keymgr_dpe_generate_params_t gen = {
        .key_dest = kDifKeymgrDpeKeyDestNone,
        .sideload_key = false,
        .version = 0,
        .slot_src_sel = 1,
    };
    memcpy(gen.salt, salt, sizeof(salt));
    (void)dif_keymgr_dpe_generate(&km, &gen);
  }
  if (point == 2) {
    dif_keymgr_dpe_status_codes_t sc = 0;
    for (uint32_t i = 0; i < 200000 && sc == 0; ++i) {
      (void)dif_keymgr_dpe_get_status_codes(&km, &sc);
    }
  }
  eg2_pbf_escalate_now("keymgr_dpe_recov_operation_err", kRetAlert);
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("keymgr_fault", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  (void)eg2_pbf_boot_index();
  if (*eg2_pbf_ret(kRetPhase) != kPhaseFault) {
    return before() && ok;
  }
  ok = eg2_pbf_escalation_check(kRetAlert) && ok;
  ok = eg2_pbf_check("keymgr_dpe WORKING_STATE Reset after the escalation reset", 0,
                     eg2_pbf_keymgr_state()) &&
       ok;
  uint32_t any = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    any |= eg2_dev_read32(kEg2DevKeymgrDpe, KEYMGR_DPE_SW_SHARE0_OUTPUT_0_REG_OFFSET + 4 * i);
    any |= eg2_dev_read32(kEg2DevKeymgrDpe, KEYMGR_DPE_SW_SHARE1_OUTPUT_0_REG_OFFSET + 4 * i);
  }
  ok = eg2_pbf_check("keymgr software output at its reset value (no key survives)", 0, any) && ok;
  eg2_pbf_activity("faults injected", 1);
  return ok;
}
