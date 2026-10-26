// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation_sleep (EG2 spec "Revocation", "Corner cases"; REQ_CMS_15,
 * REQ_PBF_18).
 *
 * Low-power entry with revocation state live: tagged capabilities in sec SRAM's arena B, about half
 * their objects marked, and a sweep of the whole of main and sec SRAM running. One op window
 * (eg2_revoke.h) sets the marks, starts the sweep, arms the aon_timer wake-up (20 AON ticks), arms
 * pwrmgr (WAKEUP_EN the AON timer, CONTROL.LOW_POWER_HINT, MAIN_PD_N 1 for normal sleep and 0 for
 * deep sleep, CFG_CDC_SYNC), records TBRE_STATUS in retention SRAM and executes WFI; the marks are
 * cleared by the same window only if the WFI returns. `sleeps` selects normal sleep (bit 0), then
 * deep sleep (bit 1), one per boot.
 *
 * Checks:
 *  - before sleeping, a capability stored to retention SRAM (CSC, into the boot record, which the
 *    wake-up boots keep) loads untagged (the retention SRAM never holds a tag, REQ_CMS_10); after
 *    each wake-up it still loads untagged, with both words as stored (retention);
 *  - each sleep is entered while the sweep runs (TBRE_STATUS.busy recorded) and the WFI does not
 *    return;
 *  - the wake-up is a reset recorded as a low-power exit (pwrmgr programmer's guide, "Exiting from
 *    Normal Sleep" and "Exiting from Deep Sleep"), after which TBRE_EPOCH and TBRE_STATUS hold
 *    their reset values (0): the interrupted sweep is not counted.
 *
 * Not checked here (dv_todo.md, spec question):
 *  - "after normal sleep: the tags, the bitmap and the sweep as before": pwrmgr's programmer's guide
 *    exits normal sleep through a reset, which resets cheriot_mem_sys's registers (the sweep is
 *    gone), and the EG2 start-up code zeroes the meta SRAM on every boot (eg2_cheriot_start.S step
 *    0), so no framework test can see tags or marks across it; the same disagreement the PBF area
 *    records for REQ_PER_05;
 *  - "after deep sleep: no tag and no bitmap bit" (CMS_15): met by that start-up zeroing by
 *    construction; CMS_15 proper is chip_sw_cheriot_meta_init_after_reset's.
 *
 * `model_gaps` (eg2sim): no load filter and no revoking TBRE; nothing here depends on them, so the
 * knob only exists for the runner.
 *
 * Built with EG2_REVOCATION_SLEEP_INJECT_FAIL, the retention SRAM capability is expected tagged
 * before the first sleep: the run must fail with `MISMATCH retention SRAM tag before sleeping`.
 */

#include "hw/top/aon_timer_regs.h"
#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top/pwrmgr_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVOCATION_SLEEP_PARAMS(X) \
  X(sleeps, 3, 1, 3)               \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVOCATION_SLEEP_PARAMS);

enum {
  kNum = 32,
  kObj = kEg2ArenaA + 0x400,  // one bitmap word
  kSlots = kEg2ArenaB,
  kOps = kEg2WorkBase,
  kMaxOps = 64,
  kBusy = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT,
  // The boot record's words.
  kRecSleeps = 0,  // the sleeps still to do (bit 0 normal, bit 1 deep)
  kRecKind = 1,    // the last sleep (1 normal, 2 deep)
  kRecSync = 2,    // the window's first out word: the CDC sync's polls left
  kRecStatus = 3,  // its second: TBRE_STATUS at the sleep entry
  kRecRetHi = 4,   // the retention SRAM capability's words
  kRecRetAddr = 5,
  kRecChecks = 6,
  kRecFailed = 7,
  kRecCap = 9,     // words 9 and 10: the capability stored to retention SRAM (8-byte aligned)
};

static eg2_tally_t tally;

static uint32_t dev_addr(eg2_dev_id_t d) { return EG2_ADDR((void *)eg2_dev(d)); }

static uint32_t ret_slot(eg2_ms_rec_t *rec) { return EG2_ADDR(&rec->data[kRecCap]); }

static void check_retention(const char *what, uint32_t idx, eg2_ms_rec_t *rec) {
  eg2_cap_t c = eg2_hw_read(eg2_ms_clc(ret_slot(rec)));
  uint32_t exp_tag = 0;
#ifdef EG2_REVOCATION_SLEEP_INJECT_FAIL
  if (idx == 0) {
    exp_tag = 1;
  }
#endif
  eg2_check(&tally, what, idx, c.tag ? 1u : 0u, exp_tag);
  eg2_check(&tally, "retention SRAM capability's high word", idx, c.hi, rec->data[kRecRetHi]);
  eg2_check(&tally, "retention SRAM capability's address", idx, c.addr, rec->data[kRecRetAddr]);
}

/** One window: the marks, the sweep, the low-power entry, WFI. Returns only if no sleep came. */
static void sleep_window(eg2_ms_rec_t *rec, uint32_t kind) {
  uint32_t mask = eg2_rand();
  for (uint32_t i = 0; i < kNum; ++i) {
    eg2_ms_csc(kSlots + 8 * i, eg2_ms_object_cap(kObj + 8 * i, 8, false));
  }
  uint32_t w, b0;
  (void)eg2_ms_revbm_bit(kObj, &w, &b0);
  uint32_t pm = dev_addr(kEg2DevPwrmgr), aon = dev_addr(kEg2DevAonTimer);
  rec->data[kRecKind] = kind;
  rec->data[kRecStatus] = 0;
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  eg2_rv_op_revbm(&o, w, mask);
  eg2_rv_op_sweep(&o, kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 20);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pm + PWRMGR_WAKEUP_EN_REG_OFFSET,
                  1u << PWRMGR_WAKEUP_EN_EN_4_BIT);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pm + PWRMGR_CONTROL_REG_OFFSET,
                  (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT) |
                      (kind == 1 ? 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT : 0));
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pm + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)eg2_rv_op(&o, kEg2RvOpPollClear, kEg2RvAuthPwrmgr, pm + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)eg2_rv_op(&o, kEg2RvOpLw, kEg2RvAuthRegs,
                  dev_addr(kEg2DevCheriotMemSysRegs) + CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                  1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  (void)eg2_rv_op(&o, kEg2RvOpWfi, kEg2RvAuthHeap, 0, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, 200000);
  // Only if the WFI returned: the marks cleared before any call.
  eg2_rv_op_revbm(&o, w, 0);
  eg2_puts(kind == 1 ? "EG2 revocation_sleep: normal sleep during a sweep\r\n"
                     : "EG2 revocation_sleep: deep sleep during a sweep\r\n");
  // The window's out words: [0] the CDC sync's polls left, [1] TBRE_STATUS; into the record.
  eg2_rv_ops_run(&o, EG2_ADDR(&rec->data[kRecSync]));
  eg2_puts("EG2 revocation_sleep: the WFI returned (no sleep)\r\n");
}

static void after_wake(eg2_ms_rec_t *rec) {
  ++tally.planned;
  uint32_t kind = rec->data[kRecKind];
  eg2_check(&tally, "reset reason: low-power exit", kind,
            eg2_ms_reset_reason() & kEg2MsResetLowPowerExit, kEg2MsResetLowPowerExit);
  eg2_check(&tally, "sleep entered during the sweep (TBRE_STATUS.busy)", kind,
            rec->data[kRecStatus] & kBusy, kBusy);
  eg2_check(&tally, "TBRE_EPOCH after the wake-up", kind, eg2_ms_tbre_epoch(), 0);
  eg2_check(&tally, "TBRE_STATUS after the wake-up", kind, eg2_ms_tbre_status(), 0);
  check_retention("retention SRAM tag after the wake-up", kind, rec);
  // The wake-up source down again.
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET,
                  1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  ++tally.done;
}

bool eg2_test_main(void) {
  eg2_ms_rec_t *rec = eg2_ms_rec_boot();
  if (!eg2_sec_sram_init()) {
    return false;
  }
  if (rec->boot == 1) {
    ++tally.planned;
    rec->data[kRecSleeps] = EG2_PARAM(sleeps);
    eg2_put_kv("EG2 revocation_sleep sleeps", EG2_PARAM(sleeps));
    eg2_puts("\r\n");
    eg2_ms_revbm_clear();
    eg2_ms_tbre_clear();
    // A tagged capability stored to retention SRAM: it loads untagged.
    eg2_ms_csc(ret_slot(rec), eg2_ms_object_cap(kObj, 8, false));
    eg2_cap_t c = eg2_hw_read(eg2_ms_clc(ret_slot(rec)));
    rec->data[kRecRetHi] = c.hi;
    rec->data[kRecRetAddr] = c.addr;
    check_retention("retention SRAM tag before sleeping", 0, rec);
    ++tally.done;
  } else {
    after_wake(rec);
  }
  rec->data[kRecChecks] += tally.checks;
  rec->data[kRecFailed] += tally.failed;
  uint32_t sleeps = rec->data[kRecSleeps];
  if (sleeps != 0) {
    uint32_t kind = (sleeps & 1) ? 1 : 2;
    rec->data[kRecSleeps] = sleeps & ~kind;
    sleep_window(rec, kind);
    return false;
  }
  eg2_put_kv("EG2 revocation_sleep all boots: checks", rec->data[kRecChecks]);
  eg2_put_kv(" failed", rec->data[kRecFailed]);
  eg2_puts("\r\n");
  eg2_tally_print("revocation_sleep", &tally);
  return eg2_tally_ok(&tally) && rec->data[kRecFailed] == 0;
}
