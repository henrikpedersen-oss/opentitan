// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_fetch_intg (EG2 verification spec REQ_CIB_07; "CHERIoT Ibex in the Chip"): a word
 * fetched from ROM or from SRAM whose integrity check fails is not executed in any form: neither
 * executed as fetched, nor decoded as another instruction, nor dropped (the instruction after it
 * running instead). The fault is the bench's, on the instruction path: one bit of the TL-UL
 * response to that one fetch at the core's instruction port (rv_core_ibex corei_tl_h_i), the way
 * chip_sw_cheriot_bus_intg corrupts the data port:
 *   field 0  d_user.data_intg (the data's integrity bits),
 *   field 1  d_user.rsp_intg (the response's integrity bits),
 *   field 2  one bit of d_data in the instruction's immediate, its integrity left as it was, so
 *            the word would decode as another `addi`.
 *
 * Documents: Ibex doc security.rst "Bus integrity checking" (incoming data checked against its
 * check bits; an internal interrupt and a major bus alert on a mismatch); rv_core_ibex
 * interfaces.md (fatal_hw_err on alert_major_bus and on a bus integrity error), registers.md
 * (ERR_STATUS.FATAL_INTG_ERR: "rv_core_ibex detected a response integrity error"); the RISC-V
 * privileged ISA (a fetch that fails is an instruction access fault, mcause 1) and Ibex doc
 * cs_registers.rst (mtval is 0 for it). Neither Ibex document says which of the two traps a
 * failed fetch takes: either is accepted, the class is logged.
 *
 * The code: a 32-byte slot (eg2_isa_decode.h's layout) `nop ; nop ; addi a0, a0, K ; ebreak`, run
 * by eg2_isa_harness.S with every register holding D, placed by `source`:
 *   0 ROM: the bench writes the words by backdoor (scrambled, with their integrity) at 0x61000
 *     when the software prints `EG2 FETCH place ...`, and gives them to Spike; the software reads
 *     them back first;
 *   1 sec SRAM, 2 main SRAM (below the stack): the software stores them (EXEC on).
 * `EG2 FETCH arm 0x<addr>` makes the bench corrupt the next response to a fetch of the `addi`'s
 * word (matched by a_source), notify Spike of an instruction fetch error there (RISC-V mode), or
 * announce it to the injection registry (CHERIoT mode: CHERIoT-Sail has no fetch-error input).
 *
 * Checks: the run traps at the `addi` (mepc), with mcause 1 or an internal interrupt (mcause bit
 * 31) and never mcause 2; a0 still D (not executed, as fetched or altered); with mcause 1, mtval
 * 0; rv_core_ibex_fatal_hw_err at the alert handler (declared, so the status sweep checks it
 * arrived); for field 1, ERR_STATUS.FATAL_INTG_ERR (then cleared).
 *
 * Knobs:
 *   source       0 ROM, 1 sec SRAM, 2 main SRAM
 *   field        0 data_intg, 1 rsp_intg, 2 data bit
 *   bit          the bit (0-6 for the integrity fields; 20-31 for the data: the immediate)
 *   imm          K
 *   model_gaps   skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                       | Why                                    |
 * |---------------------------------------------------|----------------------------------------|
 * | the fault and its checks; the ROM source (sec     | no bench and no TL-UL integrity in     |
 * |   SRAM instead): the slot runs and falls through  |   eg2sim                               |
 * |   with a0 = D + K                                 |                                        |
 *
 * Built with EG2_FETCH_INTG_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `FETCH FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/sram_ctrl_regs.h"     // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_cib/eg2_isa_decode.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define FETCH_INTG_PARAMS(X) \
  X(source, 1, 0, 2)         \
  X(field, 0, 0, 2)          \
  X(bit, 0, 0, 31)           \
  X(imm, 0x123, 1, 0x7ff)    \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(FETCH_INTG_PARAMS)

enum {
  // The sweep's layout in sec SRAM (eg2_illegal_sweep.c): the harness's context, D, the areas.
  kSecBase = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
  kCtx = kSecBase + 0x400,
  kDLo = kSecBase + 0x1000,
  kD = kSecBase + 0x2000,
  kDHi = kSecBase + 0x3000,
  kSecSlot = kSecBase + 0x5000,
  kRomSlot = TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR + 0x21000,
  kPad = 0x1000,
  kCtxMcause = 0x80,
  kCtxMepc = 0x84,
  kCtxMtval = 0x88,
  kSrcRom = 0,
  kSrcSec = 1,
  kSrcMain = 2,
  kRegA0 = 10,
};

#ifdef __CHERIOT__
enum { kRegStride = 2 };
#else
enum { kRegStride = 1 };
#endif

void eg2_isa_run(uint32_t slot, uint32_t d, volatile void *ctx);

#define SYM_ADDR_(fn, sym)                                                     \
  static uint32_t fn(void) {                                                   \
    uint32_t a;                                                                \
    __asm__("lui %0, %%hi(" #sym ")\n addi %0, %0, %%lo(" #sym ")" : "=r"(a)); \
    return a;                                                                  \
  }
SYM_ADDR_(sym_stack_start, _stack_start)
SYM_ADDR_(sym_bss_end, _bss_end)

static void fill(uint32_t lo, uint32_t hi, uint32_t v) {
  volatile uint32_t *p = eg2_chk_ptr(lo);
  for (uint32_t i = 0; i < (hi - lo) / 4; ++i) {
    p[i] = v;
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("FETCH");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  const uint32_t field = EG2_PARAM(field);
  uint32_t source = EG2_PARAM(source);
  bool ok = true;
  eg2_chk_step("field", field);

  // Sec SRAM initialised word by word (integrity), D buffer as the sweep has it; EXEC on.
  fill(kSecBase, kSecSlot + kEg2IsaSlotBytes + kPad, 0);
  fill(kDLo, kDHi, kSecBase);
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMultiBitBool4True);
  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMultiBitBool4True);

  if (gaps && source == kSrcRom) {
    eg2_chk_skip("the ROM source (no bench): sec SRAM");
    source = kSrcSec;
  }
  uint32_t slot = kSecSlot;
  if (source == kSrcMain) {
    slot = (sym_stack_start() - 0x2000) & ~0xffu;
    if (!eg2_chk_true("main SRAM below the stack is free", slot - kPad >= sym_bss_end())) {
      return false;
    }
    fill(slot - kPad, slot + kEg2IsaSlotBytes + kPad, 0);
  } else if (source == kSrcRom) {
    slot = kRomSlot;
  }
  eg2_chk_step("source", source);
  eg2_chk_step("slot", slot);

  // nop ; nop ; addi a0, a0, K ; ebreak ; 0...
  const uint32_t imm = EG2_PARAM(imm);
  uint32_t w[kEg2IsaSlotWords] = {kEg2IsaNop, kEg2IsaNop, (imm & 0xfff) << 20 | 0x00050513,
                                  kEg2IsaEbreak, 0, 0, 0, 0};
  const uint32_t target = slot + kEg2IsaTestOff;
  volatile uint32_t *p = eg2_chk_ptr(slot);
  if (source == kSrcRom) {
    LOG_INFO("EG2 FETCH place 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x", slot, w[0], w[1], w[2], w[3]);
    eg2_chk_delay(200);
  } else {
    for (uint32_t i = 0; i < kEg2IsaSlotWords; ++i) {
      p[i] = w[i];
    }
  }
  eg2_chk_fence_i();
  for (uint32_t i = 0; i < kEg2IsaSlotWords; ++i) {
    ok = eg2_chk_eq("slot word read back", w[i], p[i]) && ok;
  }
  if (!ok) {
    return eg2_chk_summary(1) && false;
  }

  int alert = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  if (!gaps) {
    if (!eg2_chk_true("rv_core_ibex_fatal_hw_err in the alert table", alert >= 0)) {
      return false;
    }
    eg2_status_expect_alert((size_t)alert);
    // The bench corrupts the next response to a fetch of `target`.
    LOG_INFO("EG2 FETCH arm 0x%08x", target);
    eg2_chk_delay(200);
  } else {
    eg2_chk_skip("the fault on the fetch, its trap and its alert (no bench)");
  }

  volatile uint32_t *ctx = eg2_chk_ptr(kCtx);
  eg2_isa_run(slot, kD, ctx);
  uint32_t mcause = ctx[kCtxMcause / 4];
  uint32_t mepc = ctx[kCtxMepc / 4];
  uint32_t mtval = ctx[kCtxMtval / 4];
  uint32_t a0 = ctx[kRegA0 * kRegStride];
  eg2_chk_step("mcause", mcause);
  eg2_chk_step("mepc", mepc);
  eg2_chk_step("mtval", mtval);
  eg2_chk_step("a0", a0);

  if (gaps) {
    // The harness and the slot: the addi runs and the slot ends at its ebreak.
    ok = eg2_chk_eq("model: the addi ran (a0)", kD + imm, a0) && ok;
    ok = eg2_chk_eq("model: the slot ended at its ebreak (mepc)", slot + 12, mepc) && ok;
    return eg2_chk_summary(10) && ok;
  }

  // Not dropped: the trap is at the word itself, not after it.
  ok = eg2_chk_eq("the trap is at the corrupted word (mepc)", target, mepc) && ok;
  // Not executed in any form: a0 unchanged (neither + K nor + another immediate).
  ok = eg2_chk_eq("a0 unchanged: the word did not execute", kD, a0) && ok;
  // Not decoded as another instruction; the trap a failed fetch takes.
  bool nmi = (mcause >> 31) != 0;
  ok = eg2_chk_true("the trap is not an illegal instruction", mcause != kEg2ChkCauseIllegal) && ok;
  ok = eg2_chk_true("the trap is an instruction access fault or an internal interrupt",
                    mcause == kEg2ChkCauseFetchFault || nmi) &&
       ok;
  if (mcause == kEg2ChkCauseFetchFault) {
    ok = eg2_chk_eq("mtval of the instruction access fault (cs_registers.rst: 0)", 0, mtval) && ok;
  }
  ok = eg2_chk_true("rv_core_ibex_fatal_hw_err reached the alert handler",
                    eg2_status_wait_alert((size_t)alert, 10000)) &&
       ok;
  uint32_t err = eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  eg2_chk_step("ERR_STATUS", err);
  if (field == 1) {
    ok = eg2_chk_true("ERR_STATUS.FATAL_INTG_ERR (a response integrity error)",
                      (err >> RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT) & 1) &&
         ok;
  }
  // Clear what the fault set (rw1c), so the final sweep sees only what is still wrong.
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, err);
  return eg2_chk_summary(14) && ok;
}
