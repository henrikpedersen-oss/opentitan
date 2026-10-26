// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rv_dm_sba_cheriot_mode (EG2 verification spec REQ_CDM_06, and REQ_CMS_16's SBA writer),
 * CHERIoT mode only: in a debug-enabled life-cycle state the debug module's system bus access
 * reads and writes memory that holds capabilities without any capability check, and an SBA write
 * over a stored capability leaves its tag clear (REQ_CMS_16: "any write by a non-CHERIoT host ...
 * must clear"), while a read leaves it set.
 *
 * Expected chip failure until ticket F23 is decided (status/rtl_tickets.md): xbar_main routes
 * rv_dm.sba straight to main SRAM, around cheriot_mem_sys, so an SBA write leaves the tag set.
 *
 * The software stores `n_caps` capabilities in main SRAM (`caps_`, .bss), capability i pointing
 * at word i of a plain array (`plain_`, which holds no capability; word j holds
 * (j * 0x01010101) ^ 0x5a5a5a5a). All are derived from the same bounded capability, so their
 * second words (the metadata) are equal and their first words are &plain_[i]. It prints
 *
 *   EG2 DEBUG caps 0x<&caps_> 0x<n_caps> 0x<&plain_> 0x<metadata word>
 *
 * then runs the debug loop (eg2_debug.h). The bench makes its SBA reads and writes of 8, 16 or 32
 * bits over the capabilities' first words, their second words, both, or a plain word, checks
 * every read against its model, writes the mask of the capabilities it wrote to the scratch word
 * and ends the loop (env/seq_lib/chip_eg2_rv_dm_sba_cheriot_mode_vseq.sv). The software then
 * loads every capability (CLC) and checks its tag: clear exactly for the capabilities in the mask;
 * an untouched capability also keeps its address.
 *
 * Knobs (the vseq writes them; it also draws the life-cycle state among the debug-enabled ones
 * and its timing knobs w_when_<t>):
 *   n_caps         stored capabilities (1-16)
 *   late_debug_sw  1: write rv_dm LATE_DEBUG_ENABLE = MuBi32True first
 *   max_iters      loop iterations before the software gives up on `done`
 *   min_iters      progress the loop must have made before `done`
 *   model_iters    iterations in eg2sim
 *   model_gaps     skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why                                            |
 * |-------------------------------------------|------------------------------------------------|
 * | the done word, the loop's progress and    | eg2sim has no debug module, JTAG or SBA: the   |
 * |   every SBA write (mask 0)                |   tags are checked kept                        |
 *
 * Built with EG2_RV_DM_SBA_CHERIOT_MODE_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_debug/eg2_debug.h"

#ifndef __CHERIOT__
#error "chip_sw_rv_dm_sba_cheriot_mode is a CHERIoT-mode test"
#endif

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define RV_DM_PARAMS(X)                  \
  X(n_caps, 8, 1, 16)                    \
  X(late_debug_sw, 0, 0, 1)              \
  X(max_iters, 5000000, 1000, 100000000) \
  X(min_iters, 16, 0, 1000000)           \
  X(model_iters, 5000, 1, 1000000)       \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(RV_DM_PARAMS)

enum {
  kMaxCaps = 16,
  kPlainWords = 32,
};

// Main SRAM in CHERIoT mode (.bss); the bench reaches both over SBA.
static uint32_t plain_[kPlainWords] __attribute__((aligned(8)));
static void *caps_[kMaxCaps];

/** The second (metadata) word of the capability stored in caps_[i], as an integer load. */
static uint32_t meta_word(uint32_t i) { return ((volatile uint32_t *)&caps_[i])[1]; }

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const uint32_t n = EG2_PARAM(n_caps);
  bool ok = true;
  for (uint32_t j = 0; j < kPlainWords; ++j) {
    plain_[j] = (j * 0x01010101u) ^ 0x5a5a5a5au;
  }
  for (uint32_t i = 0; i < n; ++i) {
    caps_[i] = (void *)&plain_[i];
  }
  // The bench's model assumes one metadata word for all: every capability is plain_'s, moved.
  const uint32_t meta = meta_word(0);
  for (uint32_t i = 0; i < n; ++i) {
    ok = eg2_chk_eq("stored capability's metadata word (as capability 0's)", meta, meta_word(i)) &&
         ok;
    ok = eg2_chk_eq("stored capability's tag before SBA", 1, __builtin_cheri_tag_get(caps_[i])) &&
         ok;
  }
  LOG_INFO("EG2 DEBUG caps 0x%08x 0x%08x 0x%08x 0x%08x", eg2_chk_addr_of(&caps_[0]), n,
           eg2_chk_addr_of(&plain_[0]), meta);

  const eg2_debug_cfg_t cfg = {
      .test_id = kEg2DebugIdSbaCheriotMode,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .min_iters = EG2_PARAM(min_iters),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
  };
  ok = eg2_debug_run(&cfg) && ok;

  // The bench's mask of the capabilities an SBA write touched (0 without the bench).
  const uint32_t mask = *eg2_debug_scratch();
  eg2_chk_step("SBA-written capabilities (mask)", mask);
  ok = eg2_chk_eq("mask within the stored capabilities", 0, n < 32 ? mask >> n : 0) && ok;
  for (uint32_t i = 0; i < n; ++i) {
    const bool written = (mask >> i) & 1;
    eg2_chk_step("capability", i);
    ok = eg2_chk_eq(written ? "tag after an SBA write (cleared, REQ_CMS_16)"
                            : "tag without an SBA write (kept)",
                    written ? 0 : 1, __builtin_cheri_tag_get(caps_[i])) &&
         ok;
    if (!written) {
      ok = eg2_chk_eq("address without an SBA write", eg2_chk_addr_of(&plain_[i]),
                      eg2_chk_addr_of(caps_[i])) &&
           ok;
    }
  }
  return eg2_chk_summary(3 * n + 3) && ok;
}
