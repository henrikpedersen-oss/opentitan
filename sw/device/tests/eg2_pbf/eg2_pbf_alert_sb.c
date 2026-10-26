// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_alert_sb (EG2 verification spec REQ_PBF_17, "Post-boot features in CHERIoT
 * mode"): a class's escalation after secure boot, in RISC-V and CHERIoT mode. Expected values from
 * the alert handler's documentation (hw/top_earlgrey/ip_autogen/alert_handler/doc: "Escalation",
 * the class state encoding of CLASSx_STATE, the crash dump of rstmgr's ALERT_INFO).
 *
 * One alert, drawn from every alert of the chip with the seed, goes to class `alert_class` (A, B,
 * C), which escalates on its first alert (ACCUM_THRESH 0) through Phase0..Phase3, each
 * kPhaseCycles long. Escalation signal 3 (pwrmgr's escalation reset) is mapped to Phase3; signals
 * 0-2 (the NMI and life-cycle escalation) stay off, so the core runs through the escalation and
 * watches it, in both modes (the CHERIoT start-up code ends a test on any trap, an NMI included).
 *
 *   stop_phase 4: the class is locked (CTRL.LOCK). Boot 0 watches CLASSx_STATE pass Phase0, 1, 2 in
 *     that order, then the escalation reset ends the boot. Boot 1: the reset reason is the
 *     escalation alone; rstmgr's crash dump (latched on entering Phase `dump_phase`) holds the
 *     class in that phase, its accumulator at 1 and the alert's cause.
 *   stop_phase 0-3: the class is not locked. Boot 0 waits for Phase `stop_phase`, checks
 *     CLR_REGWEN still 1, clears the class (CLASSx_CLR) and checks it back in Idle with its
 *     accumulator 0; no reset follows.
 *
 * Knobs: alert_class 0-2, stop_phase 0-4, dump_phase 0-3, model_gaps (0 on the chip).
 * Activity (the bench): the class's escalation state leaves Idle; with stop_phase 4 pwrmgr's
 * escalation reset request.
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (the first state reached) expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define ALERT_SB_PARAMS(X) \
  X(alert_class, 0, 0, 2)  \
  X(stop_phase, 4, 0, 4)   \
  X(dump_phase, 1, 0, 3)   \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(ALERT_SB_PARAMS)

enum {
  kPhaseCycles = 24000,  // io_div4 cycles per phase (1 ms)
  kPolls = 2000000,
  kStateIdle = 0,
  kStatePhase0 = 4,
  // Retention words.
  kRetAlert = 0,
  kRetSeen = 1,
  kRetOrder = 2,
  kRetFirst = 3,
};

static bool boot_0(eg2_pbf_class_t cls, uint32_t alert) {
  bool lock = EG2_PARAM(stop_phase) == 4;
  eg2_pbf_esc_t esc = {
      .cls = cls,
      .accum_thresh = 0,
      .phase_cycles = {kPhaseCycles, kPhaseCycles, kPhaseCycles, kPhaseCycles},
      .signal_en = lock ? 1u << 3 : 0,
      // MAP_Ex: the phase that asserts escalation signal x.
      .map = {0, 1, 2, 3},
      .crashdump_phase = EG2_PARAM(dump_phase),
      .lock = lock,
  };
  *eg2_pbf_ret(kRetAlert) = alert;
  *eg2_pbf_ret(kRetSeen) = 0;
  *eg2_pbf_ret(kRetOrder) = 0;
  uint32_t a = alert;
  eg2_pbf_escalate(&esc, &a, 1);
  if (lock) {
    sys_crashdump_enable();
  }
  eg2_status_expect_alert(alert);
  // The class's interrupt (alert_handler INTR_STATE.CLASSx) is raised with the alert.
  eg2_status_expect_intr(kEg2DevAlertHandler, 1u << (uint32_t)cls);
  eg2_pbf_note("alert raised", alert);
  eg2_puts("PBF note alert: ");
  eg2_puts(eg2_status_alert_name(alert));
  eg2_puts("\r\n");
  eg2_status_raise_alert(alert);

  // Watch the phases without printing (the UART's 10 us per character would miss them): each state
  // at least the one before, recorded in retention SRAM as it changes, so boot 1 checks them.
  uint32_t last = kStateIdle, seen = 0, first = kStateIdle;
  uint32_t target = lock ? 7 : kStatePhase0 + EG2_PARAM(stop_phase);
  bool ordered = true;
  for (uint32_t i = 0; i < kPolls; ++i) {
    uint32_t s = eg2_pbf_class_state(cls);
    if (s != last) {
      if (first == kStateIdle) {
        first = s;
        *eg2_pbf_ret(kRetFirst) = s;
      }
      if (s < last || s < kStatePhase0) {
        ordered = false;
      }
      seen |= 1u << s;
      *eg2_pbf_ret(kRetSeen) = seen;
      *eg2_pbf_ret(kRetOrder) = ordered ? 0 : 1;
      last = s;
    }
    if (s >= target) {
      break;
    }
  }
  if (lock) {
    // The escalation reset in Phase3 ends this boot; boot 1 checks what this boot recorded.
    for (uint32_t i = 0; i < kPolls; ++i) {
      eg2_pbf_spin(10);
    }
    return eg2_pbf_step("escalation reset came", false);
  }
  // Unlocked: stop at the stop phase. The checks after the clear only: the class must not move on
  // while this boot prints.
  uint32_t clr_regwen = sys_class_clr_regwen(cls);
  eg2_pbf_class_clear(cls);
  uint32_t after = eg2_pbf_class_state(cls), accum = eg2_pbf_class_accum(cls);
  bool ok = eg2_pbf_check("first state after the alert (Phase0)", kStatePhase0, first);
  eg2_pbf_note("states seen (bit = CLASSx_STATE)", seen);
  ok = eg2_pbf_step("phases in order", ordered) && ok;
  ok = eg2_pbf_check("state at the stop phase", target, last) && ok;
  ok = eg2_pbf_check("CLR_REGWEN of an unlocked class", 1, clr_regwen) && ok;
  ok = eg2_pbf_check("state after the clear (Idle)", kStateIdle, after) && ok;
  ok = eg2_pbf_check("accumulator after the clear", 0, accum) && ok;
  eg2_pbf_activity("escalation phases", (uint32_t)__builtin_popcount(seen));
  return ok;
}

static bool boot_1(eg2_pbf_class_t cls) {
  uint32_t alert = *eg2_pbf_ret(kRetAlert);
  uint32_t seen = *eg2_pbf_ret(kRetSeen);
  bool ok = eg2_pbf_check("first state after the alert (Phase0)", kStatePhase0,
                          *eg2_pbf_ret(kRetFirst));
  ok = eg2_pbf_check("reset reason: the escalation alone", kSysRstEscalation,
                     eg2_pbf_reset_reason()) &&
       ok;
  // Phase0..Phase2 watched in boot 0, in order (Phase3 ends in the reset at once).
  uint32_t want = (1u << 4) | (1u << 5) | (1u << 6);
  ok = eg2_pbf_check("phases watched before the reset", want, seen & want) && ok;
  ok = eg2_pbf_step("phases in order", *eg2_pbf_ret(kRetOrder) == 0) && ok;
  uint32_t dump[kSysDumpMaxWords] = {0};
  uint32_t n = sys_crashdump_read(dump, kSysDumpMaxWords);
  eg2_pbf_note("crash dump words", n);
  for (uint32_t i = 0; i < n && i < kSysDumpMaxWords; ++i) {
    eg2_pbf_note("crash dump", dump[i]);
  }
  if (EG2_PARAM(model_gaps) && n == 0) {
    eg2_pbf_model_gap("crash dump");
  } else {
    ok = eg2_pbf_check("crash dump: class state at the dump phase",
                       kStatePhase0 + EG2_PARAM(dump_phase), sys_dump_state(dump, (uint32_t)cls)) &&
         ok;
    ok = eg2_pbf_check("crash dump: class accumulator", 1, sys_dump_accum(dump, (uint32_t)cls)) &&
         ok;
    ok = eg2_pbf_step("crash dump: the alert's cause", sys_dump_cause(dump, alert)) && ok;
  }
  eg2_pbf_activity("escalation phases", (uint32_t)__builtin_popcount(seen));
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("alert", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  eg2_pbf_boot_stage();
  uint32_t boot = eg2_pbf_boot_index();
  eg2_pbf_class_t cls = (eg2_pbf_class_t)EG2_PARAM(alert_class);
  if (boot == 0) {
    uint32_t alert = eg2_rand_range(0, kEg2AlertCount - 1);
    return boot_0(cls, alert);
  }
  if (boot == 1 && EG2_PARAM(stop_phase) == 4) {
    return boot_1(cls);
  }
  return eg2_pbf_step("no further boot expected", false);
}
