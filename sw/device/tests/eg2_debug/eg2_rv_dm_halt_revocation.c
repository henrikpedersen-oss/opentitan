// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_halt_revocation (EG2 verification spec REQ_CDM_08, REQ_CMS_12; "Debug
 * Controller" / "Corner cases"), CHERIoT mode only.
 *
 * Phases (commands of eg2_dm_corners.h's protocol, kind kEg2DmAction, arg = the phase; the bench's
 * part is env/seq_lib/chip_eg2_rv_dm_halt_revocation_vseq.sv). The oracle is eg2_revoke.h's
 * expression (a capability based in a marked granule loads untagged), and the revocation engine
 * as cheriot_mem_sys theory_of_operation.md states it (the epoch advances by 2 per sweep,
 * `tbre_done` when the engine stops).
 *   0 A sweep over main and sec SRAM, `tbre_done` enabled through rv_plic with mstatus.MIE set,
 *     64 slots in sec SRAM's arena B pointing at objects in arena A, every other object marked.
 *     The core waits; the bench halts it while TBRE_STATUS.busy is 1, sees the sweep finish and
 *     the PLIC source pending while the core stays halted (RISC-V Debug Specification 4.1:
 *     interrupts are not taken in debug mode), and resumes: the interrupt is the first thing the
 *     core takes after resume. Here: taken exactly once, the claim is tbre_done's source, the
 *     epoch advanced by 2, marked slots lost their tags in memory and the others kept them.
 *   1 The core loads a marked and an unmarked capability in a loop (eg2_dmx_clc_loop); the bench
 *     halts it at drawn moments, aiming at a CLC whose bitmap read is outstanding. Here: every
 *     CLC's tag followed the bitmap (0 for the marked, 1 for the other), across the halts.
 *   2 The bench single-steps eg2_dmx_step_clc over three CLCs (marked, unmarked, marked) and reads
 *     each loaded tag from the program buffer (0, 1, 0). Here: the three capabilities, stored
 *     after the steps, have the same tags.
 *   3 The core waits; the bench, with the core halted, sets the bitmap bit of an object's granule
 *     with an SBA write to the revbm window (published below), and resumes. Here: the next CLC of
 *     the slot pointing at it loads tag 0.
 *
 * Published in eg2_dm_corners's scratch words (the bench reads them over SBA): word 0 the stage's
 * wait-loop address (eg2_dm_cap.h); phase 0: words 1-4 TBRE_STATUS's address and busy mask, the
 * PLIC pending word's address and tbre_done's mask; phase 3: words 1-2 the bitmap word's address
 * and the bit's mask.
 *
 * EXPECTED FAILURE until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT input, so every
 * halt in CHERIoT mode runs the base debug ROM.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_gaps.
 *
 * | Skipped with model_gaps = 1                       | Why                                     |
 * |---------------------------------------------------|-----------------------------------------|
 * | the bench's halts, steps, SBA write and done word | eg2sim has no debug module, JTAG or SBA |
 * | phase 0's revoked tags                            | the model's engine sweeps without        |
 * |                                                   | revoking (eg2_tbre_sweep_irq.c)          |
 * | phase 0: tbre_done taken as an interrupt (the     | MPACT takes an interrupt with MIE 0 in  |
 * |   PLIC pending bit is checked instead)            | the probe's handler (eg2_rand_traps.c)  |
 * | phases 1-2: the marked capabilities' tags         | eg2sim has no load filter               |
 * | phase 3's tag after the SBA mark                  | no SBA write without the bench          |
 *
 * Built with EG2_RV_DM_HALT_REVOCATION_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `DEBUG FAIL`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"  // Generated.
#include "hw/top/rv_plic_regs.h"          // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"
#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define HALT_REVOCATION_PARAMS(X)            \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(HALT_REVOCATION_PARAMS)

// Shared with chip_eg2_rv_dm_halt_revocation_vseq.sv.
enum {
  kPhSweep = 0,
  kPhClcLoop = 1,
  kPhStep = 2,
  kPhSbaMark = 3,
  kPhCount = 4,
  kSlots = 64,
  kObjects = kEg2ArenaA,
  kSlotBase = kEg2ArenaB,
  kMipMeip = 1u << 11,
  kIrqExternal = 2,  // eg2_trap_rec.irq_count index
};

// The CLC phases' objects and slots (main SRAM, in the revocable window), reached only by address
// through eg2_ms_* (F24).
static uint64_t objs_[3];
static uint64_t slots_[3];

static bool model_gaps(void) { return EG2_PARAM(model_gaps) != 0; }

static bool run_stage(eg2_dmx_ctx_t *ctx, char *stage, void *op1) {
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  return eg2_probe_call(ctx, op1, NULL, stage);
}

static uint32_t obj_addr(uint32_t i) { return eg2_chk_addr_of(objs_) + 8 * i; }
static uint32_t slot_addr(uint32_t i) { return eg2_chk_addr_of(slots_) + 8 * i; }

// Slot i holds a capability to object i; marked as asked. Returns the slot's authority.
static void *prepare_slot(uint32_t i, bool marked) {
  eg2_ms_revbm_mark(obj_addr(i), false);
  eg2_ms_csc(slot_addr(i), eg2_ms_object_cap(obj_addr(i), 8, false));
  eg2_ms_revbm_mark(obj_addr(i), marked);
  return eg2_ms_auth(slot_addr(i));
}

static bool phase_sweep(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  for (uint32_t i = 0; i < kSlots; ++i) {
    eg2_ms_csc(kSlotBase + 8 * i, eg2_ms_object_cap(kObjects + 8 * i, 8, false));
  }
  for (uint32_t i = 0; i < kSlots; i += 2) {
    eg2_ms_revbm_mark(kObjects + 8 * i, true);
  }
  volatile uint32_t *s = eg2_dm_scratch();
  s[kEg2DmxScratchArg0] =
      TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR + CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET;
  s[kEg2DmxScratchArg1] = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT;
  s[kEg2DmxScratchArg2] =
      TOP_EARLGREY_RV_PLIC_BASE_ADDR + RV_PLIC_IP_0_REG_OFFSET + 4 * (kEg2MsTbreIrq / 32);
  s[kEg2DmxScratchArg3] = 1u << (kEg2MsTbreIrq % 32);

  eg2_ms_plic_enable(kEg2MsTbreIrq, true);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_ENABLE_TBRE_DONE_BIT);
  uint32_t irqs0 = eg2_trap_rec.irq_count[kIrqExternal];
  // eg2sim: mie.MEIE stays clear (MPACT takes an interrupt even with mstatus.MIE 0).
  uint32_t mie_old;
  __asm__ volatile("csrrs %0, mie, %1" : "=r"(mie_old) : "r"(model_gaps() ? 0u : kMipMeip));
  eg2_ms_sweep_t sw;
  // The whole revocable window: long enough for the bench to see it busy and halt the core.
  eg2_ms_tbre_start(kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8, &sw);
  ok = eg2_chk_eq("sweep start taken", 1, sw.started ? 1 : 0) && ok;
  bool trapped;
  if (!model_gaps()) {
    eg2_chk_irq_on();
    trapped = run_stage(ctx, eg2_dmx_park, NULL);
    eg2_chk_irq_off();
  } else {
    // eg2sim: no halt, and no interrupt taken (MPACT takes it with MIE 0 inside the probe's
    // handler, eg2_rand_traps.c): the pending bit is polled instead.
    trapped = run_stage(ctx, eg2_dmx_park, NULL);
    for (uint32_t i = 0; i < 10000000 && !eg2_ms_plic_pending(kEg2MsTbreIrq); ++i) {
    }
  }
  ok = eg2_chk_true("no exception while waiting", !trapped) && ok;
  if (model_gaps()) {
    ok = eg2_chk_true("tbre_done pending at the PLIC", eg2_ms_plic_pending(kEg2MsTbreIrq)) && ok;
    eg2_chk_skip("phase 0: tbre_done taken as an interrupt (MPACT's interrupt entry)");
  } else {
    ok = eg2_chk_eq("tbre_done taken once (after resume)", irqs0 + 1,
                    eg2_trap_rec.irq_count[kIrqExternal]) &&
         ok;
  }
  uint32_t id = eg2_ms_plic_claim();
  ok = eg2_chk_eq("PLIC claim: tbre_done's source", kEg2MsTbreIrq, id) && ok;
  eg2_ms_plic_complete(id);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_STATE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_STATE_TBRE_DONE_BIT);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET, 0);
  eg2_ms_plic_enable(kEg2MsTbreIrq, false);
  __asm__ volatile("csrw mie, %0" : : "r"(mie_old));
  eg2_ms_tbre_wait(&sw, 10);
  ok = eg2_chk_eq("engine idle at the interrupt", 1, sw.done ? 1 : 0) && ok;
  ok = eg2_chk_eq("epoch advanced by one sweep", sw.epoch_before + 2, sw.epoch_after) && ok;
  // The bitmap cleared, so the reload sees what the sweep left in memory.
  eg2_ms_revbm_clear();
  for (uint32_t i = 0; i < kSlots; ++i) {
    bool revoked = (i % 2) == 0;
    if (revoked && model_gaps()) {
      continue;
    }
    if (!eg2_chk_eq(revoked ? "swept slot of a marked object: tag" : "swept slot: tag",
                    revoked ? 0 : 1, eg2_ms_tag(kSlotBase + 8 * i))) {
      ok = false;
      break;
    }
  }
  if (model_gaps()) {
    eg2_chk_skip("phase 0: the revoked slots' tags (the model's engine does not revoke)");
  }
  return ok;
}

static bool phase_clc_loop(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  ctx->auth_mark.p = prepare_slot(0, true);
  ctx->auth_clean.p = prepare_slot(1, false);
  bool trapped = run_stage(ctx, eg2_dmx_clc_loop, NULL);
  ok = eg2_chk_true("no exception in the CLC loop", !trapped) && ok;
  eg2_chk_step("CLC loop iterations", ctx->n);
  ok = eg2_chk_true("CLC loop ran", ctx->n >= 1) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phase 1: the marked capability's tag (no load filter in eg2sim)");
  } else {
    ok = eg2_chk_eq("CLCs whose tag disagreed with the bitmap, across the halts", 0, ctx->errs) &&
         ok;
  }
  eg2_ms_revbm_mark(obj_addr(0), false);
  return ok;
}

static bool phase_step(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  ctx->auth_mark.p = prepare_slot(0, true);
  ctx->auth_clean.p = prepare_slot(1, false);
  // t1's first value: 0 holds the stage in its wait for the bench; eg2sim starts it at once.
  bool trapped = run_stage(ctx, eg2_dmx_step_clc, (void *)(uintptr_t)(model_gaps() ? 1 : 0));
  ok = eg2_chk_true("no exception in the stepped CLCs", !trapped) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phase 2: the marked capabilities' tags (no load filter in eg2sim)");
  } else {
    ok = eg2_chk_eq("stepped CLC of the marked capability: tag", 0,
                    eg2_ms_cap_tag(ctx->out[0].p)) &&
         ok;
    ok = eg2_chk_eq("stepped CLC of the marked capability again: tag", 0,
                    eg2_ms_cap_tag(ctx->out[2].p)) &&
         ok;
  }
  ok = eg2_chk_eq("stepped CLC of the unmarked capability: tag", 1,
                  eg2_ms_cap_tag(ctx->out[1].p)) &&
       ok;
  eg2_ms_revbm_mark(obj_addr(0), false);
  return ok;
}

static bool phase_sba_mark(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  (void)prepare_slot(2, false);
  uint32_t word, bit;
  ok = eg2_chk_true("object in the revocable window", eg2_ms_revbm_bit(obj_addr(2), &word, &bit)) &&
       ok;
  volatile uint32_t *s = eg2_dm_scratch();
  s[kEg2DmxScratchArg0] = kEg2MsMetaBase + 4 * word;
  s[kEg2DmxScratchArg1] = 1u << bit;
  ok = eg2_chk_eq("slot tagged before the halt", 1, eg2_ms_tag(slot_addr(2))) && ok;
  bool trapped = run_stage(ctx, eg2_dmx_park, NULL);
  ok = eg2_chk_true("no exception across the halt", !trapped) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phase 3: the SBA write of the bitmap (no debug module)");
  } else {
    ok = eg2_chk_eq("the next CLC after the SBA mark: tag 0", 0, eg2_ms_tag(slot_addr(2))) && ok;
    ok = eg2_chk_eq("the bitmap bit the SBA write set", 1u << bit,
                    eg2_ms_revbm_read(word) & (1u << bit)) &&
         ok;
  }
  eg2_ms_revbm_mark(obj_addr(2), false);
  return ok;
}

static bool run_phase(uint32_t phase) {
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  bool ok = true;
  eg2_chk_step("phase", phase);
  eg2_probe_begin();
  switch (phase) {
    case kPhSweep:
      ok = phase_sweep(&ctx);
      break;
    case kPhClcLoop:
      ok = phase_clc_loop(&ctx);
      break;
    case kPhStep:
      ok = phase_step(&ctx);
      break;
    case kPhSbaMark:
      ok = phase_sba_mark(&ctx);
      break;
    default:
      ok = eg2_chk_true("phase known", false);
      break;
  }
  eg2_probe_end();
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  if (kind != kEg2DmAction) {
    return eg2_chk_true("action known", false);
  }
  if (!model_gaps()) {
    return run_phase(arg);
  }
  bool ok = true;
  for (uint32_t p = 0; p < kPhCount; ++p) {
    ok = run_phase(p) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  if (!eg2_sec_sram_init()) {
    return eg2_chk_true("sec SRAM initialised", false);
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdHaltRevocation,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = 0,
      .model_actions = 1,
      .wfi_ticks = 20,
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
  bool ok = eg2_dm_serve(&cfg);
  return eg2_chk_summary(8) && ok;
}
