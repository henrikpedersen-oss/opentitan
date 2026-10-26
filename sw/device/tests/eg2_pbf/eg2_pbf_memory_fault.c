// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_memory_fault (EG2 verification spec REQ_PBF_19, REQ_PBF_07, "Post-boot
 * faults"): an ECC (integrity) error in a memory while the test uses it. Check: the access error
 * and the alert, as the documents define them.
 *
 * `target` 0 main SRAM, 1 retention SRAM: the bench writes `data` to the word under test with
 *   `bits` (1 or 2) bits of its 39-bit integrity code word flipped, by backdoor through the
 *   scrambling (bench step 1, the default keys of the test ROM boot), and the test loads the word.
 *   rv_core_ibex: "fatal_hw_err: triggered when ... a bus integrity error is seen", and "when the
 *   fatal_hw_err alert is raised Ibex fetch is disabled until rv_core_ibex is reset" (theory of
 *   operation, Local Escalation Path). So the load ends the boot; class A escalates fatal_hw_err
 *   straight to the escalation reset, rstmgr captures the crash dumps, and boot 2 checks: the
 *   reset reason is the escalation, the alert crash dump holds fatal_hw_err, and the CPU crash
 *   dump's last exception address (mtval) and last data address are the loaded word (rv_core_ibex
 *   theory of operation, "Crash Dump").
 * `target` 2 RRAM: the test programs four words (one RRAM word) in the OTTF non-volatile scratch
 *   area under a data region with ECC on, and the bench flips stored bits of that RRAM word
 *   (bench step 1): `bits` 1 one bit of the low 64-bit half, 2 one bit in each half, 3 two bits of
 *   the low half (rram_macro: two Hamming(72,64) codes per RRAM word). rram_ctrl theory of
 *   operation "RRAM ECC Error Handling":
 *     1 "Single-Bit Error (Correctable)": the data returned corrected, corr1_err, CORR1_ERR_LOC the
 *       word; then the "semi-automatic repair": a Rewrite, after which a read reports no error;
 *     2 "Double-Bit Error (Correctable)": the same with corr2_err and CORR2_ERR_LOC;
 *     3 "Multi-Bit Error (Uncorrectable)": the fatal alert (fatal_err); escalated to a reset as
 *       above, boot 2 checks the reason, the alert crash dump and the CPU's last data address.
 *
 * Knobs: target, bits, word (the word's index in the area), data, model_gaps. The SystemC model
 * has no bench and models no ECC: with model_gaps = 1 the test writes `data` itself, checks it
 * reads back, and skips the fault.
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"     // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define MEMORY_FAULT_PARAMS(X)  \
  X(target, 0, 0, 2)            \
  X(bits, 1, 1, 3)              \
  X(word, 7, 0, 63)             \
  X(data, 0x5a3cc3a5, 0, 0xffffffff) \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(MEMORY_FAULT_PARAMS)

enum {
  kTargetMain = 0,
  kTargetRet = 1,
  kTargetRram = 2,
  kEscCycles = 200,
  kResetWait = 400000,
  kRetPhase = 0,
  kRetCause = 1,
  kPhaseFaulted = 0x46415554,  // "FAUT"
  kResetEscalation = 1u << (RSTMGR_RESET_INFO_HW_REQ_OFFSET + 3),
};

/** The word under test, by bus address (the bench computes the same). */
static uint32_t target_addr(void) {
  switch (EG2_PARAM(target)) {
    case kTargetMain:
      return kEg2PbfMainFree + 4 * EG2_PARAM(word);
    case kTargetRet:
      return kEg2PbfRetFree + 4 * EG2_PARAM(word);
    default:
      return eg2_pbf_nv_scratch() + 16 * EG2_PARAM(word);
  }
}

static const char *target_name(void) {
  return EG2_PARAM(target) == kTargetMain  ? "main SRAM"
         : EG2_PARAM(target) == kTargetRet ? "retention SRAM"
                                           : "RRAM";
}

/** The four words of the RRAM word under test: `data` and its neighbours from the seed. */
static void rram_words(uint32_t *w) {
  w[0] = EG2_PARAM(data);
  w[1] = ~EG2_PARAM(data);
  w[2] = EG2_PARAM(data) ^ 0x0f0f0f0f;
  w[3] = EG2_PARAM(data) + 1;
}

/** Loads the word through a probe (a load fault resumes here) and keeps the cause. */
static uint32_t probe_load(uint32_t addr, uint32_t *value) {
  eg2_rm_irq_off();
  eg2_rm_out_t out = {0, 0};
  uint32_t cause = eg2_rm_probe(eg2_rm_ptr(addr), kEg2RmLw, 0, &out);
  *value = out.value;
  return cause;
}

/** Waits for the escalation reset; returns (fails) only if it never comes. */
static bool wait_reset(uint32_t cause, uint32_t value) {
  *eg2_pbf_ret(kRetCause) = cause;
  eg2_pbf_note("load returned: mcause", cause);
  eg2_pbf_note("load returned: value", value);
  eg2_pbf_spin(kResetWait);
  return eg2_pbf_step("escalation reset after the error", false);
}

static bool model_run(uint32_t addr) {
  eg2_pbf_model_gap("ECC injection by the bench, the integrity fault and its alert");
  uint32_t got = 0;
  if (EG2_PARAM(target) == kTargetRram) {
    uint32_t w[4];
    rram_words(w);
    bool ok = eg2_pbf_step("RRAM data region",
                           eg2_pbf_rram_region(addr & ~(kEg2PbfRramPage - 1u), 1, true, true));
    ok = eg2_pbf_step("RRAM program", eg2_pbf_rram_program(addr, w, 4)) && ok;
    got = *eg2_pbf_word(addr);
    return eg2_pbf_check("word read back", EG2_PARAM(data), got) && ok;
  }
  *eg2_pbf_word(addr) = EG2_PARAM(data);
  got = *eg2_pbf_word(addr);
  return eg2_pbf_check("word read back", EG2_PARAM(data), got);
}

static bool sram_fault(uint32_t addr) {
  uint32_t alert = eg2_pbf_alert("rv_core_ibex_fatal_hw_err");
  eg2_pbf_escalate_reset(&alert, 1, kEscCycles);
  eg2_status_expect_alert(alert);
  eg2_pbf_crashdump_arm();
  *eg2_pbf_ret(kRetPhase) = kPhaseFaulted;
  eg2_pbf_bench_step(1);  // the bench corrupts the word
  uint32_t value;
  uint32_t cause = probe_load(addr, &value);
  return wait_reset(cause, value);
}

static bool rram_fault(uint32_t addr) {
  uint32_t w[4];
  rram_words(w);
  bool ok = eg2_pbf_step("RRAM data region",
                         eg2_pbf_rram_region(addr & ~(kEg2PbfRramPage - 1u), 1, true, true));
  ok = eg2_pbf_step("RRAM program", eg2_pbf_rram_program(addr, w, 4)) && ok;
  uint32_t bits = EG2_PARAM(bits);
  if (bits == 3) {
    uint32_t alert = eg2_pbf_alert("rram_ctrl_fatal_err");
    eg2_pbf_escalate_reset(&alert, 1, kEscCycles);
    eg2_status_expect_alert(alert);
    eg2_pbf_crashdump_arm();
    *eg2_pbf_ret(kRetPhase) = kPhaseFaulted;
    eg2_pbf_bench_step(1);
    uint32_t value;
    uint32_t cause = probe_load(addr, &value);
    return wait_reset(cause, value) && ok;
  }
  eg2_pbf_bench_step(1);
  uint32_t corr = bits == 1 ? RRAM_CTRL_INTR_STATE_CORR1_ERR_BIT : RRAM_CTRL_INTR_STATE_CORR2_ERR_BIT;
  uint32_t loc_reg =
      bits == 1 ? RRAM_CTRL_CORR1_ERR_LOC_REG_OFFSET : RRAM_CTRL_CORR2_ERR_LOC_REG_OFFSET;
  eg2_status_expect_intr(kEg2DevRramCtrlCore, 1u << corr);
  uint32_t got = *eg2_pbf_word(addr);
  ok = eg2_pbf_check("corrected word through the host window", w[0], got) && ok;
  uint32_t st = eg2_dev_read32(kEg2DevRramCtrlCore, RRAM_CTRL_INTR_STATE_REG_OFFSET);
  ok = eg2_pbf_check(bits == 1 ? "corr1_err raised" : "corr2_err raised", 1, (st >> corr) & 1) &&
       ok;
  uint32_t loc = eg2_dev_read32(kEg2DevRramCtrlCore, loc_reg);
  eg2_pbf_note("CORR_ERR_LOC", loc);
  ok = eg2_pbf_check("CORR_ERR_LOC: the RRAM word",
                     (addr - TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR) >> 4, (loc & 0x1fffff) >> 4) &&
       ok;
  // The semi-automatic repair: a Rewrite restores the word; a read then reports no error.
  ok = eg2_pbf_step("rewrite of the corrected word", eg2_pbf_rram_rewrite(addr)) && ok;
  eg2_dev_write32(kEg2DevRramCtrlCore, RRAM_CTRL_INTR_STATE_REG_OFFSET, 1u << corr);
  uint32_t again[4];
  ok = eg2_pbf_step("controller read after the rewrite", eg2_pbf_rram_ctrl_read(addr, again, 4)) &&
       ok;
  ok = eg2_pbf_check_words("words after the rewrite", w, again, 4) && ok;
  st = eg2_dev_read32(kEg2DevRramCtrlCore, RRAM_CTRL_INTR_STATE_REG_OFFSET);
  ok = eg2_pbf_check("no corrected error after the rewrite", 0, (st >> corr) & 1) && ok;
  eg2_pbf_activity("rram corrected errors", 1);
  return ok;
}

/** Boot 2: what the escalation reset left. */
static bool after_reset(uint32_t addr) {
  bool ok = eg2_pbf_check("phase before the reset", kPhaseFaulted, *eg2_pbf_ret(kRetPhase));
  ok = eg2_pbf_check("reset reason: escalation", kResetEscalation, eg2_pbf_reset_reason()) && ok;
  bool rram = EG2_PARAM(target) == kTargetRram;
  uint32_t alert = eg2_pbf_alert(rram ? "rram_ctrl_fatal_err" : "rv_core_ibex_fatal_hw_err");
  ok = eg2_pbf_check("alert crash dump: the alert's cause", 1, eg2_pbf_crashdump_alert(alert)) && ok;
  uint32_t mtval = eg2_pbf_crashdump_cpu(0), data_addr = eg2_pbf_crashdump_cpu(2);
  eg2_pbf_note("CPU crash dump mtval", mtval);
  eg2_pbf_note("CPU crash dump last data address", data_addr);
  eg2_pbf_note("load cause before the reset (0: none seen)", *eg2_pbf_ret(kRetCause));
  ok = eg2_pbf_check("CPU crash dump: last data address", addr, data_addr) && ok;
  if (!rram) {
    ok = eg2_pbf_check("CPU crash dump: mtval of the load fault", addr, mtval) && ok;
  }
  eg2_pbf_activity("memory faults escalated", 1);
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("memory_fault", EG2_PBF_INJECT);
  uint32_t boot = eg2_pbf_boot_index();
  uint32_t addr = target_addr();
  eg2_pbf_note(target_name(), addr);
  if (boot > 0) {
    return after_reset(addr) && ok;
  }
  if (EG2_PARAM(model_gaps)) {
    return model_run(addr) && ok;
  }
  if (EG2_PARAM(target) == kTargetRram) {
    return rram_fault(addr) && ok;
  }
  if (EG2_PARAM(bits) == 3) {
    eg2_pbf_note("bits 3 is for the RRAM; the SRAM word gets", 2);
  }
  return sram_fault(addr) && ok;
}
