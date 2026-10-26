// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_debug_sb (EG2 verification spec REQ_PBF_20, REQ_CDM, "Post-boot features in
 * CHERIoT mode", "Debug Controller"): the owner image halted and resumed over JTAG after secure
 * boot (or the test ROM), in RISC-V mode and in CHERIoT mode.
 *
 * The software runs a computation the halt must not disturb, while the bench (chip_sw_eg2_pbf_
 * debug_sb_vseq) halts the core at `halt_point`, performs `operation` (0 halt and resume only, 1
 * a register read by abstract command, 2 a memory read and 3 a memory write by system bus access)
 * and resumes it, then writes `mailbox_value` into the mailbox word by SBA, and the number of
 * halts into the word after it.
 *
 * Checks, in software:
 *   - the computation's result: sum of i * k for i < kIterations, k from the seed, against its
 *     closed form k * n (n - 1) / 2 (mod 2^32);
 *   - the life cycle state decides (lc_ctrl's life cycle signal table: HW_DEBUG_EN in
 *     TEST_UNLOCKED*, DEV, RMA): with debug enabled the mailbox holds `mailbox_value` and the
 *     halt count is at least 1; without it (PROD) the bench makes no access and the mailbox stays
 *     0;
 *   - CHERIoT mode, "tags kept": a capability stored before the halt still loads tagged, with the
 *     same address and bounds, after the resume.
 * The bench checks the halt itself (dmstatus allhalted, dcsr.cause haltreq), and its own reads.
 * The mailbox is the first word of retention SRAM above retention_sram_t (eg2_pbf_mem.h), which
 * the bench addresses by the same rule.
 *
 * Knobs: halt_point (0 early, 1 middle, 2 late, 3 drawn by the bench), operation,
 * mailbox_value, model_gaps (the SystemC model has no JTAG: the mailbox checks are skipped).
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define DEBUG_SB_PARAMS(X)                \
  X(halt_point, 1, 0, 3)                  \
  X(operation, 3, 0, 3)                   \
  X(mailbox_value, 0x0dbeef01, 1, 0xffffffff) \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(DEBUG_SB_PARAMS)

enum {
  kIterations = 20000,
  kMailboxPolls = 2000000,
  kMailbox = kEg2PbfRetFree,
  kHaltCount = kEg2PbfRetFree + 4,
};

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("debug", EG2_PBF_INJECT);
  eg2_pbf_boot_stage();
  uint32_t lc = eg2_pbf_lc_state();
  bool debug = eg2_pbf_lc_debug_enabled(lc);
  eg2_puts("PBF debug: life cycle ");
  eg2_puts(eg2_pbf_lc_name(lc));
  eg2_puts(debug ? ", debug enabled\r\n" : ", debug disabled\r\n");
  *eg2_pbf_word(kMailbox) = 0;
  *eg2_pbf_word(kHaltCount) = 0;

#ifdef __CHERIOT__
  void *volatile *slot = (void *volatile *)eg2_rm_ptr(kEg2PbfMainFree);
  void *cap = __builtin_cheri_bounds_set(
      __builtin_cheri_address_set((void *)eg2_rm_ptr(kEg2PbfMainFree), kEg2PbfMainFree + 64), 64);
  *slot = cap;
#endif

  // The bench halts the core while this runs (bench step 1: the work starts).
  uint32_t k = eg2_rand() | 1;
  eg2_pbf_bench_step(1);
  volatile uint32_t sum = 0;
  for (uint32_t i = 0; i < kIterations; ++i) {
    sum += i * k;
  }
  uint32_t expected = k * (kIterations * (kIterations - 1u) / 2u);  // 4e8: no 32-bit overflow
  ok = eg2_pbf_check("computation across the halt", expected, sum) && ok;

  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("JTAG halt, resume and system bus access (no debug module in the model)");
  } else if (debug) {
    uint32_t i = 0;
    while (i < kMailboxPolls && *eg2_pbf_word(kMailbox) != EG2_PARAM(mailbox_value)) {
      ++i;
    }
    ok = eg2_pbf_check("mailbox written by SBA", EG2_PARAM(mailbox_value), *eg2_pbf_word(kMailbox)) &&
         ok;
    uint32_t halts = *eg2_pbf_word(kHaltCount);
    eg2_pbf_activity("halts by the debugger", halts);
    ok = eg2_pbf_step("the debugger halted the core", halts >= 1) && ok;
  } else {
    eg2_pbf_spin(kMailboxPolls / 4);
    ok = eg2_pbf_check("mailbox untouched with debug disabled", 0, *eg2_pbf_word(kMailbox)) && ok;
  }

#ifdef __CHERIOT__
  void *back = *slot;
  ok = eg2_pbf_check("capability tag after the resume", 1, __builtin_cheri_tag_get(back) ? 1 : 0) &&
       ok;
  ok = eg2_pbf_check("capability address after the resume",
                     (uint32_t)__builtin_cheri_address_get(cap),
                     (uint32_t)__builtin_cheri_address_get(back)) &&
       ok;
  ok = eg2_pbf_check("capability length after the resume", 64,
                     (uint32_t)__builtin_cheri_length_get(back)) &&
       ok;
#endif
  return ok;
}
