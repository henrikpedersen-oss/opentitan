// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_corners (EG2 spec "Revocation", "Corner cases"; REQ_CMS_12, REQ_CMS_15).
 *
 * The TBRE at the edges of its use. Slots in sec SRAM's arena B, tagged capabilities each based
 * on an object of its own granule in arena A; about half the objects marked. Marks are set only
 * inside op windows (eg2_revoke.h).
 *
 * First boot:
 *  A. Contention: a sweep over the slots while the crossbar is slowed: the core reloads 64 other
 *     capabilities (arena B's upper half, unmarked) back to back, the bench reads sec SRAM over SBA
 *     and holds the SRAM ports' responses (`contention`). The window waits for tbre_done at the
 *     PLIC (the interrupt), clears the marks, and reloads every slot. Check: at the interrupt every
 *     marked slot is untagged in memory and every other one tagged; the core's reloads all tagged;
 *     TBRE_STATUS clean; the epoch +2.
 *  B. Ranges: a start with TBRE_NUM_CAPS 0 is ignored (start_err, busy stays 0, the epoch
 *     unchanged; registers.md TBRE_STATUS.start_err); a sweep of the whole of main and sec SRAM
 *     (with the slots marked again: the marked slots untagged after it), and of the whole of RRAM
 *     below the OTP pages (0x3000_0000 to 0x301F_F600; programmers_guide.md): each ends without an
 *     error and advances the epoch by 2.
 *  C. Then, for each reset of `resets` (1 software reset through rstmgr, 2 an escalation reset:
 *     rv_core_ibex's fatal_sw_err in class A, escalation signal 3), one window: the slots stored
 *     again, the marks set, a sweep of the whole of main and sec SRAM started, TBRE_STATUS
 *     recorded in retention SRAM, and the reset requested while the sweep runs.
 * The boot after each reset checks: the reset reason; the reset came during the sweep
 * (TBRE_STATUS.busy recorded); TBRE_EPOCH 0 and TBRE_STATUS 0 (their reset values: the epoch counts
 * completed sweeps only, and the interrupted one never completed).
 *
 * Not checked here (dv_todo.md):
 *  - "no stale tag after a reset" (CMS_15): the EG2 start-up code zeroes the meta SRAM on every boot
 *    (eg2_cheriot_start.S step 0), so in a framework test the tags and the bitmap are 0 after any
 *    reset by construction. CMS_15 proper is chip_sw_cheriot_meta_init_after_reset's (a late
 *    image that leaves the meta SRAM alone; expected to fail, rtl_tickets.md F11);
 *  - a mode change during a sweep: the mode is locked until reset (REQ_SOC_08); the only change is
 *    through a reset, which part C covers.
 *
 * `model_gaps` (eg2sim): no load filter, no revoking TBRE, no bench: the tags only revocation
 * makes 0 are not checked, the bench steps are skipped.
 *
 * Built with EG2_TBRE_CORNERS_INJECT_FAIL, the epoch after the contention sweep is expected +4:
 * the run must fail with `MISMATCH A: epoch`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top/rstmgr_regs.h"
#include "hw/top/rv_plic_regs.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TBRE_CORNERS_PARAMS(X) \
  X(contention, 4, 0, 4)       \
  X(resets, 3, 0, 3)           \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TBRE_CORNERS_PARAMS);

enum {
  kNum = 32,
  kObj = kEg2ArenaA + 0x400,      // 32 granules: one bitmap word
  kSlots = kEg2ArenaB,            // kNum slots
  kOther = kEg2ArenaB + 0x1000,   // 64 other capabilities (unmarked objects)
  kOtherObj = kEg2ArenaA + 0x800,
  kNumOther = 64,
  kOps = kEg2WorkBase,
  kMaxOps = 512,
  kOut = kOps + 12 * kMaxOps,
  kOtpPages = 0x301ff600u,
  kMuBi4True = 0x6,
  kBusy = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT,
  // The boot record's words.
  kRecMarks = 0,       // the marked objects' mask
  kRecResets = 1,      // the resets still to do (bit 0 software, bit 1 escalation)
  kRecKind = 2,        // the reset of the last window (1 software, 2 escalation)
  kRecStatus = 3,      // TBRE_STATUS when the reset was requested
  kRecWord = 4,        // the bitmap word of the objects
  kRecTally = 5,       // checks so far, failures so far
  kRecFailed = 6,
};

static eg2_tally_t tally;
static bool gaps, bench;

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }
static uint32_t dev_addr(eg2_dev_id_t d) { return EG2_ADDR((void *)eg2_dev(d)); }

static uint32_t obj_word(void) { return (kObj - kEg2MsHeapBase) / 256; }
static uint32_t obj_bit(uint32_t i) { return ((kObj + 8 * i - kEg2MsHeapBase) / 8) % 32; }
static uint32_t marks_value(uint32_t mask) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < kNum; ++i) {
    if ((mask >> i) & 1) {
      v |= 1u << obj_bit(i);
    }
  }
  return v;
}

static void fill(void) {
  for (uint32_t i = 0; i < kNum; ++i) {
    eg2_ms_csc(kSlots + 8 * i, eg2_ms_object_cap(kObj + 8 * i, 8, false));
  }
}

static void check_slots(const char *what, uint32_t idx, uint32_t mask, uint32_t out_first) {
  for (uint32_t i = 0; i < kNum; ++i) {
    bool m = (mask >> i) & 1;
    if (gaps && m) {
      continue;
    }
    uint32_t got = out_first != 0xffffffffu ? rd(kOut + 4 * (out_first + i))
                                            : eg2_ms_tag(kSlots + 8 * i);
    eg2_check(&tally, what, idx * kNum + i, got, m ? 0u : 1u);
  }
}

// ------------------------------------------- A -----------------------------------------------

static void part_a(uint32_t mask, uint32_t contention) {
  ++tally.planned;
  fill();
  for (uint32_t i = 0; i < kNumOther; ++i) {
    eg2_ms_csc(kOther + 8 * i, eg2_ms_object_cap(kOtherObj + 8 * (i % 32), 8, false));
  }
  eg2_ms_plic_enable(kEg2MsTbreIrq, true);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_ENABLE_TBRE_DONE_BIT);
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  eg2_rv_op_revbm(&o, obj_word(), marks_value(mask));
  eg2_rv_op_sweep(&o, kSlots, kNum);
  uint32_t core_first = o.outs;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t i = 0; i < kNumOther; ++i) {
      (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kOther + 8 * i, 0);
    }
  }
  uint32_t irq = eg2_rv_op(&o, kEg2RvOpPollSet, kEg2RvAuthPlic,
                           dev_addr(kEg2DevRvPlic) + RV_PLIC_IP_0_REG_OFFSET +
                               4 * (kEg2MsTbreIrq / 32),
                           1u << (kEg2MsTbreIrq % 32));
  eg2_rv_op_revbm(&o, obj_word(), 0);
  uint32_t first = o.outs;
  for (uint32_t i = 0; i < kNum; ++i) {
    (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlots + 8 * i, 0);
  }
  eg2_check(&tally, "A: window fits", 0, o.full, 0);

  uint32_t epoch0 = eg2_ms_tbre_epoch();
  if (contention & 1) {
    (void)eg2_rv_delays(true, bench);
  }
  if (contention & 2) {
    (void)eg2_rv_sba_noise(true, bench);
  }
  eg2_rv_ops_run(&o, kOut);
  if (contention & 2) {
    (void)eg2_rv_sba_noise(false, bench);
  }
  if (contention & 1) {
    (void)eg2_rv_delays(false, bench);
  }
  uint32_t id = eg2_ms_plic_claim();
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_STATE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_STATE_TBRE_DONE_BIT);
  eg2_ms_plic_complete(id);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET, 0);
  eg2_ms_plic_enable(kEg2MsTbreIrq, false);

  eg2_check(&tally, "A: tbre_done at the PLIC", 0, rd(kOut + 4 * irq) != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "A: PLIC claim", 0, id, kEg2MsTbreIrq);
  check_slots("A: tag at the interrupt", 0, mask, first);
  uint32_t core_tagged = 0;
  for (uint32_t k = 0; k < 4 * kNumOther; ++k) {
    core_tagged += rd(kOut + 4 * (core_first + k));
  }
  eg2_check(&tally, "A: the core's reloads tagged", 0, core_tagged, 4 * kNumOther);
  eg2_check(&tally, "A: TBRE_STATUS", 0, eg2_ms_tbre_status(), 0);
  uint32_t exp_epoch = epoch0 + 2;
#ifdef EG2_TBRE_CORNERS_INJECT_FAIL
  exp_epoch += 2;
#endif
  eg2_check(&tally, "A: epoch", 0, eg2_ms_tbre_epoch(), exp_epoch);
  eg2_ms_tbre_clear();
  ++tally.done;
}

// ------------------------------------------- B -----------------------------------------------

/** A whole-range sweep in one window (marks set, sweep, wait, marks cleared). */
static uint32_t sweep_window(uint32_t base, uint32_t n_caps, uint32_t mask) {
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  eg2_rv_op_revbm(&o, obj_word(), marks_value(mask));
  eg2_rv_op_sweep(&o, base, n_caps);
  uint32_t wait = eg2_rv_op_sweep_wait(&o);
  eg2_rv_op_revbm(&o, obj_word(), 0);
  eg2_rv_ops_run(&o, kOut);
  return rd(kOut + 4 * wait);
}

static void part_b(uint32_t mask) {
  ++tally.planned;
  // NUM_CAPS 0: ignored.
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kSlots, 0, &sw);
  eg2_check(&tally, "B: zero-length start ignored (start_err)", 0, sw.started, 0);
  eg2_check(&tally, "B: zero-length start: busy", 0, eg2_ms_tbre_status() & kBusy, 0);
  eg2_check(&tally, "B: zero-length start: epoch", 0, eg2_ms_tbre_epoch(), epoch0);
  eg2_ms_tbre_clear();

  // The whole of main and sec SRAM.
  fill();
  epoch0 = eg2_ms_tbre_epoch();
  uint32_t wait = sweep_window(kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8, mask);
  eg2_check(&tally, "B: whole SRAM sweep ended", 0, wait != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "B: whole SRAM sweep: TBRE_STATUS", 0, eg2_ms_tbre_status(), 0);
  eg2_check(&tally, "B: whole SRAM sweep: epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_ms_tbre_clear();
  check_slots("B: tag after the whole SRAM sweep", 1, mask, 0xffffffffu);

  // The whole of RRAM below the OTP pages.
  epoch0 = eg2_ms_tbre_epoch();
  wait = sweep_window(kEg2MsNvmBase, (kOtpPages - kEg2MsNvmBase) / 8, 0);
  eg2_check(&tally, "B: whole RRAM sweep ended", 0, wait != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "B: whole RRAM sweep: TBRE_STATUS", 0, eg2_ms_tbre_status(), 0);
  eg2_check(&tally, "B: whole RRAM sweep: epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_ms_tbre_clear();
  ++tally.done;
}

// ------------------------------------------- C -----------------------------------------------

/** Stores the slots, sets the marks, starts a sweep and requests the reset, in one window. */
static void reset_during_sweep(eg2_ms_rec_t *rec, uint32_t kind) {
  fill();
  rec->data[kRecKind] = kind;
  rec->data[kRecStatus] = 0;
  int alert = eg2_status_alert_find("rv_core_ibex_fatal_sw_err");
  if (kind == 2 && alert >= 0) {
    eg2_ms_escalation_arm((size_t)alert);
  }
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  eg2_rv_op_revbm(&o, obj_word(), marks_value(rec->data[kRecMarks]));
  eg2_rv_op_sweep(&o, kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8);
  (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, eg2_rand_range(0, 200));
  uint32_t st = eg2_rv_op(&o, kEg2RvOpLw, kEg2RvAuthRegs,
                          dev_addr(kEg2DevCheriotMemSysRegs) +
                              CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET,
                          0);
  if (kind == 1) {
    (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthRstmgr,
                    dev_addr(kEg2DevRstmgr) + RSTMGR_RESET_REQ_REG_OFFSET, kMuBi4True);
  } else {
    // rv_core_ibex ALERT_TEST: fatal_sw_err (bit 0), its write enable (bit 31) kept 1.
    (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthIbex, dev_addr(kEg2DevRvCoreIbexCfg),
                    (1u << 0) | (1u << 31));
  }
  (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, 200000);
  // Only if no reset came: the marks cleared before any call.
  eg2_rv_op_revbm(&o, obj_word(), 0);
  // TBRE_STATUS into the boot record (retention SRAM): the window's out words are there.
  (void)st;
  eg2_puts(kind == 1 ? "EG2 tbre_corners: software reset during a sweep\r\n"
                     : "EG2 tbre_corners: escalation reset during a sweep\r\n");
  eg2_rv_ops_run(&o, EG2_ADDR(&rec->data[kRecStatus]));
  eg2_puts("EG2 tbre_corners: no reset came\r\n");
}

static void after_reset(eg2_ms_rec_t *rec) {
  ++tally.planned;
  uint32_t kind = rec->data[kRecKind];
  uint32_t reason = eg2_ms_reset_reason();
  uint32_t exp_reason = kind == 1 ? kEg2MsResetSw : kEg2MsResetEscalation;
  eg2_check(&tally, "C: reset reason", kind, reason & exp_reason, exp_reason);
  eg2_check(&tally, "C: the reset came during the sweep (TBRE_STATUS.busy)", kind,
            rec->data[kRecStatus] & kBusy, kBusy);
  eg2_check(&tally, "C: TBRE_EPOCH after the reset", kind, eg2_ms_tbre_epoch(), 0);
  eg2_check(&tally, "C: TBRE_STATUS after the reset", kind, eg2_ms_tbre_status(), 0);
  ++tally.done;
}

bool eg2_test_main(void) {
  eg2_ms_rec_t *rec = eg2_ms_rec_boot();
  gaps = EG2_PARAM(model_gaps) != 0;
  bench = !gaps;
  if (rec->boot == 1) {
    if (!eg2_sec_sram_init()) {
      return false;
    }
    uint32_t mask = eg2_rand();
    uint32_t contention =
        EG2_PARAM(contention) == 4 ? eg2_rand_range(0, 3) : EG2_PARAM(contention);
    rec->data[kRecMarks] = mask;
    rec->data[kRecResets] = EG2_PARAM(resets);
    rec->data[kRecWord] = obj_word();
    eg2_put_kv("EG2 tbre_corners marks", mask);
    eg2_put_kv(" contention", contention);
    eg2_put_kv(" resets", EG2_PARAM(resets));
    eg2_puts("\r\n");
    eg2_ms_revbm_clear();
    eg2_ms_tbre_clear();
    part_a(mask, contention);
    part_b(mask);
  } else {
    if (!eg2_sec_sram_init()) {
      return false;
    }
    after_reset(rec);
  }
  rec->data[kRecTally] += tally.checks;
  rec->data[kRecFailed] += tally.failed;
  uint32_t resets = rec->data[kRecResets];
  if (resets != 0) {
    uint32_t kind = (resets & 1) ? 1 : 2;
    rec->data[kRecResets] = resets & ~kind;
    if (!gaps || kind == 1) {
      reset_during_sweep(rec, kind);
      return false;
    }
  }
  eg2_put_kv("EG2 tbre_corners all boots: checks", rec->data[kRecTally]);
  eg2_put_kv(" failed", rec->data[kRecFailed]);
  eg2_puts("\r\n");
  eg2_tally_print("tbre_corners", &tally);
  return eg2_tally_ok(&tally) && rec->data[kRecFailed] == 0;
}
