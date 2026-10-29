// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_exec_en (EG2 verification spec REQ_RRAM_02, REQ_TL_05, "RRAM controller in RISC-V
 * mode"): instruction fetches from RRAM are allowed only while `EXEC` holds its enable value; data
 * reads are not affected.
 *
 * rram_ctrl programmer's guide "Code Execution from RRAM": "software must write the magic value
 * 0xa26a38f7 to the EXEC register. Any other value disables instruction fetches"; hjson EXEC.EN:
 * "A value of 0xa26a38f7 allows RRAM to be used for code execution. Any other value prevents code
 * execution". A refused fetch is answered with `d_error` (spec REQ_TL_05), which the core takes as
 * an instruction access fault: mcause 1, mtval the fetch address (RISC-V privileged spec; the same
 * cause in CHERIoT mode, whose PCC permits the fetch).
 *
 * The test runs from RRAM, so the part that changes EXEC runs from a copy in sec SRAM
 * (eg2_chk_sram_call: EXEC written, fence.i, the call, EXEC restored to the magic value, fence.i).
 * Per drawn value of EXEC it calls (1) the RRAM routine `eg2_chk_stub_inc` (addi a0, a0, 1; ret),
 * or one the test wrote to a scratch page, and (2) a routine in sec SRAM that loads a word from
 * RRAM (the stub's first instruction, whose value is its encoding 0x00150513). Expected: (1)
 * returns 1 with the magic value and faults otherwise; (2) returns the word in every case. EXEC
 * reads back the magic value afterwards and the test runs on from RRAM.
 *
 * Not covered here: RRAM disabled (DIS.SW_DIS) stops fetches too, but it cannot be undone before
 * reset and the test runs from RRAM (REQ_RRAM_23's tests disable RRAM).
 *
 * Knobs:
 *   n_values        EXEC values tried
 *   w_exec_magic    weights of the value class: the magic value,
 *   w_exec_zero       0 (the reset value),
 *   w_exec_flip       the magic value with one bit flipped,
 *   w_exec_inv        its bitwise inverse,
 *   w_exec_random     a random other value
 *   target_class    0 the image's own routine, 1 a routine written to a scratch page, 2 drawn per
 *                   value
 *   model_gaps      skips what the SystemC model does not model: nothing
 *
 * Built with EG2_RRAM_EXEC_EN_INJECT_FAIL the first value check expects bit 0 flipped: the run must
 * fail with `RRAM FAIL`.
 */

#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/sram_ctrl_regs.h"  // Generated.

// CHERIoT mode: the boot stage enables execution from main and sec SRAM before it locks sram_ctrl
// (eg2_cheriot_start.S step 0b, REQ_SOC_08); this test runs code there.
const volatile uint8_t eg2_cheriot_boot_sram_exec __attribute__((used)) = 1;

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define EXEC_EN_PARAMS(X)      \
  X(n_values, 12, 1, 64)       \
  X(w_exec_magic, 20, 0, 100)  \
  X(w_exec_zero, 20, 0, 100)   \
  X(w_exec_flip, 30, 0, 100)   \
  X(w_exec_inv, 10, 0, 100)    \
  X(w_exec_random, 20, 0, 100) \
  X(target_class, 2, 0, 2)     \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(EXEC_EN_PARAMS)

enum {
  kPolls = 200000,
  kSecBase = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
  kLoadStub =
      kSecBase + kEg2ChkSramCallBytes,  // the SRAM routine that loads from RRAM
  kExecReg = TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR + RRAM_CTRL_EXEC_REG_OFFSET,
};

/** lui rd, upper part of `v` (with the sign correction for a following addi/lw
 * of `v`'s low 12). */
static uint32_t insn_lui(uint32_t rd, uint32_t v) {
  return ((v + 0x800) & 0xfffff000u) | rd << 7 | 0x37;
}
static uint32_t insn_i(uint32_t opcode, uint32_t funct3, uint32_t rd,
                       uint32_t rs1, uint32_t v) {
  return (v & 0xfff) << 20 | rs1 << 15 | funct3 << 12 | rd << 7 | opcode;
}

/**
 * Writes the SRAM routine that loads the RRAM word at `addr` into a0 and
 * returns. RISC-V mode: lui a0, %hi ; lw a0, %lo(a0). CHERIoT mode: a
 * capability from PCC (the executable root, which permits loads) with its
 * address set: auipcc ca0, 0 ; lui t0, %hi ; addi t0, t0, %lo ; csetaddr ca0,
 * ca0, t0 ; lw a0, 0(ca0) (CHERIoT ISA: CSetAddr is opcode 0x5b, funct7 0x10).
 */
static void write_load_stub(uint32_t addr) {
  uint32_t insns[5];
  uint32_t n = 0;
#ifdef __CHERIOT__
  insns[n++] = 0x00000517;                   // auipcc ca0, 0
  insns[n++] = insn_lui(5, addr);            // lui t0, %hi(addr)
  insns[n++] = insn_i(0x13, 0, 5, 5, addr);  // addi t0, t0, %lo(addr)
  insns[n++] = 0x10u << 25 | 5u << 20 | 10u << 15 | 10u << 7 |
               0x5b;                        // csetaddr ca0, ca0, t0
  insns[n++] = insn_i(0x03, 2, 10, 10, 0);  // lw a0, 0(ca0)
#else
  insns[n++] = insn_lui(10, addr);             // lui a0, %hi(addr)
  insns[n++] = insn_i(0x03, 2, 10, 10, addr);  // lw a0, %lo(addr)(a0)
#endif
  uint32_t words[8];
  eg2_rram_code_block(words, 8, 0, insns, n);
  volatile uint32_t *dst = eg2_chk_ptr(kLoadStub);
  for (uint32_t i = 0; i < 8; ++i) {
    dst[i] = words[i];
  }
  eg2_chk_fence_i();
}

static uint32_t draw_value(void) {
  const uint32_t w[5] = {EG2_PARAM(w_exec_magic), EG2_PARAM(w_exec_zero),
                         EG2_PARAM(w_exec_flip), EG2_PARAM(w_exec_inv),
                         EG2_PARAM(w_exec_random)};
  switch (eg2_chk_pick(w, 5)) {
    case 0:
      return kEg2RramExecMagic;
    case 1:
      return 0;
    case 2:
      return kEg2RramExecMagic ^ (1u << eg2_rand_range(0, 31));
    case 3:
      return ~(uint32_t)kEg2RramExecMagic;
    default: {
      uint32_t v = eg2_rand();
      return v == kEg2RramExecMagic ? v ^ 1 : v;
    }
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool ok = true;
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  ok = eg2_chk_eq("EXEC at the start (the test runs from RRAM)",
                  kEg2RramExecMagic, eg2_rram_reg(RRAM_CTRL_EXEC_REG_OFFSET)) &&
       ok;

  // Sec SRAM: whole words first (its integrity bits), execution on (sram_ctrl
  // EXEC).
  volatile uint32_t *sec = eg2_chk_ptr(kSecBase);
  for (uint32_t i = 0; i < 2 * kEg2ChkSramCallBytes / 4; ++i) {
    sec[i] = 0;
  }
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET,
                  kMultiBitBool4True);
  if (!eg2_chk_true("SRAM routine installed",
                    eg2_chk_sram_call_install(kSecBase))) {
    return false;
  }
  uint32_t stub = eg2_chk_stub_inc_addr();
  write_load_stub(stub);

  // A second RRAM target, written by the test to a scratch page.
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  uint32_t page =
      eg2_rand_range(kEg2RramScratchFirstPage, kEg2RramScratchLastPage);
  eg2_rram_region_set(region, page, 1,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
  uint32_t a = page * kEg2RramPageBytes +
               kEg2RramWordBytes *
                   eg2_rand_range(0, kEg2RramPageBytes / kEg2RramWordBytes - 1);
  uint32_t insn = eg2_rram_insn_addi_a0(1);
  uint32_t words[4];
  eg2_rram_code_block(words, 4, 0, &insn, 1);
  eg2_rram_result_t r;
  ok = eg2_chk_true("scratch routine written",
                    eg2_rram_write(kEg2RramPartData, a, words, 4, &r)) &&
       ok;
  ok = eg2_chk_eq("scratch routine op_status", 1, r.op_status) && ok;
  uint32_t scratch_stub = kEg2RramHostBase + a;

  uint32_t faults = 0;
  uint32_t runs = 0;
  for (uint32_t i = 0; i < EG2_PARAM(n_values); ++i) {
    uint32_t v = draw_value();
    uint32_t tc = EG2_PARAM(target_class) == 2 ? eg2_rand_range(0, 1)
                                               : EG2_PARAM(target_class);
    uint32_t target = tc == 0 ? stub : scratch_stub;
    bool enabled = v == kEg2RramExecMagic;
    eg2_chk_step("EXEC value", v);
    eg2_chk_step("fetch target", target);
    eg2_chk_trap_t t;
    uint32_t cause =
        eg2_chk_sram_call(kSecBase, kExecReg, v, kEg2RramExecMagic, target, &t);
    if (enabled) {
      ok = eg2_chk_eq("fetch mcause with EXEC enabled", 0, cause) && ok;
      ok = eg2_chk_eq("fetch result with EXEC enabled", 1, t.value) && ok;
      ++runs;
    } else {
      ok = eg2_chk_eq(
               "fetch mcause with EXEC disabled (instruction access fault)",
               kEg2ChkCauseFetchFault, cause) &&
           ok;
      ok = eg2_chk_eq("fetch mtval with EXEC disabled", target, t.mtval) && ok;
      ++faults;
    }
    // A data read of RRAM from SRAM code, under the same EXEC value.
    cause = eg2_chk_sram_call(kSecBase, kExecReg, v, kEg2RramExecMagic,
                              kLoadStub, &t);
    ok = eg2_chk_eq("RRAM data load mcause under this EXEC", 0, cause) && ok;
    ok = eg2_chk_eq("RRAM data load under this EXEC", kEg2ChkStubAddi,
                    t.value) &&
         ok;
    ok = eg2_chk_eq("EXEC restored", kEg2RramExecMagic,
                    eg2_rram_reg(RRAM_CTRL_EXEC_REG_OFFSET)) &&
         ok;
    // The test still runs from RRAM: a direct call of the stub.
    ok = eg2_chk_eq("fetch from RRAM after the restore", 1,
                    eg2_chk_call(stub, 0, &t) == 0 ? t.value : 0) &&
         ok;
  }
  eg2_rram_region_off(region);
  eg2_chk_step("values that fetched", runs);
  eg2_chk_step("values that faulted", faults);
  return eg2_chk_summary(EG2_PARAM(n_values) * 4) && ok;
}
