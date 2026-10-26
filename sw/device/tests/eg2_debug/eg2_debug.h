// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DEBUG_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DEBUG_H_

/**
 * @file
 * @brief The software side of the EG2 debug-module tests (eg2_debug): the core works in a known
 * loop while the bench drives the debug module over JTAG, then the bench ends the loop over SBA.
 *
 * Contract with the test's vseq (env/seq_lib/chip_eg2_rv_dm_base_vseq.sv):
 *   1. If `late_debug_sw` is set, the software writes LATE_DEBUG_ENABLE (rv_dm.regs) to MuBi32True
 *      first: in DEV with OTP_DIS_RV_DM_LATE_DEBUG not True that is what enables the debug module
 *      (rv_dm.hjson, DM_EN.CTRL.LC_GATED).
 *   2. It prints `EG2 DEBUG ready 0x<done> 0x<scratch>` to the DV log: the addresses of `done`
 *      and of a scratch word in main SRAM. The bench may start its JTAG accesses from then on.
 *   3. It loops: each iteration adds the iteration number to a running sum and polls `done`. The
 *      bench ends the loop by writing the test's magic word (kEg2DebugMagic + test id) to `done`
 *      over SBA. A halt and resume must not disturb the loop: the sum is checked against its
 *      closed form, and the loop must have made progress.
 * Without the bench (eg2sim: no debug module, no JTAG; `model_gaps` = 1) the loop runs
 * `model_iters` iterations and the wait for `done` is skipped.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/tests/eg2_mem/eg2_chk.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  kEg2DebugMagic = 0x600d0000,
  kEg2DebugIdJtag = 1,
  kEg2DebugIdDtm = 2,
  kEg2DebugIdControlStatus = 3,
  kEg2DebugIdSbaTlErrors = 4,
  kEg2DebugIdSbaCheriotMode = 5,
};

/** The loop's knobs, from the test's parameters. */
typedef struct eg2_debug_cfg {
  uint32_t test_id;        // kEg2DebugId*
  uint32_t late_debug_sw;  // 1: write LATE_DEBUG_ENABLE first
  uint32_t max_iters;      // the loop gives up after this many iterations without `done`
  uint32_t min_iters;      // progress the loop must have made before `done`
  uint32_t model_gaps;     // 1: eg2sim, no bench
  uint32_t model_iters;    // iterations in eg2sim
} eg2_debug_cfg_t;

/** Runs the contract above; returns the checks' verdict. */
bool eg2_debug_run(const eg2_debug_cfg_t *cfg);

/** The scratch word the bench may use for its SBA read/write checks. */
volatile uint32_t *eg2_debug_scratch(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DEBUG_H_
