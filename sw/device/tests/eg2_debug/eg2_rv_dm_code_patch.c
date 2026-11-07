// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_code_patch (EG2 verification spec REQ_CDM_08; "Debug Controller" / "Corner
 * cases"), both modes.
 *
 * The debugger rewrites code in main SRAM over SBA while the instruction cache holds it: a changed
 * instruction, or an `ebreak`. The software side, the test's action PATCH (arg: 0 a changed
 * instruction, 1 an ebreak):
 *   1. writes a code slot in main SRAM (.bss): `addi a0, a0, k_old ; ret`, executes `fence.i`,
 *      turns the instruction cache on (cpuctrlsts.icache_enable) and calls the slot `warm_calls`
 *      times (each call must return x + k_old), so the cache holds it;
 *   2. publishes in the scratch area: word 0 the slot's address, word 1 its first word; clears
 *      word 2 (the bench's flag) and prints `EG2 DEBUG armed 0x<slot> 0x<arg>`;
 *   3. waits until the bench has written the patch and set word 2; word 3 is the new increment
 *      k_new the bench drew;
 *   4. calls the slot `stale_calls` times without `fence.i`: each call returns x + k_old or
 *      x + k_new and nothing else (Ibex icache.rst "Cache invalidation" and the integration
 *      notes: without an invalidation the core may run either; the number of each is reported);
 *   5. executes `fence.i` and calls the slot `check_calls` times: each must return x + k_new
 *      (icache.rst: "in order to be completely sure of executing newly fetched code, the core
 *      should raise the icache_inval_i line ... The Ibex core does this in response to a FENCE.I
 *      instruction"); the slot's first word, read as data, is `addi a0, a0, k_new`.
 * With arg 1 the bench wrote `ebreak` with dcsr.ebreakm set: the call that reaches it enters debug
 * mode (the bench checks dcsr.cause = 1 and dpc = the slot), and the bench writes
 * `addi a0, a0, k_new` over SBA and runs `fence.i` from the program buffer before it resumes (the
 * RISC-V Debug Specification 0.13.2: a debugger that writes code executes fence.i), so the call
 * returns x + k_new too. The bench's checks: env/seq_lib/chip_eg2_rv_dm_code_patch_vseq.sv.
 *
 * EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
 * input, so a halt in CHERIoT mode runs the base debug ROM (the ebreak variant; the changed
 * instruction does not halt).
 *
 * Knobs (the vseq writes them):
 *   late_debug_sw  write LATE_DEBUG_ENABLE first
 *   max_iters, idle_polls   as in eg2_rv_dm_halt_corners.c
 *   k_old          the slot's first increment (the bench checks the word over SBA)
 *   warm_calls     calls before the patch (the cache holds the slot)
 *   stale_calls    calls after the patch and before fence.i (old or new code)
 *   check_calls    calls after fence.i (new code)
 *   flag_polls     polls of the bench's flag before the software gives up
 *   model_iters, model_kinds, model_actions as in eg2_rv_dm_sba_corners.c
 *   model_gaps     skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the bench's commands, SBA patch and done     | eg2sim has no debug module, JTAG or SBA: the |
 * | word; the ebreak variant                     | software patches the slot itself (a store),  |
 * |                                              | then the same stale and fence.i calls        |
 *
 * Built with EG2_RV_DM_CODE_PATCH_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `DEBUG FAIL`.
 */

#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

// CHERIoT mode: the boot stage enables execution from main SRAM before it locks sram_ctrl
// (eg2_cheriot_start.S step 0b, REQ_SOC_08); this test runs code there.
const volatile uint8_t eg2_cheriot_boot_sram_exec __attribute__((used)) = 1;

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define PATCH_PARAMS(X)                      \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(k_old, 0x123, 1, 0x3ff)                  \
  X(warm_calls, 4, 1, 64)                    \
  X(stale_calls, 2, 0, 16)                   \
  X(check_calls, 2, 1, 16)                   \
  X(flag_polls, 20000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_kinds, 0x200, 0, 0x3fe)            \
  X(model_actions, 0x1, 0, 0x1)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(PATCH_PARAMS)

enum {
  // Not in eg2_dm_corners.h (a file several tests share): this test's id.
  kEg2DebugIdCodePatch = 0x40,
  kActionPatch = kEg2DmAction,
  kScratchSlot = 0,
  kScratchOld = 1,
  kScratchFlag = 2,
  kScratchKNew = 3,
  kInsnRet = 0x00008067u,  // jalr x0, 0(ra) (cjalr cnull, cra in CHERIoT mode)
  kCsrCpuctrlsts = 0x7c0,
};

uint32_t eg2_dm_patch_call(uint32_t slot, uint32_t x);

// The code slot: main SRAM (.bss), on its own instruction-cache lines.
static volatile uint32_t slot_[16] __attribute__((aligned(64)));

static uint32_t insn_addi_a0(uint32_t k) { return ((k & 0xfffu) << 20) | 0x00050513u; }

static uint32_t cpuctrlsts_read(void) {
  uint32_t v;
  __asm__ volatile("csrr %0, 0x7c0" : "=r"(v));
  return v;
}

static void cpuctrlsts_write(uint32_t v) { __asm__ volatile("csrw 0x7c0, %0" : : "r"(v)); }

static bool patch(uint32_t variant) {
  bool ok = true;
  const bool model = EG2_PARAM(model_gaps) != 0;
  volatile uint32_t *s = eg2_dm_scratch();
  const uint32_t slot = eg2_chk_addr_of(slot_);
  const uint32_t k_old = EG2_PARAM(k_old);
  uint32_t x = 0x1000;

  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMultiBitBool4True);
  slot_[0] = insn_addi_a0(k_old);
  for (uint32_t i = 1; i < 16; ++i) {
    slot_[i] = kInsnRet;
  }
  eg2_chk_fence_i();
  const uint32_t ctl0 = cpuctrlsts_read();
  cpuctrlsts_write(ctl0 | 1u);
  ok = eg2_chk_eq("cpuctrlsts.icache_enable reads 1 (ICache = 1: cs_registers.rst)", 1u,
                  cpuctrlsts_read() & 1u) &&
       ok;

  // 1. The cache holds the slot.
  for (uint32_t i = 0; i < EG2_PARAM(warm_calls); ++i, x += 3) {
    ok = eg2_chk_eq("warm call: the code as written", x + k_old, eg2_dm_patch_call(slot, x)) && ok;
  }

  // 2.-3. The patch: by the bench over SBA, or (eg2sim) by the software.
  uint32_t k_new;
  s[kScratchFlag] = 0;
  s[kScratchKNew] = 0;
  s[kScratchOld] = slot_[0];
  s[kScratchSlot] = slot;
  if (model) {
    eg2_chk_skip("the bench's SBA patch and the ebreak variant (no debug module): a store");
    k_new = (k_old ^ 0x2a5u) & 0x3ffu;
    slot_[0] = insn_addi_a0(k_new);
  } else {
    LOG_INFO("EG2 DEBUG armed 0x%08x 0x%08x", slot, variant);
    uint32_t polls = 0;
    while (s[kScratchFlag] == 0 && polls < EG2_PARAM(flag_polls)) {
      ++polls;
    }
    if (!eg2_chk_true("the bench's patch flag (written over SBA)", s[kScratchFlag] != 0)) {
      cpuctrlsts_write(ctl0);
      return false;
    }
    k_new = s[kScratchKNew];
  }
  eg2_chk_step("k_new", k_new);
  ok = eg2_chk_true("k_new differs from k_old", k_new != k_old) && ok;

  // 4. Before fence.i: the old or the new code, nothing else.
  uint32_t n_old = 0, n_new = 0, n_other = 0;
  for (uint32_t i = 0; i < EG2_PARAM(stale_calls); ++i, x += 3) {
    uint32_t r = eg2_dm_patch_call(slot, x);
    if (r == x + k_old) {
      ++n_old;
    } else if (r == x + k_new) {
      ++n_new;
    } else {
      ++n_other;
    }
  }
  eg2_chk_step("calls before fence.i that ran the old code", n_old);
  eg2_chk_step("calls before fence.i that ran the new code", n_new);
  ok = eg2_chk_eq("calls before fence.i ran the old or the new code", 0, n_other) && ok;

  // 5. After fence.i: the new code.
  eg2_chk_fence_i();
  for (uint32_t i = 0; i < EG2_PARAM(check_calls); ++i, x += 3) {
    ok = eg2_chk_eq("after fence.i the core runs the new code", x + k_new,
                    eg2_dm_patch_call(slot, x)) &&
         ok;
  }
  ok = eg2_chk_eq("the slot's first word read as data", insn_addi_a0(k_new), slot_[0]) && ok;
  cpuctrlsts_write(ctl0);
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  if (kind == kActionPatch) {
    return patch(arg);
  }
  return eg2_chk_true("code_patch action known", false);
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdCodePatch,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = EG2_PARAM(model_kinds),
      .model_actions = EG2_PARAM(model_actions),
      .wfi_ticks = 20,
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
  bool ok = eg2_dm_serve(&cfg);
  return eg2_chk_summary(4) && ok;
}
