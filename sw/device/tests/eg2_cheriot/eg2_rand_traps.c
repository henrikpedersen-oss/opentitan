// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_traps (CHERIoT mode: `_cheriot`) and its RISC-V twin (EG2 verification spec,
 * "Traps, interrupts and control flow" and "chip_sw_eg2_rand_traps, RISC-V mode"; REQ_CIB_05,
 * REQ_CIB_06, REQ_PER_03).
 *
 * Every trap is expected: its cause, `mtval` and MEPCC (`mepc`) are computed by the test from the
 * architecture (CHERIoT-Sail's fetch, CJALR and CSR checks; the Ibex documentation's `mtval`), and
 * compared with what the handler recorded.
 *
 * CHERIoT mode:
 *  1. PCC bounds edges: eight code snippets (4-byte and compressed instructions, a 4-byte
 *     instruction straddling the top, a compressed one at top - 2, CJAL and a taken branch forward
 *     past the top and backward below the base, falling through the top) under every bounds
 *     [S + b, S + t) around them, entered by CJALR to cra, to c0 and to another register, with
 *     immediates -2, +2, +4, by c.cjalr and c.cjr, and by MRET. The test's fetch model gives
 *     where each run arrives (its ecall or c.ebreak) or which instruction faults (mcause 28, mtval
 *     PCC | bounds, MEPCC untagged at its address).
 *  2. Sentries: every object type 0-7 on the target, by every entry form, with MIE 0 and 1:
 *     CJALR's checks (tag, seal, type against the form, EX), the callee's MIE (types 2 and 3 set
 *     it, the others inherit), the link register's type (4 or 5 by MIE at the call) and its high
 *     word; a non-executable, an untagged and a data-sealed target; MRET to an untagged MEPCC.
 *  3. SR: CSR, SCR and MRET from a PCC with and without SR, against the ISA's allowlist.
 *  4. Illegal encodings: zero, all ones, CGetOffset, the legacy uccsr and mccsr, x16 (RV32E);
 *     ecall and ebreak.
 *  5. Interrupts: rv_plic's software interrupt, rv_timer (also into wfi), and a GPIO interrupt
 *     through the PLIC (claim and complete), entering through MTCC.
 *  6. `n_events` random events by weight (illegal encoding 20, CHERI fault 25, jump at a PCC edge
 *     20, interrupt 25; NMI and nested traps are not generated, see dv_todo).
 *
 * RISC-V mode:
 *  1. ePMP execute edges: a 64-byte snippet of 15 nops and an ecall under TOR, NA4 and NAPOT
 *     regions without X starting at each instruction, locked (M mode) and unlocked (U mode): an
 *     instruction access fault at the region's first instruction, or the ecall.
 *  2. Illegal encodings (zero, all ones, a CHERIoT instruction, an absent CSR, mstatus from U);
 *     ecall from M and U; ebreak.
 *  3. Interrupts as in CHERIoT mode, and 4. random events.
 *
 * Built with EG2_RAND_TRAPS_INJECT_FAIL, the first fetch that must fault is expected to arrive.
 * The run must fail there (`MISMATCH pcc edge` or, in RISC-V mode, `MISMATCH epmp exec`), with or
 * without `model_gaps`.
 *
 * `model_gaps` (eg2sim only; 0 on the chip, where every check counts): in CHERIoT mode the
 * differences of eg2sim's MPACT-CHERIoT from CHERIoT-Sail (status/dv_todo.md C2) are compared and
 * printed but not counted, per check:
 *  - pcc edges: MEPCC's tag after a PCC bounds fault (MPACT leaves it set), and every check of
 *    a CJALR with immediate -2, and `mtval` of a fetch below PCC's base after a jump (MPACT
 *    reports a tag violation, 0x402; the CHERIoT ISA a bounds violation, 0x401);
 *  - SR: `mtval` of CSpecialRW without SR (MPACT names PCC); `mcycle` without SR (MPACT and
 *    CHERIoT-Sail allow it, the ISA text does not: status/dv_todo.md C3); MRET without SR is not
 *    run (MPACT does not trap it and leaves the probe);
 *  - illegal encodings: uccsr, mccsr and x16 (MPACT executes them); `ebreak`'s `mtval` (MPACT
 *    writes the PC; status/dv_todo.md C3: the Ibex documentation says 0, CHERIoT-Sail the PC).
 *    The same for the c.ebreak a pcc-edge snippet ends at;
 *  - interrupts are not raised (MPACT takes an interrupt with MIE 0, inside the probe's trap
 *    handler, which then faults on its own MScratchC): phase 5 and the random interrupt events
 *    are skipped.
 * RISC-V mode has no model gaps.
 */

#include "hw/top/gpio_regs.h"
#include "hw/top/rv_plic_regs.h"
#include "hw/top/rv_timer_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define RAND_TRAPS_PARAMS(X) \
  X(n_events, 0, 0, 1024)    \
  X(boost, 0, 0, 255)       \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(RAND_TRAPS_PARAMS);

// model_gaps: the checks of MPACT-CHERIoT's known differences go to `gap_tally` (see above).
static bool gaps;
static eg2_tally_t gap_tally;
__attribute__((unused)) static eg2_tally_t *gap(eg2_tally_t *t) { return gaps ? &gap_tally : t; }

enum { kMstatusMie = 1u << 3, kMstatusMpie = 1u << 7 };

// Set once the inject-fail build has broken its one expectation.
__attribute__((unused)) static bool injected;

static void mie_set(bool on) {
  if (on) {
    __asm__ volatile("csrs mstatus, %0" : : "r"(kMstatusMie));
  } else {
    __asm__ volatile("csrc mstatus, %0" : : "r"(kMstatusMie));
  }
}

// The instruction a stub starts with, as mtval holds it for an illegal instruction (Ibex
// doc/03_reference/cs_registers.rst: "mtval holds the actual faulting instruction").
static uint32_t insn_at(const char *stub) {
  const volatile uint16_t *h = (const volatile uint16_t *)stub;
  uint32_t lo = h[0];
  return (lo & 3) != 3 ? lo : lo | (uint32_t)h[1] << 16;
}

static void check_trap(eg2_tally_t *t, const char *what, uint32_t idx, bool trapped,
                       uint32_t mcause, uint32_t mtval, uint32_t mepc) {
  uint32_t failed = t->failed;
  eg2_check(t, what, idx, trapped ? 1u : 0u, 1u);
  eg2_check(t, what, idx, eg2_trap_rec.mcause, mcause);
  eg2_check(t, what, idx, eg2_trap_rec.mtval, mtval);
#ifdef __CHERIOT__
  eg2_check(t, what, idx, (uint32_t)__builtin_cheri_address_get(eg2_trap_rec.mepcc.p), mepc);
#else
  eg2_check(t, what, idx, (uint32_t)eg2_trap_rec.mepcc.pad, mepc);
#endif
  if (t->failed != failed && failed < kEg2MaxReports) {
    eg2_puts("  expected mcause 0x");
    eg2_puthex(mcause);
    eg2_puts(" mtval 0x");
    eg2_puthex(mtval);
    eg2_puts(" mepc 0x");
    eg2_puthex(mepc);
    eg2_puts("\r\n");
  }
}

// --------------------------------------- Interrupts ------------------------------------------

enum { kIrqSoft = 0, kIrqTimer = 1, kIrqExt = 2 };
enum { kGpioPlicId = 37, kIrqPolls = 2000 };

static bool wait_irq(uint32_t which, uint32_t before) {
  for (uint32_t i = 0; i < kIrqPolls; ++i) {
    if (eg2_trap_rec.irq_count[which] != before) {
      return true;
    }
  }
  return false;
}

static void timer_arm(uint32_t delay) {
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CFG0_REG_OFFSET, 1u << 16);  // step 1, prescale 0
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, delay);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 1);
}

static void timer_ack(void) {
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0xffffffffu);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 0xffffffffu);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
}

// A line per step of an interrupt event (2026-10-27: the CHERIoT run on Verilator printed "irq
// event 0", the rv_plic software interrupt, and nothing more in the hour; the RISC-V run passes).
static void irq_step(const char *what) {
  eg2_puts("  irq: ");
  eg2_puts(what);
  eg2_puts("\r\n");
}

// One interrupt from `which`; `in_wfi` waits for the timer in wfi.
static void irq_event(eg2_tally_t *t, uint32_t which, bool in_wfi, uint32_t idx) {
  uint32_t before = eg2_trap_rec.irq_count[which];
  uint32_t traps = eg2_trap_rec.count;
  uint32_t bit = which == kIrqSoft ? 3u : (which == kIrqTimer ? 7u : 11u);
  __asm__ volatile("csrs mie, %0" : : "r"(1u << bit));
  irq_step("mie bit set");
  mie_set(true);
  irq_step("mstatus.MIE set");
  if (which == kIrqSoft) {
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_MSIP0_REG_OFFSET, 1);
  } else if (which == kIrqTimer) {
    timer_arm(in_wfi ? 200 : 20);
    if (in_wfi) {
      __asm__ volatile("wfi");
    }
  } else {
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_PRIO_0_REG_OFFSET + 4 * kGpioPlicId, 1);
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_IE0_0_REG_OFFSET + 4 * (kGpioPlicId / 32),
                    1u << (kGpioPlicId % 32));
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_THRESHOLD0_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevGpio, GPIO_INTR_ENABLE_REG_OFFSET, 1);
    eg2_dev_write32(kEg2DevGpio, GPIO_INTR_TEST_REG_OFFSET, 1);
  }
  irq_step("source raised");
  bool taken = wait_irq(which, before);
  mie_set(false);
  eg2_puts("  irq: wait done, taken ");
  eg2_puthex(taken ? 1u : 0u);
  eg2_puts(" irq_count ");
  eg2_puthex(eg2_trap_rec.irq_count[which] - before);
  eg2_puts(" other traps ");
  eg2_puthex(eg2_trap_rec.count - traps);
  eg2_puts("\r\n");
  eg2_check(t, which == kIrqSoft ? "irq software" : (which == kIrqTimer ? "irq timer" : "irq ext"),
            idx, taken ? 1u : 0u, 1u);
  // The source cleared; the handler masked it in mie.
  if (which == kIrqSoft) {
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_MSIP0_REG_OFFSET, 0);
  } else if (which == kIrqTimer) {
    timer_ack();
  } else {
    uint32_t id = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
    eg2_check(t, "irq ext claim", idx, id, kGpioPlicId);
    eg2_dev_write32(kEg2DevGpio, GPIO_INTR_ENABLE_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevGpio, GPIO_INTR_STATE_REG_OFFSET, 1);
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, id);
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_IE0_0_REG_OFFSET + 4 * (kGpioPlicId / 32), 0);
  }
  __asm__ volatile("csrc mie, %0" : : "r"(1u << bit));
  irq_step("source cleared");
}

static void phase_irqs(eg2_tally_t *t) {
  for (uint32_t i = 0; i < 4; ++i) {
    if (gaps) {
      ++t->done;  // model_gaps: no interrupts (the header)
      continue;
    }
    // A line per event, so a hang shows which (2026-10-27: the CHERIoT run on Verilator stopped
    // in this phase, after "EG2 illegal", with no output for the rest of the hour).
    eg2_puts("irq event ");
    eg2_putdec(i);
    eg2_puts("\r\n");
    irq_event(t, i == 3 ? kIrqTimer : i, i == 3, i);
    ++t->done;
  }
}

// ------------------------------------- Illegal encodings -------------------------------------

#ifdef __CHERIOT__
enum { kIllegalCount = 6 };
static char *illegal_stub(uint32_t i) {
  switch (i) {
    case 0:
      return eg2_stub_illegal_zero;
    case 1:
      return eg2_stub_illegal_ones;
    case 2:
      return eg2_stub_illegal_cgetoffset;
    case 3:
      return eg2_stub_illegal_uccsr;
    case 4:
      return eg2_stub_illegal_mccsr;
    default:
      return eg2_stub_illegal_x16;
  }
}
#else
enum { kIllegalCount = 5 };
static char *illegal_stub(uint32_t i) {
  switch (i) {
    case 0:
      return eg2_stub_illegal_zero;
    case 1:
      return eg2_stub_illegal_ones;
    case 2:
      return eg2_stub_illegal_cheri_in_rv;
    case 3:
      return eg2_stub_illegal_uccsr;
    default:
      return eg2_stub_csr_mstatus;  // from U mode
  }
}
#endif

static void illegal_event(eg2_tally_t *t, uint32_t i, uint32_t idx) {
  char *stub = illegal_stub(i);
  bool trapped;
#ifdef __CHERIOT__
  trapped = eg2_probe_call(NULL, NULL, NULL, stub);
#else
  trapped = i == 4 ? eg2_probe_call_umode(NULL, NULL, stub)
                   : eg2_probe_call(NULL, NULL, NULL, stub);
#endif
#ifdef __CHERIOT__
  // model_gaps: uccsr, mccsr and x16 (the header).
  eg2_tally_t *ct = i >= 3 ? gap(t) : t;
#else
  eg2_tally_t *ct = t;
#endif
  check_trap(ct, "illegal", idx, trapped, kEg2McauseIllegal, insn_at(stub), EG2_ADDR(stub));
}

static void phase_illegal(eg2_tally_t *t) {
  uint32_t idx = 0;
  for (uint32_t i = 0; i < kIllegalCount; ++i) {
    illegal_event(t, i, idx++);
  }
  // ecall and ebreak: mtval 0 (Ibex: "for all other exceptions mtval is 0").
  bool trapped = eg2_probe_call(NULL, NULL, NULL, eg2_stub_ecall);
  check_trap(t, "ecall", 0, trapped, kEg2McauseEcallM, 0, EG2_ADDR(eg2_stub_ecall));
  trapped = eg2_probe_call(NULL, NULL, NULL, eg2_stub_ebreak);
  // model_gaps: ebreak's mtval (the header); the cause and MEPCC count.
  check_trap(t, "ebreak", 0, trapped, kEg2McauseBreakpoint, eg2_trap_rec.mtval,
             EG2_ADDR(eg2_stub_ebreak));
  eg2_check(gap(t), "ebreak mtval", 0, eg2_trap_rec.mtval, 0);
#ifndef __CHERIOT__
  trapped = eg2_probe_call_umode(NULL, NULL, eg2_stub_ecall);
  check_trap(t, "ecall U", 0, trapped, kEg2McauseEcallU, 0, EG2_ADDR(eg2_stub_ecall));
#endif
  ++t->done;
}

#ifdef __CHERIOT__

// ======================================= CHERIoT mode =======================================

// ---------------------------------------- Fetch model ----------------------------------------

typedef enum ikind { kINop, kIEcall, kIEbreak, kIJump, kIIllegal } ikind_t;

typedef struct insn {
  uint8_t off, len, kind, target;
} insn_t;

typedef struct snippet {
  char *code;
  uint32_t size, entry, n;
  insn_t insn[3];
} snippet_t;

enum { kSnippetCount = 8 };

// The layouts of eg2_probe.S's snippets.
static snippet_t snippet(uint32_t s) {
  snippet_t d;
  switch (s) {
    case 0:
      d = (snippet_t){eg2_snip_w, 8, 0, 2, {{0, 4, kINop, 0}, {4, 4, kIEcall, 0}}};
      break;
    case 1:
      d = (snippet_t){eg2_snip_c, 4, 0, 2, {{0, 2, kINop, 0}, {2, 2, kIEbreak, 0}}};
      break;
    case 2:
      d = (snippet_t){eg2_snip_cw, 6, 0, 2, {{0, 2, kINop, 0}, {2, 4, kIEcall, 0}}};
      break;
    case 3:
      d = (snippet_t){eg2_snip_cjal, 12, 0, 3,
                      {{0, 4, kIJump, 8}, {4, 4, kIIllegal, 0}, {8, 4, kIEcall, 0}}};
      break;
    case 4:
      d = (snippet_t){eg2_snip_beq, 12, 0, 3,
                      {{0, 4, kIJump, 8}, {4, 4, kIIllegal, 0}, {8, 4, kIEcall, 0}}};
      break;
    case 5:
      d = (snippet_t){eg2_snip_back, 8, 4, 2, {{0, 4, kIEcall, 0}, {4, 4, kIJump, 0}}};
      break;
    case 6:
      d = (snippet_t){eg2_snip_fall, 12, 0, 3,
                      {{0, 4, kINop, 0}, {4, 4, kINop, 0}, {8, 4, kIEcall, 0}}};
      break;
    default:
      d = (snippet_t){eg2_snip_cj, 6, 0, 3,
                      {{0, 2, kIJump, 4}, {2, 2, kIIllegal, 0}, {4, 2, kIEbreak, 0}}};
      break;
  }
  return d;
}

typedef struct fetch_result {
  uint32_t mcause, mtval, pc;
  bool pcc_tag;  // MEPCC's tag
} fetch_result_t;

// Runs the snippet from `pc` under `pcc` (Sail ext_fetch_check_pc: each 2-byte granule of an
// instruction in bounds; a failure is a CHERI exception on PCC, MEPCC untagged).
static fetch_result_t fetch_model(const snippet_t *d, eg2_cap_t pcc, uint32_t pc) {
  uint32_t s = EG2_ADDR(d->code);
  fetch_result_t r = {0};
  if (!pcc.tag) {
    r.mcause = kEg2McauseCheri;
    r.mtval = eg2_cap_mtval(kEg2RegPcc, kEg2CauseTag);
    r.pc = pc;
    return r;
  }
  for (uint32_t steps = 0; steps < 8; ++steps) {
    const insn_t *in = NULL;
    for (uint32_t i = 0; i < d->n; ++i) {
      if (s + d->insn[i].off == pc) {
        in = &d->insn[i];
      }
    }
    bool g0 = eg2_cap_in_bounds(pcc, pc, 2);
    bool g1 = in == NULL || in->len == 2 || eg2_cap_in_bounds(pcc, pc + 2, 2);
    if (!g0 || !g1) {
      r.mcause = kEg2McauseCheri;
      r.mtval = eg2_cap_mtval(kEg2RegPcc, kEg2CauseBounds);
      r.pc = pc;
      return r;
    }
    if (in == NULL || in->kind == kIIllegal) {
      r.mcause = kEg2McauseIllegal;  // not reached by any layout
      r.pc = pc;
      return r;
    }
    if (in->kind == kIEcall || in->kind == kIEbreak) {
      r.mcause = in->kind == kIEcall ? kEg2McauseEcallM : kEg2McauseBreakpoint;
      r.pc = pc;
      r.pcc_tag = true;
      return r;
    }
    pc = in->kind == kIJump ? s + in->target : pc + in->len;
  }
  r.mcause = 0xffffffffu;
  return r;
}

// ---------------------------------------- Jump forms -----------------------------------------

typedef struct form_info {
  int32_t imm;
  bool cd_ra, cd_zero, cs1_ra, mret;
  uint32_t cs1;
} form_info_t;

static form_info_t form_info(eg2_jump_form_t f) {
  form_info_t i = {0, false, false, false, false, 13};
  switch (f) {
    case kEg2JumpCall:
    case kEg2JumpCCall:
      i.cd_ra = true;
      break;
    case kEg2JumpTail:
    case kEg2JumpCTail:
      i.cd_zero = true;
      break;
    case kEg2JumpOther:
      break;
    case kEg2JumpRet:
      i.cd_zero = true;
      i.cs1_ra = true;
      i.cs1 = 1;
      break;
    case kEg2JumpCallM2:
      i.cd_ra = true;
      i.imm = -2;
      break;
    case kEg2JumpCallP2:
      i.cd_ra = true;
      i.imm = 2;
      break;
    case kEg2JumpCallP4:
      i.cd_ra = true;
      i.imm = 4;
      break;
    default:
      i.mret = true;
      break;
  }
  return i;
}

typedef struct jump_exp {
  uint32_t mcause, mtval, pc;
  bool check_pc;
  bool arrived;
  eg2_cap_t pcc;  // the PCC the target runs under
} jump_exp_t;

// CJALR's checks and the new PCC (Sail CJALR), or MRET through MEPCC's legalisation.
static jump_exp_t jump_model(eg2_cp_t target, eg2_jump_form_t form) {
  form_info_t fi = form_info(form);
  jump_exp_t e = {0};
  if (fi.mret) {
    e.pcc = eg2_cap_scr_legalise(31, target.m);
    e.pc = e.pcc.addr;
    return e;
  }
  int cause = eg2_cap_check_cjalr(target.m, (uint32_t)fi.imm, fi.cd_ra, fi.cd_zero, fi.cs1_ra);
  if (cause != 0) {
    e.mcause = kEg2McauseCheri;
    e.mtval = eg2_cap_mtval(fi.cs1, (uint32_t)cause);
    return e;
  }
  eg2_capx_t x = eg2_cap_decode(target.m);
  x.otype = 0;  // unsealed into PCC
  e.pcc = eg2_cap_encode(&x);
  e.pc = (target.m.addr + (uint32_t)fi.imm) & ~1u;
  e.arrived = true;
  return e;
}

static eg2_cp_t code_cap(uint32_t base, uint32_t len, uint32_t addr) {
  eg2_cp_t c = eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_exe(), base), len, kEg2BoundsInexact);
  return eg2_cp_set_addr(c, addr);
}

// One jump into a snippet, checked against the models.
static void jump_case(eg2_tally_t *t, const char *what, uint32_t idx, uint32_t sn,
                      eg2_cp_t target, eg2_jump_form_t form) {
  snippet_t d = snippet(sn);
  jump_exp_t j = jump_model(target, form);
  uint32_t mcause, mtval, pc;
  bool tag;
  if (j.mcause != 0) {
    mcause = j.mcause;
    mtval = j.mtval;
    pc = 0;
    tag = true;
  } else {
    fetch_result_t f = fetch_model(&d, j.pcc, j.pc);
    mcause = f.mcause;
    mtval = f.mtval;
    pc = f.pc;
    tag = f.pcc_tag;
  }
  // model_gaps: a CJALR with immediate -2 (the header).
  eg2_tally_t *ct = form == kEg2JumpCallM2 ? gap(t) : t;
#ifdef EG2_RAND_TRAPS_INJECT_FAIL
  if (!injected && mcause == kEg2McauseCheri && mtval == eg2_cap_mtval(kEg2RegPcc, 1)) {
    mcause = kEg2McauseEcallM;  // a wrong expectation: the fetch past the top arrives
    injected = true;
    ct = t;
  }
#endif
  bool trapped = eg2_probe_jump(NULL, NULL, target.hw, form);
  uint32_t failed = ct->failed;
  eg2_check(ct, what, idx, trapped ? 1u : 0u, 1u);
  eg2_check(ct, what, idx, eg2_trap_rec.mcause, mcause);
  // model_gaps: an ebreak's mtval, and the cause in mtval of a fetch below PCC's base (the
  // header).
  bool below = j.mcause == 0 && mcause == kEg2McauseCheri && pc < eg2_cap_base(j.pcc);
  eg2_check(mcause == kEg2McauseBreakpoint || below ? gap(ct) : ct, what, idx, eg2_trap_rec.mtval,
            mtval);
  if (j.mcause == 0) {
    // The faulting or arriving instruction, and MEPCC's tag (cleared by a bounds fault; model_gaps:
    // the tag, see the header).
    eg2_check(ct, what, idx, (uint32_t)__builtin_cheri_address_get(eg2_trap_rec.mepcc.p), pc);
    eg2_check(gap(ct), what, idx, eg2_trap_rec.mepcc_tag, tag ? 1u : 0u);
  }
  if (ct->failed != failed && failed < kEg2MaxReports) {
    eg2_puts("  snippet ");
    eg2_putdec(sn);
    eg2_puts(" form ");
    eg2_putdec(form);
    eg2_cap_print("target", target.m);
  }
}

static void phase_pcc_edges(eg2_tally_t *t) {
  static const uint8_t kForms[] = {kEg2JumpCall, kEg2JumpTail, kEg2JumpOther, kEg2JumpCCall,
                                   kEg2JumpCTail, kEg2JumpMret, kEg2JumpCallM2, kEg2JumpCallP2};
  uint32_t idx = 0;
  for (uint32_t sn = 0; sn < kSnippetCount; ++sn) {
    snippet_t d = snippet(sn);
    uint32_t s = EG2_ADDR(d.code);
    // Bases 4 below the snippet, at it, 2 into it; tops from the entry to 2 past its end.
    for (uint32_t b = 0; b < 3; ++b) {
      uint32_t base = b == 0 ? s - 4 : (b == 1 ? s : s + 2);
      for (uint32_t top = s + d.entry; top <= s + d.size + 2; ++top) {
        if (top < base) {
          continue;
        }
        for (uint32_t f = 0; f < sizeof(kForms); ++f) {
          eg2_jump_form_t form = (eg2_jump_form_t)kForms[f];
          // The capability's address: the entry, or 2 off it for the immediate forms.
          uint32_t entry = s + d.entry;
          uint32_t addr = form == kEg2JumpCallM2 ? entry + 2
                                                 : (form == kEg2JumpCallP2 ? entry - 2 : entry);
          eg2_cp_t c = code_cap(base, top - base, addr);
          jump_case(t, "pcc edge", idx++, sn, c, form);
        }
      }
    }
    ++t->done;
  }
}

// ------------------------------------------ Sentries -----------------------------------------

static void phase_sentries(eg2_tally_t *t) {
  uint32_t stub = EG2_ADDR(eg2_stub_arrive);
  eg2_cp_t seal_root = eg2_cp_root_seal();
  eg2_cp_t code = code_cap(stub & ~7u, 32, stub);
  eg2_cp_t pcc_root = eg2_cp_root_exe();
  uint32_t idx = 0;
  for (uint32_t ot = 0; ot < 8; ++ot) {
    eg2_cp_t target = ot == 0 ? code : eg2_cp_seal(code, eg2_cp_set_addr(seal_root, ot));
    for (uint32_t f = 0; f < kEg2JumpFormCount; ++f) {
      eg2_jump_form_t form = (eg2_jump_form_t)f;
      if (form == kEg2JumpCallM2 || form == kEg2JumpCallP2 || form == kEg2JumpCallP4) {
        continue;  // would land inside the arrival stub's first instruction
      }
      for (uint32_t m = 0; m < 2; ++m) {
        jump_exp_t j = jump_model(target, form);
        mie_set(m != 0);
        bool trapped = eg2_probe_jump(NULL, NULL, target.hw, form);
        mie_set(false);
        uint32_t failed = t->failed;
        eg2_check(t, "sentry", idx, trapped ? 1u : 0u, 1u);
        if (j.mcause != 0) {
          eg2_check(t, "sentry", idx, eg2_trap_rec.mcause, j.mcause);
          eg2_check(t, "sentry", idx, eg2_trap_rec.mtval, j.mtval);
        } else if (!j.pcc.tag) {
          // MRET to a sealed target: MEPCC's legalisation cleared the tag; the fetch faults.
          eg2_check(t, "sentry", idx, eg2_trap_rec.mcause, kEg2McauseCheri);
          eg2_check(t, "sentry", idx, eg2_trap_rec.mtval, eg2_cap_mtval(kEg2RegPcc, kEg2CauseTag));
        } else {
          eg2_check(t, "sentry", idx, eg2_trap_rec.mcause, kEg2McauseEcallM);
          if (!form_info(form).mret) {
            // The callee's MIE (MPIE at its ecall): types 2 and 3 set it, the others inherit.
            uint32_t mie = ot == kEg2OtypeSentryId ? 0u : (ot == kEg2OtypeSentryIe ? 1u : m);
            if (form == kEg2JumpRet) {
              mie = ot == kEg2OtypeSentryBid ? 0u : (ot == kEg2OtypeSentryBie ? 1u : m);
            }
            eg2_check(t, "sentry MIE", idx, (eg2_trap_rec.mstatus & kMstatusMpie) ? 1u : 0u, mie);
            // The link register: a backward sentry by MIE at the call (the RET form keeps the
            // target in cra).
            uint32_t want_type = form == kEg2JumpRet
                                     ? eg2_cap_otype(target.m)
                                     : (m ? kEg2OtypeSentryBie : kEg2OtypeSentryBid);
            eg2_check(t, "sentry link type", idx, eg2_trap_rec.value, want_type);
            if (form == kEg2JumpCall || form == kEg2JumpCallP2 || form == kEg2JumpCCall) {
              eg2_cap_t link = eg2_cap_link(pcc_root.m, 0, true, m != 0);
              eg2_check(t, "sentry link high", idx, eg2_trap_rec.value2, link.hi);
            }
          }
        }
        if (t->failed != failed && failed < kEg2MaxReports) {
          eg2_puts("  otype ");
          eg2_putdec(ot);
          eg2_puts(" form ");
          eg2_putdec(form);
          eg2_puts(" mie ");
          eg2_putdec(m);
          eg2_puts("\r\n");
        }
        ++idx;
      }
    }
    ++t->done;
  }
  // Targets CJALR refuses: not executable, untagged, sealed as data; MRET to an untagged MEPCC.
  eg2_cp_t data = eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_mem(), stub), 16, kEg2BoundsExact);
  eg2_cp_t bad[3] = {data, eg2_cp_clear_tag(code),
                     eg2_cp_seal(data, eg2_cp_set_addr(seal_root, 9))};
  for (uint32_t i = 0; i < 3; ++i) {
    jump_exp_t j = jump_model(bad[i], kEg2JumpCall);
    bool trapped = eg2_probe_jump(NULL, NULL, bad[i].hw, kEg2JumpCall);
    eg2_check(t, "cjalr refused", i, trapped ? 1u : 0u, 1u);
    eg2_check(t, "cjalr refused", i, eg2_trap_rec.mcause, j.mcause);
    eg2_check(t, "cjalr refused", i, eg2_trap_rec.mtval, j.mtval);
  }
  bool trapped = eg2_probe_jump(NULL, NULL, eg2_cp_clear_tag(code).hw, kEg2JumpMret);
  check_trap(t, "mret untagged", 0, trapped, kEg2McauseCheri,
             eg2_cap_mtval(kEg2RegPcc, kEg2CauseTag), stub);
  ++t->done;
}

// ---------------------------------------------- SR -------------------------------------------

typedef struct sr_case {
  char *stub;
  bool allowed_without_sr;
  uint32_t mtval;  // without SR
} sr_case_t;

enum { kSrCases = 7 };

// The ISA's allowlist (CHERIoT ISA, "Controlling access to system registers"): cycle, time,
// instret and hpmcounter reads only; CSpecialRW and MRET need SR.
static sr_case_t sr_case(uint32_t i) {
  uint32_t pcc_sr = eg2_cap_mtval(kEg2RegPcc, kEg2CauseSr);
  switch (i) {
    case 0:
      return (sr_case_t){eg2_stub_csr_mstatus, false, pcc_sr};
    case 1:
      return (sr_case_t){eg2_stub_csr_cycle, true, 0};
    case 2:
      return (sr_case_t){eg2_stub_csr_hpm3, true, 0};
    case 3:
      return (sr_case_t){eg2_stub_csr_mcycle, false, pcc_sr};
    case 4:
      return (sr_case_t){eg2_stub_scr_mtdc, false, eg2_cap_mtval(0x20 | 29, kEg2CauseSr)};
    case 5:
      return (sr_case_t){eg2_stub_scr_mtcc, false, eg2_cap_mtval(0x20 | 28, kEg2CauseSr)};
    default:
      return (sr_case_t){eg2_stub_mret, false, pcc_sr};
  }
}

static void phase_sr(eg2_tally_t *t) {
  for (uint32_t i = 0; i < kSrCases; ++i) {
    sr_case_t c = sr_case(i);
    uint32_t s = EG2_ADDR(c.stub);
    for (uint32_t sr = 0; sr < 2; ++sr) {
      if (sr == 1 && i == kSrCases - 1) {
        continue;  // MRET with SR would leave the probe
      }
      if (gaps && sr == 0 && i == kSrCases - 1) {
        continue;  // model_gaps: MRET without SR (the header)
      }
      eg2_cp_t pcc = code_cap(s, 8, s);
      if (sr == 0) {
        pcc = eg2_cp_and_perm(pcc, ~(uint32_t)kEg2PermSr);
      }
      bool trapped = eg2_probe_call(NULL, NULL, NULL, pcc.hw);
      if (sr == 1 || c.allowed_without_sr) {
        eg2_check(t, sr ? "with SR" : "without SR (allowed)", i, trapped ? 1u : 0u, 0);
      } else {
        // model_gaps: mcycle (allowed by CHERIoT-Sail and MPACT, status/dv_todo.md C3) and the
        // SCRs' mtval (the header).
        check_trap(i >= 3 ? gap(t) : t, "without SR", i, trapped, kEg2McauseCheri, c.mtval, s);
      }
    }
  }
  ++t->done;
}

// ------------------------------------------ Random -------------------------------------------

enum { kEvIllegal, kEvCheri, kEvEdge, kEvIrq, kEvCount };
static const uint8_t kEvDefault[kEvCount] = {20, 25, 20, 25};

static void rand_event(eg2_tally_t *t, eg2_family_t *ev, uint32_t n) {
  switch (eg2_family_pick(ev)) {
    case kEvIllegal:
      illegal_event(t, eg2_pick(kIllegalCount), n);
      break;
    case kEvCheri: {
      // A data access past a random small capability's top.
      uint32_t len = eg2_rand_range(1, 64);
      eg2_cp_t c = eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_mem(), kEg2ArenaA + 0x100), len,
                                 kEg2BoundsExact);
      eg2_cp_t at = eg2_cp_set_addr(c, c.m.addr + len);
      bool trapped = eg2_probe_call(at.hw, NULL, NULL, eg2_stub_lb);
      check_trap(t, "rand cheri", n, trapped, kEg2McauseCheri,
                 eg2_cap_mtval(10, kEg2CauseBounds), EG2_ADDR(eg2_stub_lb));
      break;
    }
    case kEvEdge: {
      uint32_t sn = eg2_pick(kSnippetCount);
      snippet_t d = snippet(sn);
      uint32_t s = EG2_ADDR(d.code);
      uint32_t top = s + d.entry + eg2_rand_range(0, d.size + 2 - d.entry);
      jump_case(t, "rand edge", n, sn, code_cap(s, top - s, s + d.entry),
                (eg2_jump_form_t)eg2_pick(kEg2JumpRet));
      break;
    }
    default:
      if (!gaps) {  // model_gaps: no interrupts (the header)
        irq_event(t, eg2_pick(3), false, n);
      }
      break;
  }
}

#else  // RISC-V mode

// ======================================== RISC-V mode =======================================

enum { kPmpX = 4, kPmpR = 1, kPmpTor = 1 << 3, kPmpNa4 = 2 << 3, kPmpNapot = 3 << 3, kPmpL = 0x80 };

static void pmp_set(uint32_t cfg8, uint32_t addr7, uint32_t addr8) {
  __asm__ volatile("csrw pmpaddr7, %0" : : "r"(addr7));
  __asm__ volatile("csrw pmpaddr8, %0" : : "r"(addr8));
  uint32_t c2;
  __asm__ volatile("csrr %0, pmpcfg2" : "=r"(c2));
  c2 = (c2 & ~0xffu) | cfg8;
  __asm__ volatile("csrw pmpcfg2, %0" : : "r"(c2));
  __asm__ volatile("fence.i" ::: "memory");
}

// The snippet's instructions from `off` to its end without X; entered at its start.
static void exec_case(eg2_tally_t *t, uint32_t idx, uint32_t mode, uint32_t off, bool locked,
                      bool umode) {
  uint32_t s = EG2_ADDR(eg2_snip_nops);
  uint32_t lo = s + off, hi = s + 64;
  uint32_t addr7 = lo >> 2, addr8;
  if (mode == kPmpTor) {
    addr8 = hi >> 2;
  } else if (mode == kPmpNa4) {
    addr8 = lo >> 2;
    hi = lo + 4;
  } else {
    lo = s;  // NAPOT: the whole 64-byte snippet
    addr8 = (s >> 2) | 7;
  }
  pmp_set(mode | kPmpR | (locked ? kPmpL : 0u), addr7, addr8);
  bool enforced = locked || umode;
  uint32_t mcause = umode ? kEg2McauseEcallU : kEg2McauseEcallM;
  uint32_t pc = s + 60;
  uint32_t mtval = 0;
  if (enforced) {
    mcause = kEg2McauseFetchFault;  // the first instruction in the region
    pc = lo;
    mtval = lo;
  }
#ifdef EG2_RAND_TRAPS_INJECT_FAIL
  if (!injected && mcause == kEg2McauseFetchFault) {
    mcause = kEg2McauseEcallM;
    injected = true;
  }
#endif
  bool trapped = umode ? eg2_probe_call_umode(NULL, NULL, eg2_snip_nops)
                       : eg2_probe_jump(NULL, NULL, eg2_snip_nops, kEg2JumpCall);
  pmp_set(0, 0, 0);
  check_trap(t, "epmp exec", idx, trapped, mcause, mtval, pc);
}

static void phase_exec(eg2_tally_t *t) {
  uint32_t idx = 0;
  static const uint8_t kModes[3] = {kPmpTor, kPmpNa4, kPmpNapot};
  for (uint32_t m = 0; m < 3; ++m) {
    for (uint32_t off = 0; off < 64; off += 4) {
      if (kModes[m] == kPmpNapot && off != 0) {
        continue;
      }
      for (uint32_t l = 0; l < 2; ++l) {
        for (uint32_t u = 0; u < 2; ++u) {
          exec_case(t, idx++, kModes[m], off, l != 0, u != 0);
        }
      }
    }
    ++t->done;
  }
}

enum { kEvIllegal, kEvEpmp, kEvIrq, kEvCount };
static const uint8_t kEvDefault[kEvCount] = {20, 15, 30};

static void rand_event(eg2_tally_t *t, eg2_family_t *ev, uint32_t n) {
  switch (eg2_family_pick(ev)) {
    case kEvIllegal:
      illegal_event(t, eg2_pick(kIllegalCount), n);
      break;
    case kEvEpmp: {
      static const uint8_t kModes[3] = {kPmpTor, kPmpNa4, kPmpNapot};
      exec_case(t, n, kModes[eg2_pick(3)], 4 * eg2_pick(16), eg2_rand_range(0, 1) != 0,
                eg2_rand_range(0, 1) != 0);
      break;
    }
    default:
      irq_event(t, eg2_pick(3), false, n);
      break;
  }
}

#endif  // __CHERIOT__

static void phase_random(eg2_tally_t *t) {
  eg2_family_t ev = {.n = kEvCount, .plain_mask = 0, .boosted = kEvCount};
  for (uint32_t e = 0; e < kEvCount; ++e) {
    ev.w[e] = eg2_rand_range(kEvDefault[e] / 2u, kEvDefault[e] * 3u / 2u);
  }
  uint32_t b = EG2_PARAM(boost);
  if (b != 0 && b != kEg2BoostNone) {
    uint32_t k = (b - 1) % kEvCount, others = 0;
    for (uint32_t e = 0; e < kEvCount; ++e) {
      others += e == k ? 0 : ev.w[e];
    }
    ev.w[k] = others;
    ev.boosted = k;
  }
  eg2_family_print("event", &ev);
  uint32_t n = EG2_PARAM(n_events) != 0 ? EG2_PARAM(n_events) : eg2_rand_range(64, 1024);
  t->planned = n;
  for (uint32_t i = 0; i < n; ++i) {
    rand_event(t, &ev, i);
    ++t->done;
  }
}

static void merge(eg2_tally_t *total, const char *name, const eg2_tally_t *t) {
  eg2_tally_print(name, t);
  total->checks += t->checks;
  total->failed += t->failed;
  total->planned += t->planned;
  total->done += t->done;
}

bool eg2_test_main(void) {
#ifdef __CHERIOT__
  gaps = EG2_PARAM(model_gaps) != 0;
  eg2_cp_save_sealing_root();
#endif
  if (!eg2_sec_sram_init()) {
    eg2_puts("FAIL: no main SRAM arena\r\n");
    return false;
  }
  eg2_probe_begin();
  eg2_tally_t total = {0};
#ifdef __CHERIOT__
  eg2_tally_t t1 = {.planned = kSnippetCount};
  phase_pcc_edges(&t1);
  merge(&total, "pcc edges", &t1);
  eg2_tally_t t2 = {.planned = 9};
  phase_sentries(&t2);
  merge(&total, "sentries", &t2);
  eg2_tally_t t3 = {.planned = 1};
  phase_sr(&t3);
  merge(&total, "sr", &t3);
#else
  eg2_tally_t t1 = {.planned = 3};
  phase_exec(&t1);
  merge(&total, "epmp exec", &t1);
#endif
  eg2_tally_t t4 = {.planned = 1};
  phase_illegal(&t4);
  merge(&total, "illegal", &t4);
  eg2_tally_t t5 = {.planned = 4};
  phase_irqs(&t5);
  merge(&total, "irqs", &t5);
  eg2_tally_t t6 = {0};
  phase_random(&t6);
  merge(&total, "random", &t6);
  eg2_probe_end();
  eg2_put_kv("EG2 traps taken", eg2_trap_rec.count);
  eg2_put_kv(" interrupts", eg2_trap_rec.irq_count[0] + eg2_trap_rec.irq_count[1] +
                                eg2_trap_rec.irq_count[2]);
  eg2_puts("\r\n");
  if (gaps) {
    eg2_tally_print("model gaps (not counted)", &gap_tally);
  }
  eg2_tally_print("total", &total);
  return eg2_tally_ok(&total);
}
