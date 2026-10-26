// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation_irq (EG2 spec "Revocation", "Corner cases"; REQ_CIB_05, REQ_CMS_11,
 * REQ_CMS_12; ports the RTOS test `revocation_preempt`).
 *
 * Revocation under interrupts, with a handler of the test's own (eg2_revocation_irq.S, installed
 * in MTCC) that works as a context switch: it spills every capability register to a save area in
 * sec SRAM (outside every marked granule) and reloads them with CLC.
 *
 *  1. Preemption (`n_rounds`): a thread (eg2_rvirq_thread) holds five capabilities in cs0, cs1,
 *     ca2, ca3 and ca5, each based on an object of its own bitmap word in arena A, while rv_timer
 *     fires every `period` cycles (random 150-2000). On one timer interrupt (1-3) the handler
 *     marks the victim register's object (the thread is preempted while its object is freed), and
 *     in a third of the rounds clears the mark again before its reload; in another third nothing
 *     is marked. Check: the victim comes back untagged exactly when its granule was marked at the
 *     reload, and stays so; the other four keep their tags; every address is unchanged; the
 *     handler's own spills (csp, cgp, cra and the others) are unaffected: the thread runs on and
 *     returns. The thread clears the mark before any call.
 *  2. The revocation sequence under the timer: race part B's window (16 slots, about half their
 *     objects marked, a sweep over the slots started, the slots reloaded back to back for 4 rounds,
 *     the sweep waited for, the marks cleared) run with the periodic timer interrupting it. Check:
 *     every reload's tag as the bitmap gives, the swept slots' tags after the sweep, the sweep
 *     clean with the epoch +2, and at least one timer interrupt taken during the window.
 *  3. tbre_done with a timer interrupt pending: with mstatus.MIE clear, rv_timer's interrupt and a
 *     short sweep's tbre_done (PLIC source 189) both become pending; then MIE is set. Check: both
 *     taken once, the external interrupt first and the timer second (the Ibex manual, "Interrupts":
 *     the external interrupt has priority over the timer interrupt; RISC-V privileged
 *     specification 3.1.9), and the PLIC claim returns source 189.
 *  4. An NMI during a sweep: a sweep over the whole of sec SRAM, and the watchdog's bark (aon_timer,
 *     `nmi_bark` ticks, rv_core_ibex NMI_ENABLE.wdog) during it. Check: one NMI, taken through MTCC
 *     (the test's handler) with mcause 0x8000001F (REQ_CIB_05), while TBRE_STATUS.busy was set; the
 *     sweep then ends without an error and the epoch advances by 2.
 *
 * Not done here (dv_todo.md): the spec row's "every revocation test above rerun with rv_timer
 * firing": part 2 runs one revocation sequence under the timer; the other corner-case tests have
 * no timer knob yet.
 *
 * `model_gaps` (eg2sim): MPACT has no load filter, no revoking TBRE and no NMI (eg2sim: "NMI in
 * CHERIoT mode: not modelled"): the tags only revocation makes 0 are not checked, part 2's
 * interrupt count is not checked (the model's timer is too slow for the window), and part 4 is
 * skipped. Parts 1 and 3 run in the model: its timer interrupts enter the handler.
 *
 * Built with EG2_REVOCATION_IRQ_INJECT_FAIL, part 2 expects its first unmarked slot to reload
 * untagged: the run must fail with `MISMATCH 2: tag during the sweep #<n>`.
 */

#include "hw/top/aon_timer_regs.h"
#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top/rv_plic_regs.h"
#include "hw/top/rv_timer_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVOCATION_IRQ_PARAMS(X) \
  X(n_rounds, 0, 0, 64)          \
  X(period, 0, 0, 100000)        \
  X(nmi_bark, 0, 0, 1000)        \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVOCATION_IRQ_PARAMS);

// eg2_revocation_irq.S.
extern char eg2_rvirq_vector[];
extern void eg2_rvirq_thread(void *slots, uint32_t *out, void *ctx, uint32_t want,
                             uint32_t limit);

enum {
  // The context (eg2_revocation_irq.S C_*).
  kCtx = kEg2WorkBase + 0x9000,
  kCTCmp = 128,
  kCTIe = 136,
  kCTIs = 144,
  kCPCc = 152,
  kCMIs = 160,
  kCMSt = 168,
  kCACtrl = 176,
  kCAIs = 184,
  kCINmi = 192,
  kCMark = 200,
  kCPeriod = 208,
  kCCmp = 212,
  kCNTimer = 216,
  kCNExt = 220,
  kCNNmi = 224,
  kCNOther = 228,
  kCMarkAt = 232,
  kCMarkVal = 236,
  kCUnmark = 240,
  kCMarked = 244,
  kCPeriodic = 248,
  kCClaimed = 252,
  kCStAtNmi = 256,
  kCNmiCause = 260,
  kCOrderN = 264,
  kCOrder = 268,
  kCSize = 512,
  // Part 1: five slots and their objects (one bitmap word each), the thread's out words.
  kRegs = 5,
  kSlots1 = kEg2ArenaB,
  kObj1 = kEg2ArenaA,
  kOut1 = kCtx + kCSize,
  // Part 2: 16 slots and objects (race part B's layout).
  kSlots2 = kEg2ArenaB + 0x100,
  kObj2 = kEg2ArenaA + 0x800,
  kNum2 = 16,
  kRounds2 = 4,
  kOps = kEg2WorkBase,
  kMaxOps = 256,
  kOut2 = kOps + 12 * kMaxOps,
  // Part 3 and 4: the sweeps.
  kSlots3 = kEg2ArenaB + 0x400,
  kMipMtip = 1u << 7,
  kMipMeip = 1u << 11,
  kMstatusMie = 1u << 3,
  kMcauseTimer = 0x80000007u,
  kMcauseExt = 0x8000000bu,
  kMcauseNmi = 0x8000001fu,
  kTbreBusy = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT,
};

static eg2_tally_t tally;
static bool gaps;

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }
static void wr(uint32_t a, uint32_t v) { eg2_ms_write32(a, v); }
static uint32_t ctx_rd(uint32_t off) { return rd(kCtx + off); }
static void ctx_wr(uint32_t off, uint32_t v) { wr(kCtx + off, v); }

static void *reg(eg2_dev_id_t dev, uint32_t off) { return (void *)(eg2_dev(dev) + off / 4); }

static void ctx_init(void) {
  for (uint32_t off = 0; off < kCSize; off += 4) {
    wr(kCtx + off, 0);
  }
  eg2_ms_csc(kCtx + kCTCmp, reg(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET));
  eg2_ms_csc(kCtx + kCTIe, reg(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET));
  eg2_ms_csc(kCtx + kCTIs, reg(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET));
  eg2_ms_csc(kCtx + kCPCc, reg(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET));
  eg2_ms_csc(kCtx + kCMIs, reg(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_STATE_REG_OFFSET));
  eg2_ms_csc(kCtx + kCMSt, reg(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET));
  eg2_ms_csc(kCtx + kCACtrl, reg(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET));
  eg2_ms_csc(kCtx + kCAIs, reg(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET));
  eg2_ms_csc(kCtx + kCINmi, reg(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_STATE_REG_OFFSET));
  // The mark word: a harmless default (bitmap word 0 is never written by the handler unless a
  // round sets mark_at).
  eg2_ms_csc(kCtx + kCMark, eg2_mem(kEg2MsMetaBase, 4));
}

static void *saved_tcc, *saved_scratch;

static void handler_install(void) {
  void *tcc, *scratch;
  __asm__ volatile("cspecialr %0, mtcc" : "=C"(tcc));
  __asm__ volatile("cspecialr %0, mscratchc" : "=C"(scratch));
  saved_tcc = tcc;
  saved_scratch = scratch;
  void *ctx = eg2_mem(kCtx, kCSize);
  __asm__ volatile("cspecialw mscratchc, %0" : : "C"(ctx) : "memory");
  void *h = __builtin_cheri_address_set(tcc, __builtin_cheri_address_get(eg2_rvirq_vector));
  __asm__ volatile("cspecialw mtcc, %0" : : "C"(h) : "memory");
}

static void handler_remove(void) {
  void *tcc = saved_tcc, *scratch = saved_scratch;
  __asm__ volatile("cspecialw mtcc, %0" : : "C"(tcc) : "memory");
  __asm__ volatile("cspecialw mscratchc, %0" : : "C"(scratch) : "memory");
}

/** rv_timer: counting from 0, first interrupt after `first` cycles; periodic or one-shot. */
static void timer_start(uint32_t first, uint32_t period, bool periodic) {
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CFG0_REG_OFFSET, 1u << RV_TIMER_CFG0_STEP_OFFSET);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, first);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
  ctx_wr(kCPeriod, period);
  ctx_wr(kCCmp, first);
  ctx_wr(kCPeriodic, periodic);
  __asm__ volatile("csrs mie, %0" : : "r"(kMipMtip));
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 1);
}

// mstatus.MIE is set and cleared only inline in the function whose code must run with it: a
// CHERIoT return (cret through the backward sentry the call made) restores the interrupt state
// the call was made with, so a callee's change of MIE is undone when it returns.
static void timer_stop(void) {
  __asm__ volatile("csrc mie, %0" : : "r"(kMipMtip));
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
}

static uint32_t draw_period(void) {
  return EG2_PARAM(period) != 0 ? EG2_PARAM(period) : eg2_rand_range(150, 2000);
}

// ------------------------------------------- 1 -----------------------------------------------

static void part1(uint32_t round) {
  ++tally.planned;
  uint32_t obj[kRegs];
  for (uint32_t i = 0; i < kRegs; ++i) {
    obj[i] = kObj1 + 256 * i + 8 * eg2_pick(32);
    eg2_ms_csc(kSlots1 + 8 * i, eg2_ms_object_cap(obj[i], 8, false));
    if (!eg2_check(&tally, "1: slot tagged", round * kRegs + i, eg2_ms_tag(kSlots1 + 8 * i), 1)) {
      return;
    }
  }
  uint32_t victim = eg2_pick(kRegs), mode = eg2_pick(3);  // 0 none, 1 mark, 2 mark and clear
  uint32_t mark_at = eg2_rand_range(1, 3), period = draw_period();
  uint32_t w, b;
  (void)eg2_ms_revbm_bit(obj[victim], &w, &b);
  eg2_ms_csc(kCtx + kCMark, eg2_mem(kEg2MsMetaBase + 4 * w, 4));
  ctx_wr(kCMarkAt, mode != 0 ? mark_at : 0);
  ctx_wr(kCMarkVal, 1u << b);
  ctx_wr(kCUnmark, mode == 2);
  ctx_wr(kCMarked, 0);
  ctx_wr(kCNTimer, 0);
  uint32_t want = mark_at + eg2_rand_range(1, 2);
  timer_start(period, period, true);
  eg2_rvirq_thread(eg2_mem(kSlots1, 8 * kRegs), eg2_mem(kOut1, 44), eg2_mem(kCtx, kCSize), want,
                   4000000);
  timer_stop();
  // The thread cleared the bitmap word; every check from here is on stored words.
  uint32_t timers = ctx_rd(kCNTimer);
  eg2_check(&tally, "1: thread saw its interrupts", round, rd(kOut1 + 40) != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "1: victim's object marked by the handler", round, ctx_rd(kCMarked),
            mode != 0 ? 1u : 0u);
  for (uint32_t i = 0; i < kRegs; ++i) {
    uint32_t exp = (i == victim && mode == 1) ? 0 : 1;
    if (!(gaps && exp == 0)) {
      eg2_check(&tally, "1: register tag after the preemption", round * kRegs + i,
                rd(kOut1 + 8 * i), exp);
    }
    eg2_check(&tally, "1: register address after the preemption", round * kRegs + i,
              rd(kOut1 + 8 * i + 4), obj[i]);
  }
  eg2_check(&tally, "1: bitmap word cleared by the thread", round, eg2_ms_revbm_read(w), 0);
  eg2_put_kv("EG2 1 round", round);
  eg2_put_kv(" victim", victim);
  eg2_put_kv(" mode", mode);
  eg2_put_kv(" period", period);
  eg2_put_kv(" timer interrupts", timers);
  eg2_puts("\r\n");
  ++tally.done;
}

// ------------------------------------------- 2 -----------------------------------------------

static void part2(void) {
  ++tally.planned;
  uint32_t marked = 0;
  for (uint32_t i = 0; i < kNum2; ++i) {
    eg2_ms_csc(kSlots2 + 8 * i, eg2_ms_object_cap(kObj2 + 8 * i, 8, false));
    if (!eg2_check(&tally, "2: slot tagged", i, eg2_ms_tag(kSlots2 + 8 * i), 1)) {
      return;
    }
    if (eg2_pick(2)) {
      marked |= 1u << i;
    }
  }
  // kObj2's 16 granules are in one bitmap word.
  uint32_t w, b0;
  (void)eg2_ms_revbm_bit(kObj2, &w, &b0);
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  eg2_rv_op_revbm(&o, w, marked << b0);
  eg2_rv_op_sweep(&o, kSlots2, kNum2);
  uint32_t first = o.outs;
  for (uint32_t r = 0; r < kRounds2; ++r) {
    for (uint32_t i = 0; i < kNum2; ++i) {
      (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlots2 + 8 * i, 0);
      (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, eg2_rand_range(0, 30));
    }
  }
  uint32_t wait = eg2_rv_op_sweep_wait(&o);
  eg2_rv_op_revbm(&o, w, 0);
  eg2_check(&tally, "2: window fits", 0, o.full, 0);
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  ctx_wr(kCMarkAt, 0);
  ctx_wr(kCNTimer, 0);
  uint32_t period = draw_period();
  timer_start(period, period, true);
  __asm__ volatile("csrs mstatus, %0" : : "r"(kMstatusMie));
  eg2_rv_ops_run(&o, kOut2);
  __asm__ volatile("csrc mstatus, %0" : : "r"(kMstatusMie));
  timer_stop();
  bool injected = false;
  (void)injected;
  for (uint32_t r = 0; r < kRounds2; ++r) {
    for (uint32_t i = 0; i < kNum2; ++i) {
      bool m = (marked >> i) & 1;
      uint32_t exp = m ? 0 : 1;
#ifdef EG2_REVOCATION_IRQ_INJECT_FAIL
      if (!m && !injected) {
        injected = true;
        exp = 0;
      }
#endif
      if (!(gaps && m)) {
        eg2_check(&tally, "2: tag during the sweep", r * kNum2 + i,
                  rd(kOut2 + 4 * (first + r * kNum2 + i)), exp);
      }
    }
  }
  eg2_check(&tally, "2: sweep ended", 0, rd(kOut2 + 4 * wait) != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "2: TBRE_STATUS", 0, eg2_ms_tbre_status(), 0);
  eg2_check(&tally, "2: epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_ms_tbre_clear();
  for (uint32_t i = 0; i < kNum2; ++i) {
    bool m = (marked >> i) & 1;
    if (!(gaps && m)) {
      eg2_check(&tally, "2: tag after the sweep", i, eg2_ms_tag(kSlots2 + 8 * i), m ? 0 : 1);
    }
  }
  uint32_t timers = ctx_rd(kCNTimer);
  if (!gaps) {
    eg2_check(&tally, "2: timer interrupts during the window", 0, timers != 0 ? 1u : 0u, 1u);
  }
  eg2_put_kv("EG2 2 period", period);
  eg2_put_kv(" timer interrupts", timers);
  eg2_puts("\r\n");
  ++tally.done;
}

// ------------------------------------------- 3 -----------------------------------------------

static void part3(void) {
  ++tally.planned;
  eg2_ms_tbre_clear();
  for (uint32_t i = 0; i < 4; ++i) {
    eg2_ms_csc(kSlots3 + 8 * i, eg2_ms_object_cap(kObj2 + 8 * i, 8, false));
  }
  eg2_ms_plic_enable(kEg2MsTbreIrq, true);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_ENABLE_TBRE_DONE_BIT);
  __asm__ volatile("csrs mie, %0" : : "r"(kMipMeip));
  ctx_wr(kCMarkAt, 0);
  ctx_wr(kCNTimer, 0);
  ctx_wr(kCNExt, 0);
  ctx_wr(kCClaimed, 0);
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  // MIE stays clear: both become pending first.
  timer_start(200, 0, false);
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kSlots3, 4, &sw);
  uint32_t mip = 0, polls = 0;
  for (; polls < 1000000; ++polls) {
    __asm__ volatile("csrr %0, mip" : "=r"(mip));
    if ((mip & (kMipMtip | kMipMeip)) == (kMipMtip | kMipMeip)) {
      break;
    }
  }
  eg2_put_kv("EG2 3 mip", mip);
  eg2_put_kv(" polls", polls);
  eg2_put_kv(" tbre_done pending", eg2_ms_plic_pending(kEg2MsTbreIrq));
  eg2_put_kv(" sweep started", sw.started);
  eg2_put_kv(" TBRE_STATUS", eg2_ms_tbre_status());
  eg2_put_kv(" INTR_STATE",
             eg2_dev_read32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_STATE_REG_OFFSET));
  eg2_puts("\r\n");
  eg2_check(&tally, "3: both pending", 0, (mip & (kMipMtip | kMipMeip)) == (kMipMtip | kMipMeip),
            1);
  ctx_wr(kCOrderN, 0);
  __asm__ volatile("csrs mstatus, %0" : : "r"(kMstatusMie));
  eg2_ms_spin(200);
  __asm__ volatile("csrc mstatus, %0" : : "r"(kMstatusMie));
  timer_stop();
  __asm__ volatile("csrc mie, %0" : : "r"(kMipMeip));
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET, 0);
  eg2_ms_plic_enable(kEg2MsTbreIrq, false);
  eg2_check(&tally, "3: interrupts taken", 0, ctx_rd(kCOrderN), 2);
  eg2_check(&tally, "3: first taken", 0, ctx_rd(kCOrder), kMcauseExt);
  eg2_check(&tally, "3: second taken", 0, ctx_rd(kCOrder + 4), kMcauseTimer);
  eg2_check(&tally, "3: PLIC claim", 0, ctx_rd(kCClaimed), kEg2MsTbreIrq);
  eg2_check(&tally, "3: timer interrupts", 0, ctx_rd(kCNTimer), 1);
  eg2_check(&tally, "3: external interrupts", 0, ctx_rd(kCNExt), 1);
  eg2_check(&tally, "3: TBRE_STATUS", 0, eg2_ms_tbre_status(), 0);
  eg2_check(&tally, "3: epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_ms_tbre_clear();
  ++tally.done;
}

// ------------------------------------------- 4 -----------------------------------------------

static void part4(void) {
  ++tally.planned;
  eg2_ms_tbre_clear();
  ctx_wr(kCNNmi, 0);
  ctx_wr(kCNmiCause, 0);
  ctx_wr(kCStAtNmi, 0);
  uint32_t bark = EG2_PARAM(nmi_bark) != 0 ? EG2_PARAM(nmi_bark) : eg2_rand_range(2, 8);
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, 0xffffffffu);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, bark);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET,
                  1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT);
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET,
                  1u << RV_CORE_IBEX_NMI_ENABLE_WDOG_EN_BIT);
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kEg2SecBase, kEg2SecSize / 8, &sw);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 1);
  uint32_t polls = 0;
  while (ctx_rd(kCNNmi) == 0 && polls < 2000000) {
    ++polls;
  }
  eg2_ms_tbre_wait(&sw, 10000000);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  eg2_check(&tally, "4: NMIs taken", 0, ctx_rd(kCNNmi), 1);
  eg2_check(&tally, "4: NMI mcause", 0, ctx_rd(kCNmiCause), kMcauseNmi);
  eg2_check(&tally, "4: NMI during the sweep (TBRE_STATUS.busy)", 0, ctx_rd(kCStAtNmi) & kTbreBusy,
            kTbreBusy);
  eg2_check(&tally, "4: sweep ended", 0, sw.done, 1);
  eg2_check(&tally, "4: TBRE_STATUS", 0, sw.status, 0);
  eg2_check(&tally, "4: epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_check(&tally, "4: NMI_STATE", 0,
            eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_STATE_REG_OFFSET), 0);
  eg2_ms_tbre_clear();
  eg2_put_kv("EG2 4 bark", bark);
  eg2_put_kv(" polls", polls);
  eg2_puts("\r\n");
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n_rounds = EG2_PARAM(n_rounds) != 0 ? EG2_PARAM(n_rounds) : eg2_rand_range(8, 24);
  eg2_put_kv("EG2 revocation_irq rounds", n_rounds);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  ctx_init();
  handler_install();
  for (uint32_t r = 0; r < n_rounds; ++r) {
    part1(r);
  }
  part2();
  part3();
  if (!gaps) {
    part4();
  }
  handler_remove();
  eg2_check(&tally, "unexpected traps in the handler", 0, ctx_rd(kCNOther), 0);
  eg2_tally_print("revocation_irq", &tally);
  return eg2_tally_ok(&tally);
}
