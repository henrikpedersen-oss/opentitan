// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_trvk_backpressure (EG2 spec "Revocation", "Corner cases"; REQ_CIB_06,
 * REQ_CMS_11).
 *
 * CLC streams while the crossbar answers slowly. Each of `n_rounds` rounds marks `marked_pct` % of
 * 64 one-granule objects in sec SRAM's arena A, then runs, with nothing in between the
 * instructions of each (eg2_revoke.S):
 *  - six CLCs back to back from the slots (capabilities to the objects, in arena B), then six CSCs
 *    of the loaded registers to an output area;
 *  - a CLC and a word load through the loaded capability as the next instruction (a probe: a
 *    revoked capability traps with a tag violation, capcause 0x02, on ca2);
 *  - a CLC and CGetTag of the loaded register as the next instruction;
 *  - a CLC and a CSC of the same register;
 *  - a CSC of a fresh capability and a CLC of the same address straight after, while the write is
 *    still on its way to memory.
 * Around it: the bench's response delays on the SRAM link (`delays`), the TBRE sweeping main SRAM
 * up to arena B and SBA reads of sec SRAM (`contention`), and rv_timer interrupts `irq_delay` cycles
 * after each round starts (`irq`), taken through the probe handler.
 *
 * Check (the test's model of the marked granules and the load filter's expression, eg2_revoke.h):
 *  - every CLC's tag as the expression gives, the dependent load's trap or value, the tag CGetTag
 *    saw, the tag the CSC wrote; after the marks are cleared every stored result is reloaded and
 *    compared (tag, high word, address);
 *  - every request reaches the bus once: the bench counts the requests to arena B on the core's
 *    data port and on cheriot_mem_sys's port to the crossbar (two per CLC and per CSC) and the
 *    core's bitmap reads for the objects' granules (one per CLC of a tagged capability based in
 *    them); an interrupt taken between a load's request and its completion would repeat the
 *    request;
 *  - one timer interrupt per round that armed it; the sweeps end without error and the epoch
 *    advances by 2 per sweep;
 *  - the scoreboard's instruction check on every retired instruction (bench).
 *
 * Not generated (rtl_tickets.md, dv_todo.md): the debug halt (F13: rv_dm has no CHERIoT mode, the
 * halt cannot run the debug ROM in CHERIoT mode) and the NMI (the probe handler counts interrupts
 * 3, 7 and 11 only; the NMI case is chip_sw_cheriot_revocation_irq's).
 *
 * The marks cover the objects only: no capability the test keeps in a register across a call is
 * based in a marked granule, and the sequences keep loaded capabilities out of ct1 and ct2, which
 * the probe handler restores through memory on an interrupt.
 *
 * `model_gaps` (eg2sim): the SystemC model has no load filter and no revoking TBRE
 * (eg2_mpact_cpu.h), MPACT takes no interrupts here, and there is no bench: the revoked
 * expectations, the interrupts and the bench steps' results are skipped.
 *
 * Built with EG2_TRVK_BACKPRESSURE_INJECT_FAIL, the first live (not revoked) burst output is
 * expected untagged: the run must fail with `MISMATCH burst tag #<n>`.
 */

#include "hw/top/rv_timer_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TRVK_BACKPRESSURE_PARAMS(X) \
  X(n_rounds, 0, 0, 28)             \
  X(marked_pct, 101, 0, 101)        \
  X(delays, 2, 0, 2)                \
  X(contention, 4, 0, 4)            \
  X(irq, 2, 0, 2)                   \
  X(irq_delay, 0, 0, 2000)          \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TRVK_BACKPRESSURE_PARAMS);

// eg2_revoke.S.
extern void eg2_rv_burst6(void *slots, void *out);
extern uint32_t eg2_rv_clc_tag(void *slot);
extern void eg2_rv_clc_csc(void *src, void *dst);
extern uint32_t eg2_rv_csc_clc(void *slot, uint32_t base, uint32_t len);
extern char eg2_rv_stub_clc_lw[];

enum {
  kObjects = 64,
  kSlots = 64,
  // Arena B (the counted window): the slots, the burst outputs, the CLC/CSC destination, the
  // CSC/CLC slot.
  kWin = kEg2ArenaB,
  kWinTop = kEg2ArenaB + kEg2ArenaSize,
  kSlot = kWin,
  kOut = kWin + 0x400,
  kDst = kWin + 0x800,
  kRw = kWin + 0xc00,
  // The work area: per object its marked flag; per slot its object and the value stored.
  kMarked = kEg2WorkBase,
  kSlotObj = kMarked + 4 * kObjects,
  kSlotHi = kSlotObj + 4 * kSlots,
  kSlotAddr = kSlotHi + 4 * kSlots,
  // Per round: what the sequences returned.
  kRes = kSlotAddr + 4 * kSlots,
  kResDepTrap = 0,
  kResDepMcause,
  kResDepMtval,
  kResDepValue,
  kResClcTag,
  kResCscClcTag,
  kResFresh,
  kResWords,
  kBurst = 6,
  kMipMtie = 1u << 7,
  kMstatusMie = 1u << 3,
  kEg2IrqTimer = 1,
};

static eg2_tally_t tally;
static bool gaps;

// The objects: one granule each, spread over arena A (stride 17 granules), word 0 and 1 a pattern.
static uint32_t obj_addr(uint32_t i) { return kEg2ArenaA + 8 * ((i * 17u) % (kEg2ArenaSize / 8)); }
static uint32_t obj_word(uint32_t a) { return a ^ 0xa5a5a5a5u; }

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }
static void wr(uint32_t a, uint32_t v) { eg2_ms_write32(a, v); }

/** What a CLC of slot `s` returns: the stored value with the expression applied (full authority). */
static eg2_cap_t slot_expect(uint32_t s) {
  eg2_cap_t mem = {.hi = rd(kSlotHi + 4 * s), .addr = rd(kSlotAddr + 4 * s), .tag = true};
  bool marked = rd(kMarked + 4 * rd(kSlotObj + 4 * s)) != 0;
  return eg2_rv_expect(eg2_hw_read(eg2_ms_auth(kSlot)), mem, marked);
}

/** A check whose expected tag is 0 because of revocation is a model gap. */
static void check_tag(const char *what, uint32_t idx, uint32_t got, uint32_t exp, bool revoked) {
  if (gaps && revoked) {
    return;
  }
  eg2_check(&tally, what, idx, got, exp);
}

static void check_cap(const char *what, uint32_t idx, uint32_t addr, eg2_cap_t exp, bool revoked) {
  eg2_cap_t got = eg2_hw_read(eg2_ms_clc(addr));
  check_tag(what, idx, got.tag ? 1u : 0u, exp.tag ? 1u : 0u, revoked);
  eg2_check(&tally, "reloaded high word", idx, got.hi, exp.hi);
  eg2_check(&tally, "reloaded address", idx, got.addr, exp.addr);
}

static void timer_arm(uint32_t delay) {
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CFG0_REG_OFFSET, 1u << 16);  // step 1, prescale 0
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, delay);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
  __asm__ volatile("csrs mie, %0" : : "r"(kMipMtie));
  __asm__ volatile("csrs mstatus, %0" : : "r"(kMstatusMie));
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 1);
}

static void timer_disarm(void) {
  __asm__ volatile("csrc mstatus, %0" : : "r"(kMstatusMie));
  __asm__ volatile("csrc mie, %0" : : "r"(kMipMtie));
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
}

/** The TBRE sweeping main SRAM and arena A up to arena B, restarted whenever it is idle. */
static uint32_t sweeps;
static void sweep_kick(bool on) {
  if (!on || (eg2_ms_tbre_status() & 1u)) {
    return;
  }
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kEg2MsHeapBase, (kWin - kEg2MsHeapBase) / 8, &sw);
  eg2_check(&tally, "contention sweep started", sweeps, sw.started ? 1u : 0u, 1u);
  ++sweeps;
}

/** One round: the sequences, with the marks of this round set. */
static OT_NOINLINE void round_run(uint32_t r, bool bench, bool sweep, bool irq, uint32_t irq_delay,
                                  uint32_t *irqs_armed) {
  uint32_t first = (r * kBurst) % (kSlots - kBurst);
  volatile uint32_t *res = eg2_mem(kRes + 4 * kResWords * r, 4 * kResWords);
  uint32_t dep = (first + kBurst) % kSlots, tag1 = (first + kBurst + 1) % kSlots,
           cc = (first + kBurst + 2) % kSlots;
  uint32_t fresh = eg2_pick(kObjects);
  // The bench counts from here; the requests the test makes to arena B are planned below.
  (void)eg2_rv_count_begin(kWin, kWinTop, bench);
  sweep_kick(sweep);
  if (irq) {
    timer_arm(irq_delay);
    ++*irqs_armed;
  }
  eg2_rv_burst6(eg2_mem(kSlot + 8 * first, 8 * kBurst), eg2_mem(kOut + 8 * kBurst * (r % 16), 8 * kBurst));
  bool trapped = eg2_probe_call(eg2_mem(kSlot + 8 * dep, 8), NULL, NULL, eg2_rv_stub_clc_lw);
  res[kResDepTrap] = trapped;
  res[kResDepMcause] = trapped ? eg2_trap_rec.mcause : 0;
  res[kResDepMtval] = trapped ? eg2_trap_rec.mtval : 0;
  res[kResDepValue] = eg2_trap_rec.value;
  res[kResClcTag] = eg2_rv_clc_tag(eg2_mem(kSlot + 8 * tag1, 8));
  eg2_rv_clc_csc(eg2_mem(kSlot + 8 * cc, 8), eg2_mem(kDst + 8 * (r % 64), 8));
  res[kResCscClcTag] = eg2_rv_csc_clc(eg2_mem(kRw + 8 * (r % 64), 8), obj_addr(fresh), 8);
  res[kResFresh] = fresh;
  if (irq) {
    // The interrupt arrives within the round, or the wait for it here catches it.
    for (uint32_t i = 0; i < 20000 && eg2_trap_rec.irq_count[kEg2IrqTimer] < *irqs_armed; ++i) {
    }
    timer_disarm();
  }
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  bool bench = !gaps;
  uint32_t n_rounds = EG2_PARAM(n_rounds) != 0 ? EG2_PARAM(n_rounds) : eg2_rand_range(8, 28);
  uint32_t marked_pct =
      EG2_PARAM(marked_pct) == 101 ? eg2_rand_range(10, 90) : EG2_PARAM(marked_pct);
  bool delays = EG2_PARAM(delays) == 2 ? eg2_pick(4) != 0 : EG2_PARAM(delays) != 0;
  uint32_t contention = EG2_PARAM(contention) == 4 ? eg2_rand_range(0, 3) : EG2_PARAM(contention);
  bool irq = !gaps && (EG2_PARAM(irq) == 2 ? eg2_pick(4) != 0 : EG2_PARAM(irq) != 0);
  eg2_put_kv("EG2 trvk_backpressure rounds", n_rounds);
  eg2_put_kv(" marked_pct", marked_pct);
  eg2_put_kv(" delays", delays);
  eg2_put_kv(" contention", contention);
  eg2_put_kv(" irq", irq);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();

  // The objects' words, and the slots: slot s points at object s (plain or sealed with type 9).
  for (uint32_t i = 0; i < kObjects; ++i) {
    uint32_t a = obj_addr(i);
    wr(a, obj_word(a));
    wr(a + 4, obj_word(a + 4));
  }
  for (uint32_t s = 0; s < kSlots; ++s) {
    uint32_t o = s % kObjects;
    void *c = eg2_ms_object_cap(obj_addr(o) + 4 * eg2_pick(2), 4, eg2_pick(4) == 0);
    eg2_ms_csc(kSlot + 8 * s, c);
    eg2_cap_t v = eg2_hw_read(eg2_ms_clc(kSlot + 8 * s));
    wr(kSlotObj + 4 * s, o);
    wr(kSlotHi + 4 * s, v.hi);
    wr(kSlotAddr + 4 * s, v.addr);
    if (!eg2_check(&tally, "slot tagged", s, v.tag ? 1u : 0u, 1u)) {
      return false;
    }
  }

  eg2_cp_save_sealing_root();
  eg2_probe_begin();
  // The bench counts bitmap reads for the objects' granules only (arena A).
  (void)eg2_rv_count_revbm(kEg2ArenaA, kEg2ArenaA + kEg2ArenaSize, bench);
  if (delays) {
    (void)eg2_rv_delays(true, bench);
  }
  if (contention & 2) {
    (void)eg2_rv_sba_noise(true, bench);
  }
  uint32_t epoch0 = eg2_ms_tbre_epoch(), irqs_armed = 0;
  for (uint32_t r = 0; r < n_rounds; ++r) {
    ++tally.planned;
    // The marks of this round, on the objects only.
    for (uint32_t i = 0; i < kObjects; ++i) {
      bool m = eg2_rand_range(0, 99) < marked_pct;
      wr(kMarked + 4 * i, m);
      eg2_ms_revbm_mark(obj_addr(i), m);
    }
    uint32_t irq_delay = EG2_PARAM(irq_delay) != 0 ? EG2_PARAM(irq_delay) : eg2_rand_range(20, 400);
    round_run(r, bench, (contention & 1) != 0, irq, irq_delay, &irqs_armed);
    // The marks off before anything is logged or reloaded.
    for (uint32_t i = 0; i < kObjects; ++i) {
      eg2_ms_revbm_mark(obj_addr(i), false);
    }
    eg2_rv_counts_t cnt;
    (void)eg2_rv_count_end(&cnt, bench);

    // The checks, by reloading.
    uint32_t first = (r * kBurst) % (kSlots - kBurst);
    uint32_t dep = (first + kBurst) % kSlots, tag1 = (first + kBurst + 1) % kSlots,
             cc = (first + kBurst + 2) % kSlots;
    uint32_t res = kRes + 4 * kResWords * r;
    // CLCs: the burst, the dependent load, CGetTag, CLC/CSC, CSC/CLC; CSCs: the burst's, CLC/CSC,
    // CSC/CLC. Every CLC loads a tagged capability based in an object.
    uint32_t clcs = kBurst + 4, cscs = kBurst + 2, bitmap_reads = clcs;
    for (uint32_t j = 0; j < kBurst; ++j) {
      eg2_cap_t e = slot_expect(first + j);
      bool revoked = !e.tag;
#ifdef EG2_TRVK_BACKPRESSURE_INJECT_FAIL
      static bool injected;
      if (!injected && e.tag) {
        injected = true;
        e.tag = false;
      }
#endif
      check_cap("burst tag", r * kBurst + j, kOut + 8 * kBurst * (r % 16) + 8 * j, e, revoked);
    }
    eg2_cap_t e = slot_expect(dep);
    eg2_capx_t ex = eg2_cap_decode(e);
    if (!(gaps && !e.tag)) {
      // A revoked capability: tag violation on ca2 (12); a sealed one: seal violation.
      bool fault = !e.tag || ex.otype != 0;
      eg2_check(&tally, "dependent load trapped", r, rd(res + 4 * kResDepTrap), fault ? 1u : 0u);
      if (fault) {
        eg2_check(&tally, "dependent load mcause", r, rd(res + 4 * kResDepMcause), kEg2McauseCheri);
        eg2_check(&tally, "dependent load mtval", r, rd(res + 4 * kResDepMtval),
                  eg2_cap_mtval(12, !e.tag ? kEg2CauseTag : kEg2CauseSeal));
      } else {
        eg2_check(&tally, "dependent load value", r, rd(res + 4 * kResDepValue),
                  obj_word(e.addr));
      }
    }
    e = slot_expect(tag1);
    check_tag("CGetTag after CLC", r, rd(res + 4 * kResClcTag), e.tag, !e.tag);
    e = slot_expect(cc);
    check_cap("CSC of the loaded register", r, kDst + 8 * (r % 64), e, !e.tag);
    uint32_t fresh = rd(res + 4 * kResFresh);
    bool fresh_marked = rd(kMarked + 4 * fresh) != 0;
    check_tag("CLC after CSC", r, rd(res + 4 * kResCscClcTag), fresh_marked ? 0u : 1u,
              fresh_marked);
    if (cnt.valid) {
      eg2_check(&tally, "core data requests", r, cnt.core, 2 * (clcs + cscs));
      eg2_check(&tally, "crossbar data requests", r, cnt.xbar, 2 * (clcs + cscs));
      eg2_check(&tally, "bitmap reads", r, cnt.bitmap, bitmap_reads);
    }
    ++tally.done;
  }
  if (contention & 2) {
    (void)eg2_rv_sba_noise(false, bench);
  }
  if (delays) {
    (void)eg2_rv_delays(false, bench);
  }
  uint32_t irqs = eg2_trap_rec.irq_count[kEg2IrqTimer];
  eg2_probe_end();
  if (irq) {
    eg2_check(&tally, "timer interrupts", 0, irqs, irqs_armed);
  }
  if (contention & 1) {
    eg2_ms_sweep_t sw = {0};
    eg2_ms_tbre_wait(&sw, 10000000);
    eg2_check(&tally, "last sweep done", 0, sw.done ? 1u : 0u, 1u);
    eg2_check(&tally, "TBRE_STATUS", 0, sw.status, 0);
    eg2_check(&tally, "epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2 * sweeps);
  }
  eg2_ms_tbre_clear();
  eg2_put_kv("EG2 trvk_backpressure interrupts", irqs);
  eg2_put_kv(" sweeps", sweeps);
  eg2_puts("\r\n");
  eg2_tally_print("trvk_backpressure", &tally);
  return eg2_tally_ok(&tally);
}
