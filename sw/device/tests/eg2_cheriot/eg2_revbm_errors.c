// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revbm_errors (EG2 spec "Revocation", "Corner cases"; REQ_CMS_08, REQ_CIB_03),
 * the part that is not the glitch (the glitch on the load filter is chip_sw_cheriot_trvk_fault).
 * CHERIoT mode only. One case per run (`error`, 0 draws):
 *
 *  1 bus_err  the response to the core's bitmap read (corerevbm) answers d_error, with correct
 *             integrity;
 *  2 intg     that response with one bit of its response integrity, its data integrity or its
 *             data flipped (`field` 1-3, 0 draws; a data bit other than the one the capability
 *             selects);
 *  3 mem_err  a bitmap memory error: the meta SRAM's response to the bitmap read that serves the
 *             core (cheriot_mem_sys's meta_sram_tl) with one data bit flipped, its integrity
 *             unchanged (a bit error in the memory), again not the selected bit;
 *  4 reset    a reset while the core's bitmap read is outstanding (`reset_kind` 1 power-on,
 *             2 NDM, 3 the AON watchdog's bite; 0 draws): the bench holds the read's response
 *             until the reset.
 *
 * Cases 1-3 (bench steps revbm_err_arm/_end, chip_sw_eg2_revbm_errors_vseq). Eight tagged
 * capabilities in sec SRAM's arena B, each based on its own object in arena A (one bitmap word
 * per object); the bitmap is clear, so only the error can revoke. In one op window (eg2_revoke.h):
 * CLC + CGetTag of slot t (the faulted lookup), then of slot n (another word). Checks:
 *  - the bench: the fault took effect; the load filter handed the core slot t's capability with
 *    its tag cleared (ibex_trvk upstream_tag_revoked_o, upstream_tag_o 0 in the verdict cycle),
 *    within a bound (no hang); the alert: rv_core_ibex's fatal_hw_err for cases 1-2 (core spec
 *    REQ_TMP_04 and `cheriot_revoke_load_barrier`: the error word revokes and raises
 *    alert_major_bus; rv_core_ibex interfaces.md: alert_major_bus and bus integrity errors raise
 *    fatal_hw_err), cheriot_mem_sys's fatal_fault for case 3 (theory_of_operation.md "Error
 *    Handling": an integrity fault on a meta SRAM response);
 *  - the software, if it still runs: slot t's CLC returned tag 0, slot n's tag 1, the alert
 *    reached the alert handler.
 * rv_core_ibex theory_of_operation.md: "When the fatal_hw_err alert is raised Ibex fetch is
 * disabled". If it is, the software stops after the fault and the bench ends the run with its
 * own verdict (as chip_sw_cheriot_trvk_fault).
 *
 * Case 4 (bench steps revbm_hold_reset/_end). Boot 1: slot t stored; in one op window the
 * watchdog armed (kind 3), slot t's granule marked, the CLC of slot t; the bench holds that
 * bitmap read and the reset comes while it is outstanding (the bench applies POR or NDM, the
 * watchdog bites). The bench writes `reset_done` during the reset. Boot 2: the reset reason; the
 * bench saw the read held and the reset arrive during the hold; the bitmap word reads 0
 * (REQ_CMS_15: the meta SRAM initialised after every reset); then, fresh slots and one window:
 * slot t unmarked loads tagged (no stale verdict from the read cut by the reset), marked loads
 * untagged, slot n tagged: the filter works after the reset (no hang).
 *
 * `model_gaps` (eg2sim): no bench and no load filter: case 4 is replaced by case 1, nothing is
 * injected, and only the slots' tags and slot n's tag are checked.
 *
 * Built with EG2_REVBM_ERRORS_INJECT_FAIL, slot n is expected untagged in cases 1-3 (`MISMATCH
 * neighbour keeps its tag`) and the marked slot tagged after the reset in case 4 (`MISMATCH
 * marked slot after the reset`). Its chip runs add +eg2_inject_fail=1, which makes the bench
 * expect a tagged response (`EG2CHK FAIL failed check revoked`), for a core whose fetch stops.
 */

#include "hw/top/aon_timer_regs.h"
#include "hw/top/pwrmgr_regs.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVBM_ERRORS_PARAMS(X) \
  X(error, 0, 0, 4)            \
  X(field, 0, 0, 3)            \
  X(reset_kind, 0, 0, 3)       \
  X(reset_done, 0, 0, 1)       \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVBM_ERRORS_PARAMS);

#ifdef EG2_REVBM_ERRORS_INJECT_FAIL
#define INJECT 1u
#else
#define INJECT 0u
#endif

enum {
  kSlots = kEg2ArenaB,
  kNum = 8,
  kOps = kEg2WorkBase,
  kMaxOps = 32,
  kOut = kOps + 12 * kMaxOps,
  // The watchdog's bite, in AON clock cycles after it is enabled: well after the CLC has issued.
  kBite = 40,
  // revbm_err_end's answer (chip_sw_eg2_revbm_errors_vseq).
  kAnsHit = 1u << 0,
  kAnsRevoked = 1u << 1,
  kAnsTaggedLeak = 1u << 2,
  kAnsCoreAlert = 1u << 3,
  kAnsCmsAlert = 1u << 4,
  // revbm_hold_end's answer.
  kAnsHeld = 1u << 0,
  kAnsResetInHold = 1u << 1,
};

typedef enum error_case {
  kBusErr = 1,
  kIntg = 2,
  kMemErr = 3,
  kReset = 4,
} error_case_t;

static eg2_tally_t tally;
static bool gaps, bench;

static uint32_t obj_addr(uint32_t i) { return kEg2ArenaA + 256 * i; }
static uint32_t slot_addr(uint32_t i) { return kSlots + 8 * i; }
static uint32_t dev_addr(eg2_dev_id_t d) { return EG2_ADDR((void *)eg2_dev(d)); }

static const char *case_name(uint32_t c) {
  switch (c) {
    case kBusErr:
      return "bus_err";
    case kIntg:
      return "intg";
    case kMemErr:
      return "mem_err";
    default:
      return "reset";
  }
}

/** Stores the slots (bitmap clear) and checks each loads tagged. */
static bool fill(void) {
  for (uint32_t i = 0; i < kNum; ++i) {
    eg2_ms_csc(slot_addr(i), eg2_ms_object_cap(obj_addr(i), 8, false));
    if (!eg2_check(&tally, "slot tagged", i, eg2_ms_tag(slot_addr(i)), 1)) {
      return false;
    }
  }
  return true;
}

static void revbm_word_bit(uint32_t addr, uint32_t *word, uint32_t *bit) {
  *word = (addr - kEg2MsHeapBase) / 256;
  *bit = ((addr - kEg2MsHeapBase) / 8) % 32;
}

// ------------------------------------------ Cases 1-3 ------------------------------------------

static void error_case(error_case_t c, uint32_t field) {
  ++tally.planned;
  if (!fill()) {
    return;
  }
  uint32_t t = eg2_rand_range(1, kNum - 2);
  uint32_t n = (t + 1 + eg2_pick(kNum - 1)) % kNum;
  eg2_put_kv("EG2 revbm_errors slot", t);
  eg2_put_kv(" neighbour", n);
  eg2_put_kv(" field", field);
  eg2_puts("\r\n");

  // The alert the documents give: the bench checks it whether or not the core goes on.
  const char *alert_name =
      c == kMemErr ? "cheriot_mem_sys_fatal_fault" : "rv_core_ibex_fatal_hw_err";
  int alert = eg2_status_alert_find(alert_name);
  eg2_check(&tally, "alert known", 0, alert >= 0, 1);

  // Bench kinds: 0 bus_err, 1 intg, 2 mem_err.
  (void)eg2_ms_bench("revbm_err_arm", (uint32_t)(c - 1) | (field << 8), obj_addr(t), bench);

  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  uint32_t out_t = eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, slot_addr(t), 0);
  uint32_t out_n = eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, slot_addr(n), 0);
  eg2_rv_ops_run(&o, kOut);
  // Here only if the core still fetches after the alert.
  uint32_t tag_t = eg2_ms_read32(kOut + 4 * out_t);
  uint32_t tag_n = eg2_ms_read32(kOut + 4 * out_n);

  (void)eg2_ms_bench("revbm_err_end", 0, 0, bench);
  uint32_t ans = bench ? eg2_ms_bench_result() : 0;
  eg2_put_kv("EG2 revbm_errors tags: faulted", tag_t);
  eg2_put_kv(" neighbour", tag_n);
  eg2_put_kv(" bench", ans);
  eg2_puts("\r\n");

  eg2_check(&tally, "neighbour keeps its tag", n, tag_n, INJECT ? 0u : 1u);
  if (!gaps) {
    eg2_check(&tally, "injection took effect", t, ans & kAnsHit, kAnsHit);
    eg2_check(&tally, "failed check revoked (CGetTag)", t, tag_t, 0);
    eg2_check(&tally, "bench: revoked response delivered", t, ans & kAnsRevoked, kAnsRevoked);
    eg2_check(&tally, "bench: verdict response tagged", t, ans & kAnsTaggedLeak, 0);
    uint32_t want = c == kMemErr ? kAnsCmsAlert : kAnsCoreAlert;
    eg2_check(&tally, "bench: alert requested", t, ans & want, want);
    if (alert >= 0) {
      bool got = eg2_status_wait_alert((size_t)alert, 100000);
      eg2_check(&tally, "alert at the alert handler", t, got, 1);
      if (got) {
        eg2_status_expect_alert((size_t)alert);
      }
      // A corrupted meta SRAM word that reaches the core is also a bus integrity error there
      // (rv_core_ibex interfaces.md); declared if it came.
      int core = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
      if (c == kMemErr && core >= 0 && eg2_status_wait_alert((size_t)core, 2000)) {
        eg2_puts("EG2 revbm_errors: rv_core_ibex fatal_hw_err also raised\r\n");
        eg2_status_expect_alert((size_t)core);
      }
    }
  }
  ++tally.done;
}

// -------------------------------------------- Case 4 -------------------------------------------

/** The watchdog's arming, as window ops (no call between it and the CLC). */
static void op_watchdog(eg2_rv_ops_t *o) {
  uint32_t pwr = dev_addr(kEg2DevPwrmgr);
  uint32_t aon = dev_addr(kEg2DevAonTimer);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pwr + PWRMGR_RESET_EN_REG_OFFSET,
                  1u << PWRMGR_RESET_EN_EN_1_BIT);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pwr + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)eg2_rv_op(o, kEg2RvOpPollClear, kEg2RvAuthPwrmgr, pwr + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET,
                  0xffffffffu);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, kBite);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WDOG_CTRL_REG_OFFSET,
                  1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);
}

static eg2_ms_reset_kind_t reset_of(uint32_t kind) {
  return kind == 1 ? kEg2MsResetKindPor
                   : (kind == 2 ? kEg2MsResetKindNdm : kEg2MsResetKindWatchdog);
}

static void reset_boot_1(uint32_t kind) {
  ++tally.planned;
  if (!fill()) {
    return;
  }
  uint32_t t = eg2_rand_range(1, kNum - 2);
  uint32_t w, b;
  revbm_word_bit(obj_addr(t), &w, &b);
  eg2_put_kv("EG2 revbm_errors reset: slot", t);
  eg2_puts(" kind ");
  eg2_puts(eg2_ms_reset_name(reset_of(kind)));
  eg2_puts("\r\n");
  (void)eg2_ms_bench("revbm_hold_reset", obj_addr(t), kind, bench);
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  if (kind == 3) {
    op_watchdog(&o);
  }
  eg2_rv_op_revbm(&o, w, 1u << b);
  (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, slot_addr(t), 0);
  (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, 400000);
  // Only if no reset came: the mark cleared before any call.
  eg2_rv_op_revbm(&o, w, 0);
  eg2_rv_ops_run(&o, kOut);
  eg2_puts("EG2 revbm_errors: no reset came\r\n");
  eg2_check(&tally, "reset during the bitmap read", 0, 0, 1);
}

static void reset_boot_2(uint32_t kind) {
  ++tally.planned;
  // The reset reason first (the test ROM's copy in retention SRAM).
  uint32_t reason = eg2_ms_reset_reason();
  uint32_t exp = eg2_ms_reset_expected(reset_of(kind));
  eg2_check(&tally, "reset reason", kind, reason & exp, exp);
  // A watchdog that a system reset left running would bite again.
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  (void)eg2_ms_bench("revbm_hold_end", 0, 0, bench);
  uint32_t ans = bench ? eg2_ms_bench_result() : 0;
  eg2_put_kv("EG2 revbm_errors reset reason", reason);
  eg2_put_kv(" bench", ans);
  eg2_puts("\r\n");
  eg2_check(&tally, "bench: bitmap read held", kind, ans & kAnsHeld, kAnsHeld);
  eg2_check(&tally, "bench: reset while the read was outstanding", kind, ans & kAnsResetInHold,
            kAnsResetInHold);
  // REQ_CMS_15: no revocation bit after any reset (the mark of boot 1 included).
  uint32_t nz = 0;
  for (uint32_t i = 0; i < kNum; ++i) {
    uint32_t w, b;
    revbm_word_bit(obj_addr(i), &w, &b);
    nz |= eg2_ms_revbm_read(w);
  }
  eg2_check(&tally, "bitmap after the reset", kind, nz, 0);

  if (!eg2_sec_sram_init()) {
    eg2_check(&tally, "sec SRAM initialised", kind, 0, 1);
    return;
  }
  eg2_ms_revbm_clear();
  if (!fill()) {
    return;
  }
  uint32_t t = eg2_rand_range(1, kNum - 2);
  uint32_t n = (t + 1) % kNum;
  uint32_t w, b;
  revbm_word_bit(obj_addr(t), &w, &b);
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  uint32_t o_before = eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, slot_addr(t), 0);
  eg2_rv_op_revbm(&o, w, 1u << b);
  uint32_t o_marked = eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, slot_addr(t), 0);
  uint32_t o_n = eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, slot_addr(n), 0);
  eg2_rv_op_revbm(&o, w, 0);
  eg2_rv_ops_run(&o, kOut);
  eg2_check(&tally, "unmarked slot after the reset", t, eg2_ms_read32(kOut + 4 * o_before), 1);
  eg2_check(&tally, "marked slot after the reset", t, eg2_ms_read32(kOut + 4 * o_marked),
            INJECT ? 1u : 0u);
  eg2_check(&tally, "other slot after the reset", n, eg2_ms_read32(kOut + 4 * o_n), 1);
  ++tally.done;
}

bool eg2_test_main(void) {
  gaps = EG2_PARAM(model_gaps) != 0;
  bench = !gaps;
  uint32_t c = EG2_PARAM(error) != 0 ? EG2_PARAM(error) : eg2_rand_range(1, 4);
  if (gaps && c == kReset) {
    c = kBusErr;
  }
  uint32_t kind = EG2_PARAM(reset_kind) != 0 ? EG2_PARAM(reset_kind) : eg2_rand_range(1, 3);
  if (c == kReset && EG2_PARAM(reset_done) != 0) {
    eg2_puts("EG2 revbm_errors: boot after the reset\r\n");
    reset_boot_2(kind);
    eg2_tally_print("revbm_errors", &tally);
    return eg2_tally_ok(&tally);
  }
  if (!eg2_sec_sram_init()) {
    return false;
  }
  eg2_puts("EG2 revbm_errors case ");
  eg2_puts(case_name(c));
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  if (c == kReset) {
    reset_boot_1(kind);
  } else {
    uint32_t field = EG2_PARAM(field) != 0 ? EG2_PARAM(field) : eg2_rand_range(1, 3);
    error_case((error_case_t)c, field);
  }
  eg2_tally_print("revbm_errors", &tally);
  return eg2_tally_ok(&tally);
}
