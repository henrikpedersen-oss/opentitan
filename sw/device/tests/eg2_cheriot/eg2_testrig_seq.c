// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_testrig_seq (EG2 spec "TestRIG streams in the chip"; REQ_CIB_06). CHERIoT mode.
 *
 * TestRIG chooses the instructions, the bench everything around them, and the scoreboard checks
 * every instruction as in every other test. The streams are QuickCheckVEngine's (every CHERIoT
 * generator but `interrupt`), laid out at the PCs CHERIoT-Sail ran them at (eg2_testrig_pool.h,
 * scripts/testrig_pool.py), each ending in an `ebreak`.
 *
 * Knobs: `n_streams` drawn from the pool by `w_gen_<g>`, packed four to a knob (`w_gens_<k>`, byte
 * g % 4 of knob g / 4, each 1-100: one knob per generator would take the image's globals past 4 KiB,
 * F21); `code_region` (main SRAM, sec SRAM, the
 * RRAM area in place, programmed through rram_ctrl); interrupts `w_irq_<s>` (none, rv_timer, the
 * PLIC's software interrupt, aon_timer, a GPIO edge the bench drives) every `irq_period` cycles;
 * `sweep_during` a TBRE sweep over the sandbox while a stream runs; `icache`, `dummy_instr`.
 *
 * The harness, per stream: the sandbox zeroed (its tags cleared), the stream's words placed,
 * `fence.i`, the drawn interrupt armed, then eg2_testrig_run enters it with the state after a DII
 * reset: x1-x15 zero, PCC the executable root at the sandbox (without SR, so that a stream cannot
 * redirect the harness's trap vector; a stream's first SCR or privileged CSR access then ends
 * it), MTDC and MScratchC the roots. A stream ends at its end marker (completed) or at another
 * trap (counted, and the harness goes on: QCVEngine's data accesses are at CHERIoT-Sail's RAM,
 * 0x8000_0000, unmapped in Earl Grey 2, and its trap handlers at Sail's mtvec).
 *
 * Check: the scoreboard on every instruction (interrupts as input events); liveness: every drawn
 * stream returned, and a completed stream retired at least the instructions Sail ran.
 *
 * A failure names its stream (`EG2 testrig stream <generator>/<file>`); the file replays at core
 * level (`make testrig-xlm-replay FAILURE=<file>`, ibex_cheriot_verification).
 *
 * Built with EG2_TESTRIG_SEQ_INJECT_FAIL, one more stream is planned than run: the run must fail
 * with `EG2 testrig_seq: <n>/<n+1> done`.
 *
 * `model_gaps` (eg2sim only; 0 on the chip): the bench's GPIO edges are not asked for (no bench);
 * no interrupt is armed (MPACT-CHERIoT takes an interrupt with MIE 0, inside the trap handler,
 * which then faults on its own MScratchC); and each stream runs at a 1 KiB slot of main SRAM no
 * earlier stream used, at most 16 streams (MPACT-CHERIoT runs stale code after a store over code
 * it ran, even after `fence.i`: status/dv_todo.md M3). The checks are unchanged.
 */

#include "hw/top/gpio_regs.h"
#include "hw/top/rv_plic_regs.h"
#include "hw/top/rv_timer_regs.h"
#include "hw/top/sram_ctrl_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_testrig_pool.h"

EG2_TEST_CONFIG();

#define TESTRIG_SEQ_PARAMS(X)        \
  X(n_streams, 0, 0, 256)            \
  X(w_gens_0, 0x0a0a0a0a, 0, 0xffffffff) \
  X(w_gens_1, 0x0a0a0a0a, 0, 0xffffffff) \
  X(w_gens_2, 0x0a0a0a0a, 0, 0xffffffff) \
  X(w_gens_3, 0x0a0a0a0a, 0, 0xffffffff) \
  X(w_gens_4, 0x0a0a0a0a, 0, 0xffffffff) \
  X(code_region, 0, 0, 3)            \
  X(icache, 2, 0, 2)                 \
  X(w_irq_none, 40, 0, 100)          \
  X(w_irq_rv_timer, 15, 0, 100)      \
  X(w_irq_plic_sw, 15, 0, 100)       \
  X(w_irq_aon_timer, 15, 0, 100)     \
  X(w_irq_gpio, 15, 0, 100)          \
  X(irq_period, 0, 0, 5000)          \
  X(sweep_during, 2, 0, 2)           \
  X(dummy_instr, 2, 0, 2)            \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TESTRIG_SEQ_PARAMS);

_Static_assert(EG2_TR_GENERATORS == 20, "the knobs name the pool's 20 generators");

void eg2_testrig_run(void *entry, void *save);

/** The save area's address, for the stream's return path (eg2_testrig_run.S). */
volatile uint32_t eg2_testrig_save_addr;

enum {
  kSave = kEg2SecBase + kEg2SecSize - 64,
  kSandboxSize = 0x1000,
  // model_gaps: one stream's slot (a stream spans at most 1 KiB) and the streams run.
  kSlot = 0x400,
  kGapStreams = 16,
  kIrqNone = 0,
  kIrqTimer,
  kIrqPlicSw,
  kIrqAon,
  kIrqGpio,
  kIrqCount,
  kPlicIdAon = 158,
  kPlicIdGpio0 = 37,
};

static eg2_tally_t tally;
// The bytes zeroed before a stream and swept during it: kSandboxSize, or kSlot (model_gaps).
static uint32_t sandbox_size;

static uint32_t gen_weight(uint32_t g) {
  uint32_t w;
  switch (g / 4) {
    case 0: w = EG2_PARAM(w_gens_0); break;
    case 1: w = EG2_PARAM(w_gens_1); break;
    case 2: w = EG2_PARAM(w_gens_2); break;
    case 3: w = EG2_PARAM(w_gens_3); break;
    default: w = EG2_PARAM(w_gens_4); break;
  }
  return (w >> (8 * (g % 4))) & 0xff;
}

/** A stream of the drawn generator (the pool's streams of generators with weight). */
static uint32_t draw_stream(void) {
  uint32_t total = 0;
  for (uint32_t s = 0; s < EG2_TR_STREAMS; ++s) {
    total += gen_weight(kEg2TrStreams[s].gen);
  }
  uint32_t r = eg2_rand_range(0, total - 1);
  for (uint32_t s = 0; s < EG2_TR_STREAMS; ++s) {
    uint32_t w = gen_weight(kEg2TrStreams[s].gen);
    if (r < w) {
      return s;
    }
    r -= w;
  }
  return EG2_TR_STREAMS - 1;
}

static uint32_t sandbox_base(uint32_t region) {
  switch (region) {
    case 1:
      return (eg2_sym_bss_end() + 0xfff) & ~0xfffu;
    case 2:
      return kEg2SecBase;
    default:
      return kEg2MsRramArea;
  }
}

/** The stream's words at the sandbox (RRAM: programmed through the controller). */
static bool place(uint32_t s, uint32_t base, bool rram) {
  const eg2_tr_stream_t *st = &kEg2TrStreams[s];
  uint32_t span = (st->span + 15u) & ~15u;
  if (rram) {
    // Built a 16-byte RRAM word at a time in the work area, then programmed.
    for (uint32_t w = 0; w < span; w += 16) {
      uint32_t buf[4] = {0, 0, 0, 0};
      for (uint32_t i = 0; i < st->count; ++i) {
        uint32_t off = kEg2TrWords[st->first + i][0] & 0xffff;
        bool half = (kEg2TrWords[st->first + i][0] >> 16) & 1;
        uint32_t word = kEg2TrWords[st->first + i][1];
        for (uint32_t b = 0; b < (half ? 2u : 4u); ++b) {
          uint32_t a = off + b;
          if (a >= w && a < w + 16) {
            uint32_t byte = (word >> (8 * b)) & 0xff;
            buf[(a - w) / 4] |= byte << (8 * ((a - w) % 4));
          }
        }
      }
      uint32_t err;
      if (!eg2_ms_rram_program(base + w, buf, 4, &err)) {
        return false;
      }
    }
    return true;
  }
  volatile uint32_t *z = eg2_mem(base, sandbox_size);
  for (uint32_t i = 0; i < sandbox_size / 4; ++i) {
    z[i] = 0;
  }
  for (uint32_t i = 0; i < st->count; ++i) {
    uint32_t off = kEg2TrWords[st->first + i][0] & 0xffff;
    bool half = (kEg2TrWords[st->first + i][0] >> 16) & 1;
    uint32_t word = kEg2TrWords[st->first + i][1];
    volatile uint16_t *h = eg2_mem(base + off, half ? 2 : 4);
    h[0] = (uint16_t)word;
    if (!half) {
      h[1] = (uint16_t)(word >> 16);
    }
  }
  return true;
}

static void arm_irq(uint32_t kind, uint32_t period) {
  switch (kind) {
    case kIrqTimer:
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CFG0_REG_OFFSET, 1u << RV_TIMER_CFG0_STEP_OFFSET);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, period / 4 + 1);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 1);
      __asm__ volatile("csrs mie, %0" : : "r"(1u << 7));
      break;
    case kIrqPlicSw:
      eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_MSIP0_REG_OFFSET, 1);
      __asm__ volatile("csrs mie, %0" : : "r"(1u << 3));
      break;
    case kIrqAon:
      eg2_dev_write32(kEg2DevAonTimer, 0x4, 0);              // WKUP_CTRL off
      eg2_dev_write32(kEg2DevAonTimer, 0x14, 0);             // WKUP_COUNT_LO
      eg2_dev_write32(kEg2DevAonTimer, 0xc, period / 500 + 1);  // WKUP_THOLD_LO (aon ticks)
      eg2_dev_write32(kEg2DevAonTimer, 0x8, 0);              // WKUP_THOLD_HI
      eg2_ms_plic_enable(kPlicIdAon, true);
      eg2_dev_write32(kEg2DevAonTimer, 0x4, 1);              // WKUP_CTRL on
      __asm__ volatile("csrs mie, %0" : : "r"(1u << 11));
      break;
    case kIrqGpio:
      // Rising edges on every GPIO input interrupt (through the PLIC); the bench drives the edges
      // (chip_sw_eg2_testrig_seq_vseq) on the pads the test ROM routes to GPIO.
      eg2_dev_write32(kEg2DevGpio, GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);
      eg2_dev_write32(kEg2DevGpio, GPIO_INTR_CTRL_EN_RISING_REG_OFFSET, 0xffffffffu);
      eg2_dev_write32(kEg2DevGpio, GPIO_INTR_ENABLE_REG_OFFSET, 0xffffffffu);
      for (uint32_t id = kPlicIdGpio0; id < kPlicIdGpio0 + 32; ++id) {
        eg2_ms_plic_enable(id, true);
      }
      __asm__ volatile("csrs mie, %0" : : "r"(1u << 11));
      (void)eg2_ms_bench("gpio_edges", period, 0, EG2_PARAM(model_gaps) == 0);
      break;
    default:
      break;
  }
}

static void disarm_irq(void) {
  __asm__ volatile("csrc mie, %0" : : "r"((1u << 3) | (1u << 7) | (1u << 11)));
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_MSIP0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, 0x4, 0);
  eg2_dev_write32(kEg2DevAonTimer, 0x2c, 0x3);  // INTR_STATE: both cleared
  for (uint32_t id = eg2_ms_plic_claim(); id != 0; id = eg2_ms_plic_claim()) {
    eg2_ms_plic_complete(id);
  }
  eg2_ms_plic_enable(kPlicIdAon, false);
  eg2_dev_write32(kEg2DevGpio, GPIO_INTR_ENABLE_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevGpio, GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);
  for (uint32_t id = kPlicIdGpio0; id < kPlicIdGpio0 + 32; ++id) {
    eg2_ms_plic_enable(id, false);
  }
}

bool eg2_test_main(void) {
  bool gaps = EG2_PARAM(model_gaps) != 0;
  sandbox_size = kSandboxSize;
  if (!eg2_sec_sram_init() || !eg2_ms_rram_setup()) {
    return false;
  }
  for (uint32_t g = 0; g < EG2_TR_GENERATORS; ++g) {
    uint32_t w = gen_weight(g);
    if (w < 1 || w > 100) {
      eg2_put_kv("EG2 generator weight out of range [1, 100], generator", g);
      eg2_puts("\r\n");
      return false;
    }
  }
  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, 0x6);
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, 0x6);
  uint32_t ctl;
  __asm__ volatile("csrr %0, 0x7c0" : "=r"(ctl));
  if (EG2_PARAM(icache) != 2) {
    ctl = (ctl & ~1u) | EG2_PARAM(icache);
  }
  uint32_t di = EG2_PARAM(dummy_instr) != 2 ? EG2_PARAM(dummy_instr) : eg2_pick(2);
  ctl = (ctl & ~4u) | (di << 2);
  __asm__ volatile("csrw 0x7c0, %0" : : "r"(ctl));

  uint32_t n = EG2_PARAM(n_streams) != 0 ? EG2_PARAM(n_streams) : eg2_rand_range(16, 256);
  uint32_t region = EG2_PARAM(code_region);
  if (region == 0) {
    uint32_t d = eg2_rand_range(0, 3);
    region = d < 2 ? 1 : d == 2 ? 2 : 3;
  }
  uint32_t period = EG2_PARAM(irq_period) != 0 ? EG2_PARAM(irq_period) : 0;
  bool sweep = EG2_PARAM(sweep_during) != 2 ? EG2_PARAM(sweep_during) != 0 : eg2_rand_range(0, 9) < 3;
  eg2_family_t irqs = {.n = kIrqCount, .plain_mask = 0, .boosted = kIrqCount};
  irqs.w[kIrqNone] = EG2_PARAM(w_irq_none);
  irqs.w[kIrqTimer] = gaps ? 0 : EG2_PARAM(w_irq_rv_timer);
  irqs.w[kIrqPlicSw] = gaps ? 0 : EG2_PARAM(w_irq_plic_sw);
  irqs.w[kIrqAon] = gaps ? 0 : EG2_PARAM(w_irq_aon_timer);
  irqs.w[kIrqGpio] = gaps ? 0 : EG2_PARAM(w_irq_gpio);
  if (gaps) {
    irqs.w[kIrqNone] = 1;  // model_gaps: no interrupts (the header)
  }
  eg2_family_print("irq", &irqs);
  if (gaps) {
    // model_gaps: a fresh slot of main SRAM per stream (the header).
    region = 1;
    uint32_t top = eg2_main_arena();
    uint32_t room = top > sandbox_base(1) ? (top - sandbox_base(1)) / kSlot : 0;
    sandbox_size = kSlot;
    n = n < room ? n : room;
    n = n < kGapStreams ? n : kGapStreams;
  }
  uint32_t base0 = sandbox_base(region);
  uint32_t base = base0;
  eg2_put_kv("EG2 testrig_seq streams", n);
  eg2_put_kv(" region", region);
  eg2_put_kv(" sandbox", base);
  eg2_puts(sweep ? " sweeps during the streams\r\n" : "\r\n");
  eg2_testrig_save_addr = kSave;

  uint32_t completed = 0, trapped = 0;
  eg2_probe_begin();
  for (uint32_t k = 0; k < n; ++k) {
    ++tally.planned;
    uint32_t s = draw_stream();
    const eg2_tr_stream_t *st = &kEg2TrStreams[s];
    if (gaps) {
      base = base0 + k * kSlot;
    }
    eg2_puts("EG2 testrig stream ");
    eg2_putdec(k);
    eg2_puts(" ");
    eg2_puts(kEg2TrNames + kEg2TrNameAt[st->gen]);
    eg2_puts("/");
    eg2_puts(kEg2TrFiles + st->file);
    eg2_puts("\r\n");
    if (!place(s, base, region == 3)) {
      eg2_puts("EG2 RRAM programming failed\r\n");
      break;
    }
    __asm__ volatile("fence.i" ::: "memory");
    void *entry;
    __asm__ volatile("auipcc %0, 0" : "=C"(entry));
    __asm__ volatile("candperm %0, %0, %1" : "+C"(entry) : "r"(~(uint32_t)kEg2PermSr));
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(entry) : "r"(base));
    if (k == 0) {
      uint32_t perm;
      __asm__ volatile("cgetperm %0, %1" : "=r"(perm) : "C"(entry));
      eg2_put_kv("EG2 testrig entry permissions", perm);
      eg2_puts("\r\n");
    }
    uint32_t irq = eg2_family_pick(&irqs);
    uint32_t p = period != 0 ? period : eg2_rand_range(50, 5000);
    arm_irq(irq, p);
    if (sweep && region != 3) {
      eg2_ms_sweep_t sw;
      eg2_ms_tbre_start(base, sandbox_size / 8, &sw);
    }
    uint32_t i0, i1;
    __asm__ volatile("csrr %0, minstret" : "=r"(i0));
    eg2_testrig_run(entry, eg2_mem(kSave, 48));
    __asm__ volatile("csrr %0, minstret" : "=r"(i1));
    disarm_irq();
    if (sweep && region != 3) {
      eg2_ms_sweep_t sw = {0};
      eg2_ms_tbre_wait(&sw, 1000000);
      eg2_ms_tbre_clear();
    }
    uint32_t at = eg2_trap_rec.mepcc.p ? EG2_ADDR(eg2_trap_rec.mepcc.p) : 0;
    // Completed: at the end marker, or trapping at the instruction Sail trapped at.
    bool done_ok = at == base + st->end &&
                   (st->endtrap || eg2_trap_rec.mcause == kEg2McauseBreakpoint);
    if (done_ok) {
      ++completed;
      eg2_check(&tally, "instructions retired >= Sail's", k, i1 - i0 >= st->ran ? 1 : 0, 1);
    } else {
      if (trapped < 8) {
        eg2_put_kv("  ended by mcause", eg2_trap_rec.mcause);
        eg2_put_kv(" mtval", eg2_trap_rec.mtval);
        eg2_put_kv(" at", at);
        eg2_put_kv(" (end marker at", base + st->end);
        eg2_puts(")\r\n");
      }
      ++trapped;
    }
    ++tally.done;
  }
  eg2_probe_end();
#ifdef EG2_TESTRIG_SEQ_INJECT_FAIL
  ++tally.planned;
#endif
  eg2_put_kv("EG2 testrig_seq completed", completed);
  eg2_put_kv(" ended by a trap", trapped);
  eg2_puts("\r\n");
  eg2_tally_print("testrig_seq", &tally);
  return eg2_tally_ok(&tally);
}
