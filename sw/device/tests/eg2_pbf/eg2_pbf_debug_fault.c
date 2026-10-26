// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_debug_fault (EG2 verification spec REQ_PBF_20, REQ_PBF_07, "Post-boot
 * faults", "Debug Controller"): a reset or an escalation while the debugger has the core halted,
 * or while a system bus access is in flight.
 *
 * Boot 1: the test asks the bench to halt it (bench step 1) and waits for the reset.
 *   fault 0, a reset: the debugger's non-debug-module reset (dmcontrol.ndmreset), issued by the
 *     bench while the core is halted (fault_point 0) or right after it starts an SBA write of
 *     `mailbox_value` to the mailbox (fault_point 1);
 *   fault 1, an escalation: before the step the test arms class A on rv_core_ibex's fatal_sw_err
 *     to escalate to the escalation reset after `esc_cycles`, and raises it right after the step:
 *     the escalation lands while the bench holds the core halted (and, at fault_point 1, while
 *     its SBA write is in flight).
 * Boot 2 (software): the reset reason is the fault's (RESET_INFO.HW_REQ: rv_dm's ndm reset, or the
 * escalation); the mailbox (retention SRAM, kept across both resets) holds its old value 0 or
 * the whole of `mailbox_value`, never a part of it ("the access completes or errors, never half
 * done"), and 0 when no SBA access was made (fault_point 0); for an escalation the alert crash
 * dump holds fatal_sw_err.
 * The bench checks the debug module's state after the reset (chip_sw_eg2_pbf_debug_fault_vseq):
 * after the escalation reset the debug module is back at its reset state (dmcontrol.dmactive 0);
 * after an ndm reset it is not reset, by definition (the RISC-V debug specification: ndmreset
 * resets everything but the debug module), and the core runs again (dmstatus.allrunning).
 *
 * With debug disabled by the life cycle (PROD) the bench cannot halt the core; a reset then comes
 * from the test (a software reset in place of the ndm reset, reported as a gap), and so it does in
 * the SystemC model (model_gaps), which has no debug module.
 *
 * Knobs: fault_point, fault, esc_cycles, mailbox_value, model_gaps.
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "hw/top/rstmgr_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define DEBUG_FAULT_PARAMS(X)                     \
  X(fault_point, 1, 0, 1)                         \
  X(fault, 1, 0, 1)                               \
  X(esc_cycles, 2000, 100, 100000)                \
  X(mailbox_value, 0x0dbe5ba1, 1, 0xffffffff)     \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(DEBUG_FAULT_PARAMS)

enum {
  kMailbox = kEg2PbfRetFree,
  kResetWait = 4000000,
  kRetPhase = 0,
  kRetSwReset = 1,
  kPhaseHalt = 0x48414c54,  // "HALT"
  kResetSw = 1u << RSTMGR_RESET_INFO_SW_RESET_BIT,
  kResetEscalation = 1u << (RSTMGR_RESET_INFO_HW_REQ_OFFSET + 3),
  kResetNdm = 1u << (RSTMGR_RESET_INFO_HW_REQ_OFFSET + 4),
};

static bool boot_1(bool debug) {
  *eg2_pbf_word(kMailbox) = 0;
  *eg2_pbf_ret(kRetPhase) = kPhaseHalt;
  uint32_t alert = eg2_pbf_alert("rv_core_ibex_fatal_sw_err");
  bool escalate = EG2_PARAM(fault) == 1;
  if (escalate) {
    eg2_pbf_escalate_reset(&alert, 1, EG2_PARAM(esc_cycles));
    eg2_status_expect_alert(alert);
    eg2_pbf_crashdump_arm();
  }
  bool bench = debug && !EG2_PARAM(model_gaps);
  *eg2_pbf_ret(kRetSwReset) = (!escalate && !bench) ? 1 : 0;
  eg2_pbf_bench_step(1);  // the bench halts the core from here on
  if (escalate) {
    eg2_status_raise_alert(alert);
  } else if (!bench) {
    if (EG2_PARAM(model_gaps)) {
      eg2_pbf_model_gap("debug module: the ndm reset (a software reset in its place)");
    } else {
      eg2_pbf_gap("ndm reset", "debug is disabled in this life cycle state; a software reset in "
                               "its place");
    }
    eg2_pbf_sw_reset();
  }
  eg2_pbf_spin(kResetWait);
  return eg2_pbf_step("reset while halted", false);
}

static bool boot_2(void) {
  bool ok = eg2_pbf_check("phase before the reset", kPhaseHalt, *eg2_pbf_ret(kRetPhase));
  uint32_t want = EG2_PARAM(fault) == 1 ? kResetEscalation
                  : *eg2_pbf_ret(kRetSwReset) ? kResetSw
                                              : kResetNdm;
  ok = eg2_pbf_check("reset reason", want, eg2_pbf_reset_reason()) && ok;
  uint32_t m = *eg2_pbf_word(kMailbox);
  eg2_pbf_note("mailbox after the reset", m);
  if (EG2_PARAM(fault_point) == 0) {
    ok = eg2_pbf_check("mailbox with no SBA access", 0, m) && ok;
  } else {
    ok = eg2_pbf_step("mailbox old or new, never a part", m == 0 || m == EG2_PARAM(mailbox_value)) &&
         ok;
    eg2_pbf_activity("sba writes completed before the reset", m != 0);
  }
  if (EG2_PARAM(fault) == 1) {
    uint32_t a = eg2_pbf_alert("rv_core_ibex_fatal_sw_err");
    ok = eg2_pbf_check("alert crash dump: fatal_sw_err", 1, eg2_pbf_crashdump_alert(a)) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("debug_fault", EG2_PBF_INJECT);
  uint32_t lc = eg2_pbf_lc_state();
  bool debug = eg2_pbf_lc_debug_enabled(lc);
  eg2_puts("PBF debug_fault: life cycle ");
  eg2_puts(eg2_pbf_lc_name(lc));
  eg2_puts(debug ? ", debug enabled\r\n" : ", debug disabled\r\n");
  if (eg2_pbf_boot_index() == 0) {
    return boot_1(debug) && ok;
  }
  return boot_2() && ok;
}
