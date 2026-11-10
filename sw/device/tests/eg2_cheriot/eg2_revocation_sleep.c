// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation_sleep (EG2 spec "Revocation", "Corner cases"; REQ_CMS_15,
 * REQ_PBF_18, REQ_PER_05).
 *
 * Low-power entry with revocation state live: tagged capabilities in sec SRAM's arena B, a random
 * set of their objects marked (one bitmap word, drawn), and a sweep of the whole of main and sec
 * SRAM running. One op window (eg2_revoke.h) sets the marks, starts the sweep, arms the aon_timer
 * wake-up (20 AON ticks), arms pwrmgr (WAKEUP_EN the AON timer; CONTROL read-modify-write:
 * LOW_POWER_HINT, MAIN_PD_N 1 for normal sleep and 0 for deep sleep, the other fields as they
 * were; CFG_CDC_SYNC), records TBRE_STATUS and executes WFI. `sleeps` selects normal sleep (bit 0), then deep sleep (bit 1).
 *
 * The two exits, from pwrmgr's documents (hw/top_earlgrey/ip_autogen/pwrmgr/doc):
 *  - normal sleep keeps every power domain on ("All power domains are kept on for fast resume",
 *    theory_of_operation.md "Normal Sleep") and software "continues executing the instruction
 *    after the wait-for-interrupt invocation" (programmers_guide.md "Exiting from Normal Sleep";
 *    pwrmgr_fsm.sv FastPwrStateLowPowerPrep asserts the non-always-on domains' resets only as
 *    ~main_pd_ni; OpenTitan's pwrmgr_normal_sleep_all_wake_ups continues after the WFI). So
 *    cheriot_mem_sys, the meta SRAM and the core keep their state: REQ_PER_05's "a wake-up from
 *    normal sleep keeps the core's state and continues in CHERIoT mode with the tags intact". The
 *    core leaves the WFI on pwrmgr's wake-up interrupt (PLIC, mie.MEIE; mstatus.MIE 0: no trap);
 *  - deep sleep exits through a reset recorded as a low-power exit ("Exiting from Deep Sleep").
 *
 * Checks:
 *  - before sleeping, a capability stored to retention SRAM (CSC, into the boot record, which the
 *    wake-up boots keep) loads untagged (the retention SRAM never holds a tag, REQ_CMS_10); after
 *    each wake-up it still loads untagged, with both words as stored (retention);
 *  - each sleep is entered while the sweep runs (TBRE_STATUS.busy recorded);
 *  - after normal sleep, in the same window, before any mark is cleared: WAKE_INFO the aon timer
 *    alone; the bitmap word as written; the sweep goes on and finishes (TBRE_STATUS.busy clears);
 *    each of arena B's capabilities loads (CLC) with the tag the load filter and the sweep give
 *    it: tagged if its object is not marked, untagged if it is. Then: no reset (rstmgr RESET_INFO
 *    has no low-power exit, the boot's reset reason and boot count unchanged), the sweep counted
 *    (TBRE_EPOCH + 2), the mode (misa) still CHERIoT, a capability held on the stack across the
 *    sleep still tagged;
 *  - after deep sleep, the reset recorded as a low-power exit, after which TBRE_EPOCH and
 *    TBRE_STATUS hold their reset values (0): the interrupted sweep is not counted. "No tag and no
 *    bitmap bit" after it (CMS_15) is met by the EG2 start-up code's zeroing of the meta SRAM on
 *    every boot (eg2_cheriot_start.S step 0); CMS_15 proper is
 *    chip_sw_cheriot_meta_init_after_reset's.
 *
 * `model_gaps` (eg2sim): no load filter and no revoking TBRE, so the marked objects'
 * capabilities keep their tags there; their tag checks are skipped (named in the log), the
 * unmarked ones' are made.
 *
 * Built with EG2_REVOCATION_SLEEP_INJECT_FAIL, the retention SRAM capability is expected tagged
 * before the first sleep: the run must fail with `MISMATCH retention SRAM tag before sleeping`.
 *
 * EG2_NEEDS_BENCH: on the Verilator top no sleep entered by WFI came back within the hour
 * (regr-vlt 2026-10-26: TIMEOUT after `EG2 revocation_sleep sleeps`; see eg2_pbf_power_sb.c).
 */

#include "hw/top/aon_timer_regs.h"
#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top/pwrmgr_regs.h"
#include "hw/top/rstmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
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
  kOuts = kEg2WorkBase + 0x400,  // the normal sleep window's out words (after the ops)
  kBusy = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT,
  // The aon_timer's wake-up, by its generated index (it moved from 4 to 6 in the 2026-11-06 merge).
  kAonWake = 1u << kTopEarlgreyPowerManagerWakeUpsAonTimerWkupReq,
  kMipMeip = 1u << 11,
  kMisaE = 1u << 4,
  kMisaI = 1u << 8,
  // The boot record's words.
  kRecSleeps = 0,  // the sleeps still to do (bit 0 normal, bit 1 deep)
  kRecKind = 1,    // the last sleep (1 normal, 2 deep)
  kRecSync = 2,    // the deep window's first out word: the CDC sync's polls left
  kRecStatus = 3,  // its second: TBRE_STATUS at the sleep entry
  kRecRetHi = 4,   // the retention SRAM capability's words
  kRecRetAddr = 5,
  kRecChecks = 6,
  kRecFailed = 7,
  kRecCap = 9,     // words 9 and 10: the capability stored to retention SRAM (8-byte aligned)
  // The normal sleep window's out words (at kOuts).
  kOutSync = 0,    // the CDC sync's polls left
  kOutStatus = 1,  // TBRE_STATUS at the sleep entry
  kOutWake = 2,    // WAKE_INFO after the wake-up
  kOutRevbm = 3,   // the bitmap word after the wake-up
  kOutSweep = 4,   // the sweep's wait: polls left (0: it never finished)
  kOutTags = 5,    // kNum words: each slot's CLC tag
};

static eg2_tally_t tally;

// A line per set-up step (2026-10-27): the Verilator run of 2026-10-26 stopped after "EG2
// revocation_sleep sleeps" and before "normal sleep during a sweep", so in the set-up below and
// not in the sleep its EG2_NEEDS_BENCH note blames; these lines show which step.
static void step(const char *what) {
  eg2_puts("EG2 revocation_sleep step: ");
  eg2_puts(what);
  eg2_puts("\r\n");
}

static uint32_t dev_addr(eg2_dev_id_t d) { return EG2_ADDR((void *)eg2_dev(d)); }

static uint32_t ret_slot(eg2_ms_rec_t *rec) { return EG2_ADDR(&rec->data[kRecCap]); }

static void check_retention(const char *what, uint32_t idx, eg2_ms_rec_t *rec) {
  eg2_cap_t c = eg2_hw_read(eg2_ms_clc(ret_slot(rec)));
  uint32_t exp_tag = 0;
#ifdef EG2_REVOCATION_SLEEP_INJECT_FAIL
  if (idx == 0) {
    eg2_inject_armed(what);
    exp_tag = 1;
  }
#endif
  eg2_check(&tally, what, idx, c.tag ? 1u : 0u, exp_tag);
  eg2_check(&tally, "retention SRAM capability's high word", idx, c.hi, rec->data[kRecRetHi]);
  eg2_check(&tally, "retention SRAM capability's address", idx, c.addr, rec->data[kRecRetAddr]);
}

/** The aon_timer's wake-up source down again (its request follows WKUP_CAUSE). */
static void aon_wake_clear(void) {
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET,
                  1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
}

/** CONTROL without LOW_POWER_HINT and MAIN_PD_N: the other fields (USB_CLK_EN_ACTIVE, reset value
 * 1, keeps the USB clock on in the active state) as they are. */
static uint32_t control_base(void) {
  return eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_CONTROL_REG_OFFSET) &
         ~((1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT) | (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT));
}

/**
 * One window: the marks, the sweep, the low-power entry, WFI. `kind` 1 normal sleep, 2 deep
 * sleep. Returns the bitmap word index and the mask it marked. For a normal sleep the window goes
 * on after the WFI (the wake-up, the bitmap word, the sweep's end, the slots' tags into `out`)
 * and clears the marks; for a deep sleep it returns only if the WFI returned.
 */
static void sleep_window(uint32_t kind, uint32_t out, uint32_t *word, uint32_t *mask_out) {
  uint32_t mask = eg2_rand();
  for (uint32_t i = 0; i < kNum; ++i) {
    eg2_ms_csc(kSlots + 8 * i, eg2_ms_object_cap(kObj + 8 * i, 8, false));
  }
  step("capabilities stored to arena B");
  uint32_t w, b0;
  (void)eg2_ms_revbm_bit(kObj, &w, &b0);
  *word = w;
  *mask_out = mask;
  uint32_t pm = dev_addr(kEg2DevPwrmgr), aon = dev_addr(kEg2DevAonTimer);
  uint32_t control = control_base();
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  eg2_rv_op_revbm(&o, w, mask);
  eg2_rv_op_sweep(&o, kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 20);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pm + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonWake);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pm + PWRMGR_CONTROL_REG_OFFSET,
                  control | (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT) |
                      (kind == 1 ? 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT : 0));
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthPwrmgr, pm + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)eg2_rv_op(&o, kEg2RvOpPollClear, kEg2RvAuthPwrmgr, pm + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)eg2_rv_op(&o, kEg2RvOpLw, kEg2RvAuthRegs,
                  dev_addr(kEg2DevCheriotMemSysRegs) + CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET, 0);
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthAon, aon + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                  1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  (void)eg2_rv_op(&o, kEg2RvOpWfi, kEg2RvAuthHeap, 0, 0);
  if (kind == 1) {
    // Normal sleep: here after the wake-up, the marks still set.
    (void)eg2_rv_op(&o, kEg2RvOpLw, kEg2RvAuthPwrmgr, pm + PWRMGR_WAKE_INFO_REG_OFFSET, 0);
    (void)eg2_rv_op(&o, kEg2RvOpLw, kEg2RvAuthRevbm, kEg2MsMetaBase + 4 * w, 0);
    (void)eg2_rv_op_sweep_wait(&o);
    for (uint32_t i = 0; i < kNum; ++i) {
      (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlots + 8 * i, 0);
    }
  } else {
    (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, 200000);
  }
  // The marks cleared before any call.
  eg2_rv_op_revbm(&o, w, 0);
  eg2_puts(kind == 1 ? "EG2 revocation_sleep: normal sleep during a sweep\r\n"
                     : "EG2 revocation_sleep: deep sleep during a sweep\r\n");
  // Out of the UART before the low-power entry (2026-10-27: on Verilator this line never appeared,
  // still in the TX FIFO when the chip slept, so the run looked hung in the set-up before it).
  eg2_uart_drain();
  if (o.full) {
    eg2_check(&tally, "the window's ops fit", kind, 0, 1);
    return;
  }
  eg2_rv_ops_run(&o, out);
}

/** Normal sleep: the window, then the checks, in this boot. */
static void normal_sleep(eg2_ms_rec_t *rec) {
  ++tally.planned;
  rec->data[kRecKind] = 1;
  uint32_t reason = eg2_ms_reset_reason();
  uint32_t boot = rec->boot;
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  // A capability held on the stack across the sleep.
  void *volatile keep = (void *)rec;
  // pwrmgr's wake-up interrupt ends the WFI: PLIC and mie.MEIE, mstatus.MIE 0 (no trap).
  __asm__ volatile("csrci mstatus, 8");
  eg2_ms_plic_enable(kTopEarlgreyPlicIrqIdPwrmgrWakeup, true);
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_INTR_ENABLE_REG_OFFSET,
                  1u << PWRMGR_INTR_ENABLE_WAKEUP_BIT);
  __asm__ volatile("csrs mie, %0" : : "r"(kMipMeip));
  uint32_t w, mask;
  sleep_window(1, kOuts, &w, &mask);
  // Here after the wake-up, in the same boot; every mark clear again.
  __asm__ volatile("csrc mie, %0" : : "r"(kMipMeip));
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
  uint32_t claimed = eg2_ms_plic_claim();
  if (claimed != 0) {
    eg2_ms_plic_complete(claimed);
  }
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_INTR_ENABLE_REG_OFFSET, 0);
  eg2_ms_plic_enable(kTopEarlgreyPlicIrqIdPwrmgrWakeup, false);
  eg2_puts("EG2 revocation_sleep: continued after the normal sleep's WFI\r\n");
  const uint32_t k = 1;
  eg2_check(&tally, "normal sleep: WAKE_INFO the aon timer alone", k,
            eg2_ms_read32(kOuts + 4 * kOutWake), kAonWake);
  eg2_check(&tally, "the WFI ended by pwrmgr's wake-up interrupt (PLIC claim)", k, claimed,
            kTopEarlgreyPlicIrqIdPwrmgrWakeup);
  eg2_check(&tally, "sleep entered during the sweep (TBRE_STATUS.busy)", k,
            eg2_ms_read32(kOuts + 4 * kOutStatus) & kBusy, kBusy);
  eg2_check(&tally, "rstmgr RESET_INFO: no low-power exit reset", k,
            eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET) &
                (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT),
            0);
  eg2_check(&tally, "the boot's reset reason unchanged", k, eg2_ms_reset_reason(), reason);
  eg2_check(&tally, "the boot count unchanged", k, rec->boot, boot);
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  eg2_check(&tally, "mode after the wake-up (misa): CHERIoT", k, misa & (kMisaE | kMisaI), kMisaE);
  eg2_check(&tally, "a capability on the stack: tag after the sleep", k,
            __builtin_cheri_tag_get(keep) ? 1u : 0u, 1);
  eg2_check(&tally, "the bitmap word after the wake-up (as written)", k,
            eg2_ms_read32(kOuts + 4 * kOutRevbm), mask);
  eg2_check(&tally, "the sweep finished after the wake-up", k,
            eg2_ms_read32(kOuts + 4 * kOutSweep) != 0, 1);
  eg2_check(&tally, "TBRE_EPOCH: the sweep counted", k, eg2_ms_tbre_epoch(), epoch0 + 2);
  uint32_t b0;
  (void)eg2_ms_revbm_bit(kObj, &w, &b0);
  eg2_put_kv("EG2 revocation_sleep normal sleep: bitmap word", w);
  eg2_put_kv(" marks", mask);
  eg2_puts("\r\n");
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t skipped = 0;
  for (uint32_t i = 0; i < kNum; ++i) {
    bool marked = b0 + i < 32 && ((mask >> (b0 + i)) & 1);
    if (marked && gaps) {
      ++skipped;
      continue;
    }
    eg2_check(&tally, marked ? "slot tag after the sleep (object marked: revoked)"
                             : "slot tag after the sleep (object not marked: kept)",
              i, eg2_ms_read32(kOuts + 4 * (kOutTags + i)), marked ? 0 : 1);
  }
  if (skipped != 0) {
    eg2_put_kv("EG2 revocation_sleep model_gaps: marked slots' tags not checked (no load filter, "
               "no revoking TBRE)",
               skipped);
    eg2_puts("\r\n");
  }
  check_retention("retention SRAM tag after the normal sleep", k, rec);
  // The wake-up source down, CONTROL back without the hint, WAKE_INFO and the TBRE cleared.
  aon_wake_clear();
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_CONTROL_REG_OFFSET,
                  control_base() | (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT));
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
  eg2_ms_tbre_clear();
  ++tally.done;
}

/** The boot after the deep sleep's low-power exit reset. */
static void after_deep_wake(eg2_ms_rec_t *rec) {
  ++tally.planned;
  const uint32_t k = 2;
  eg2_check(&tally, "deep sleep: the kind of the last sleep", k, rec->data[kRecKind], 2);
  eg2_check(&tally, "reset reason: low-power exit", k,
            eg2_ms_reset_reason() & kEg2MsResetLowPowerExit, kEg2MsResetLowPowerExit);
  eg2_check(&tally, "sleep entered during the sweep (TBRE_STATUS.busy)", k,
            rec->data[kRecStatus] & kBusy, kBusy);
  eg2_check(&tally, "TBRE_EPOCH after the wake-up", k, eg2_ms_tbre_epoch(), 0);
  eg2_check(&tally, "TBRE_STATUS after the wake-up", k, eg2_ms_tbre_status(), 0);
  check_retention("retention SRAM tag after the wake-up", k, rec);
  aon_wake_clear();
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
    step("bitmap cleared");
    eg2_ms_tbre_clear();
    step("TBRE cleared");
    // A tagged capability stored to retention SRAM: it loads untagged.
    eg2_ms_csc(ret_slot(rec), eg2_ms_object_cap(kObj, 8, false));
    step("capability stored to retention SRAM");
    eg2_cap_t c = eg2_hw_read(eg2_ms_clc(ret_slot(rec)));
    step("capability loaded from retention SRAM");
    rec->data[kRecRetHi] = c.hi;
    rec->data[kRecRetAddr] = c.addr;
    check_retention("retention SRAM tag before sleeping", 0, rec);
    ++tally.done;
  } else {
    // Only a deep sleep resets the chip.
    after_deep_wake(rec);
  }
  if (rec->data[kRecSleeps] & 1) {
    rec->data[kRecSleeps] &= ~1u;
    normal_sleep(rec);
  }
  rec->data[kRecChecks] += tally.checks;
  rec->data[kRecFailed] += tally.failed;
  if (rec->data[kRecSleeps] & 2) {
    rec->data[kRecSleeps] &= ~2u;
    rec->data[kRecKind] = 2;
    rec->data[kRecStatus] = 0;
    uint32_t w, mask;
    // The window's out words: [0] the CDC sync's polls left, [1] TBRE_STATUS; into the record.
    sleep_window(2, EG2_ADDR(&rec->data[kRecSync]), &w, &mask);
    eg2_puts("EG2 revocation_sleep: the WFI returned (no deep sleep)\r\n");
    return false;
  }
  eg2_put_kv("EG2 revocation_sleep all boots: checks", rec->data[kRecChecks]);
  eg2_put_kv(" failed", rec->data[kRecFailed]);
  eg2_puts("\r\n");
  eg2_tally_print("revocation_sleep", &tally);
  return eg2_tally_ok(&tally) && rec->data[kRecFailed] == 0;
}
