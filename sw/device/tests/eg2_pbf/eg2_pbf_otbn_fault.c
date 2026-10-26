// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_otbn_fault (EG2 verification spec REQ_PBF_11, "Post-boot faults"): an ECDSA
 * P-256 operation on OTBN faulted while it runs, in both modes.
 *
 * fault (drawn by the bench, chip_sw_eg2_pbf_otbn_fault_vseq, which makes the integrity errors):
 *   0  IMEM integrity error: the bench flips one bit of every IMEM word through the backdoor
 *      while OTBN runs (`fault_point`: early, middle, late in the operation);
 *   1  DMEM integrity error: the same for every DMEM word;
 *   2  escalation: class A escalates otbn_recov (raised through ALERT_TEST) while OTBN runs: the
 *      life-cycle escalation reaches OTBN (a fatal LIFECYCLE_ESCALATION error) and the core,
 *      then the escalation reset.
 * Check, from the otbn theory of operation "Reaction to Fatal Errors":
 *   faults 0, 1  the operation ends: STATUS LOCKED, ERR_BITS exactly the integrity violation of
 *                the memory, FATAL_ALERT_CAUSE the same, otbn_fatal raised (declared to the
 *                status module), DMEM reads 0 (inaccessible when locked: wiped and locked), the
 *                driver reports an error; then a software reset (a locked OTBN leaves LOCKED only
 *                through reset) and, on the next boot, OTBN IDLE again and one verification of a
 *                NIST vector passes;
 *   fault 2      on the next boot: the escalation reset and the alert crash dump
 *                (eg2_pbf_escalation_check); OTBN IDLE again and a verification passes. OTBN's
 *                lock before the reset is the bench's check (the vseq samples STATUS).
 * Knobs: fault (0-2; the bench's draw), fault_point (0-2), model_gaps.
 *
 * | Skipped with model_gaps = 1 | Why |
 * |---|---|
 * | the OTBN operation and the lock checks | OTBN is not in the SystemC model (dv_todo decision
 *   6); the escalation path still runs there, without an OTBN operation in flight |
 *
 * With EG2_PBF_INJECT_FAIL the first value check expects bit 0 flipped: `PBF FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/otbn_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_otbn.h"

EG2_TEST_CONFIG();

#define OTBN_FAULT_PARAMS(X) \
  X(fault, 1, 0, 2)          \
  X(fault_point, 1, 0, 2)    \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(OTBN_FAULT_PARAMS)

enum {
  kPolls = 4000000,
  kRetFault = 0,
  kRetAlert = 1,
};

static bool recover(void) {
  bool ok = eg2_pbf_entropy_up();
  ok = eg2_pbf_step("otbn leaves its post-reset secure wipe",
                    eg2_pbf_otbn_wait_not(0x04, kPolls)) &&
       ok;
  ok = eg2_pbf_check("otbn STATUS IDLE after the reset", kEg2PbfOtbnIdle, eg2_pbf_otbn_status()) &&
       ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("OTBN verification after the reset");
    return ok;
  }
  bool valid = false;
  ok = eg2_pbf_step("otbn verify after the reset",
                    eg2_pbf_otbn_verify_start(&kEg2PbfKatEcdsa[1], false) &&
                        eg2_pbf_otbn_verify_end(&valid)) &&
       ok;
  ok = eg2_pbf_check("otbn verdict after the reset = NIST (valid)", 1, valid) && ok;
  return ok;
}

static bool boot_0(void) {
  uint32_t fault = EG2_PARAM(fault);
  *eg2_pbf_ret(kRetFault) = fault;
  eg2_pbf_note("fault (0 IMEM, 1 DMEM integrity, 2 escalation)", fault);
  bool ok = eg2_pbf_entropy_up();
  ok = eg2_pbf_step("otbn leaves its post-reset secure wipe",
                    eg2_pbf_otbn_wait_not(0x04, kPolls)) &&
       ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("OTBN operation in flight");
    if (fault == 2) {
      eg2_pbf_escalate_now("otbn_recov", kRetAlert);
    }
    eg2_pbf_model_gap("IMEM/DMEM integrity errors (made by the bench)");
    eg2_pbf_sw_reset();
  }
  // A verification: about 0.3 million OTBN cycles, long enough for the bench's fault.
  ok = eg2_pbf_step("otbn verify starts", eg2_pbf_otbn_verify_start(&kEg2PbfKatEcdsa[1], false)) &&
       ok;
  eg2_pbf_activity("otbn operations started", 1);
  if (fault == 2) {
    eg2_pbf_escalate_now("otbn_recov", kRetAlert);
  }
  int fatal = eg2_status_alert_find("otbn_fatal");
  if (fatal >= 0) {
    eg2_status_expect_alert((size_t)fatal);
  }
  ok = eg2_pbf_step("otbn operation ends", eg2_pbf_otbn_wait_not(kEg2PbfOtbnBusyExecute, kPolls)) &&
       ok;
  uint32_t err = fault == 0 ? OTBN_ERR_BITS_IMEM_INTG_VIOLATION_BIT
                            : OTBN_ERR_BITS_DMEM_INTG_VIOLATION_BIT;
  uint32_t cause = fault == 0 ? OTBN_FATAL_ALERT_CAUSE_IMEM_INTG_VIOLATION_BIT
                              : OTBN_FATAL_ALERT_CAUSE_DMEM_INTG_VIOLATION_BIT;
  ok = eg2_pbf_check("otbn STATUS LOCKED", kEg2PbfOtbnLocked, eg2_pbf_otbn_status()) && ok;
  ok = eg2_pbf_check("otbn ERR_BITS: the integrity violation", 1u << err,
                     eg2_pbf_otbn_err_bits()) &&
       ok;
  ok = eg2_pbf_check("otbn FATAL_ALERT_CAUSE: the integrity violation", 1u << cause,
                     eg2_pbf_otbn_fatal_cause()) &&
       ok;
  uint32_t any = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    any |= eg2_pbf_otbn_dmem_word(i);
  }
  ok = eg2_pbf_check("otbn DMEM reads 0 when locked (wiped, inaccessible)", 0, any) && ok;
  bool valid = true;
  ok = eg2_pbf_step("the driver reports the faulted verification as an error",
                    !eg2_pbf_otbn_verify_end(&valid)) &&
       ok;
  ok = eg2_pbf_step("otbn_fatal reached the alert handler",
                    fatal >= 0 && eg2_status_wait_alert((size_t)fatal, 10000)) &&
       ok;
  if (!ok) {
    return false;  // the next boot would hide the failure
  }
  // Only a reset unlocks OTBN; the next boot checks it recovered.
  eg2_pbf_sw_reset();
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("otbn_fault", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  if (eg2_pbf_boot_index() == 0) {
    return boot_0() && ok;
  }
  uint32_t fault = *eg2_pbf_ret(kRetFault);
  if (fault == 2) {
    ok = eg2_pbf_escalation_check(kRetAlert) && ok;
  } else {
    ok = eg2_pbf_check("reset reason: software reset", 1u << 2, eg2_pbf_reset_reason()) && ok;
  }
  return recover() && ok;
}
