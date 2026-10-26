// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DM_CORNERS_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DM_CORNERS_H_

/**
 * @file
 * @brief The software side of the debug module's corner tests (`chip_eg2_rv_dm_*` of the EG2
 * spec's "Debug Controller" / "Corner cases"): the core runs the kernel the bench asks for, while
 * the bench drives the debug module over JTAG and times its requests against the kernel.
 *
 * Contract with the vseq (env/seq_lib/chip_eg2_rv_dm_corners_base_vseq.sv):
 *   1. With `late_debug_sw` the software writes rv_dm LATE_DEBUG_ENABLE = MuBi32True first.
 *   2. It prints `EG2 DEBUG boot 0x<reset reason>` (rstmgr RESET_INFO as the test ROM saved it
 *      in retention SRAM) and
 *      `EG2 DEBUG ready 0x<done> 0x<scratch> 0x<cmd> 0x<stop> 0x<vector> 0x<lock>`:
 *      the words the bench writes over SBA (`done`, `cmd`, `stop`), a scratch area of
 *      kEg2DmScratchWords words, the kernels' trap vector and the address of rv_core_ibex's
 *      CHERIOT_LOCK register (the CHERIoT start-up code's lock store).
 *   3. It waits for a command: the bench writes `cmd` = seq << 16 | arg << 8 | kind (seq != 0
 *      and new). The software clears `stop`, prints `EG2 DEBUG start 0x<seq> 0x<kind> 0x<target>`
 *      (target: the bus address of the kernel's timed instruction), runs the kernel until the
 *      bench writes `stop` != 0, checks what it did, and prints `EG2 DEBUG end 0x<seq> 0x<ok>`
 *      (ok: 1 while
 *      every check so far passed; the bench fails the run on 0 at once).
 *      A kind >= kEg2DmAction is the test's own action (cfg->action), with the same lines.
 *   4. The bench ends the test by writing kEg2DebugMagic + test id to `done`.
 * Without the bench (eg2sim, `model_gaps` = 1) the software runs every kernel in `model_kinds`
 * for `model_iters` iterations and its actions with arg 0, and checks them the same way.
 *
 * Kernels (eg2_dm_kernels.S): DIV a divide loop, LOAD a load from RRAM, STORE a store across the
 * always-on CDC (aon_timer), CAP a CLC/CSC swap (LW/SW in RISC-V mode), WFI wfi woken by rv_timer,
 * TRAP an ecall loop, IRQ rv_plic's software interrupt, NMI the watchdog bark, PARK a counting loop
 * whose a0 and t1 the debugger may use.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/tests/eg2_debug/eg2_debug.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  kEg2DmDiv = 1,
  kEg2DmLoad = 2,
  kEg2DmStore = 3,
  kEg2DmCap = 4,
  kEg2DmWfi = 5,
  kEg2DmTrap = 6,
  kEg2DmIrq = 7,
  kEg2DmNmi = 8,
  kEg2DmPark = 9,
  kEg2DmKernelCount = 10,
  /** Commands from here on are the test's own actions. */
  kEg2DmAction = 0x10,
  kEg2DmScratchWords = 32,
  /** The corner tests' ids (kEg2DebugMagic + id ends a test). */
  kEg2DebugIdHaltCorners = 0x10,
  kEg2DebugIdAbstractErrors = 0x11,
  kEg2DebugIdResetCorners = 0x12,
  kEg2DebugIdLcDrop = 0x13,
  kEg2DebugIdSbaCorners = 0x14,
  kEg2DebugIdStepCorners = 0x15,
  kEg2DebugIdCounters = 0x17,
  kEg2DebugIdTriggers = 0x18,
};

/** A kernel's context; the offsets are fixed in eg2_dm_kernels.S. */
typedef struct eg2_dm_kctx {
  volatile uint32_t *stop;
  volatile uint32_t *dev;
  void *volatile *slots;
  volatile uint32_t *msip;
  volatile uint32_t *ack[3];
  uint32_t ack_val[3];
  uint32_t max;
  uint32_t arg;
  uint32_t n;
  uint32_t sum;
  uint32_t traps;
  uint32_t irqs;
} eg2_dm_kctx_t;

/** Runs kernel `kind`; returns the iterations it made. */
uint32_t eg2_dm_kernel(uint32_t kind, eg2_dm_kctx_t *ctx);
/** The bus address of kernel `kind`'s timed instruction; 0 gives the kernels' trap vector. */
uint32_t eg2_dm_kernel_target(uint32_t kind);

/** A test's own action `kind` (>= kEg2DmAction) with `arg`; returns its checks' verdict. */
typedef bool (*eg2_dm_action_fn)(uint32_t kind, uint32_t arg);

typedef struct eg2_dm_cfg {
  uint32_t test_id;        // kEg2DebugId*
  uint32_t late_debug_sw;  // 1: write LATE_DEBUG_ENABLE first
  uint32_t max_iters;      // a kernel's iteration limit (the bench stops it first)
  uint32_t idle_polls;     // polls of `cmd` and `done` before the software gives up
  uint32_t model_gaps;     // 1: eg2sim, no bench
  uint32_t model_iters;    // a kernel's iterations in eg2sim
  uint32_t model_kinds;    // bit k: kernel k runs in eg2sim
  uint32_t model_actions;  // bit k: action kEg2DmAction + k runs in eg2sim
  uint32_t wfi_ticks;      // WFI: rv_timer ticks (1 us each) between wake-ups
  uint32_t nmi_bark;       // NMI: watchdog bark threshold, aon_timer ticks
  uint32_t nmi_spin;       // NMI: polls for the NMI per iteration
  eg2_dm_action_fn action;  // the test's own actions, or NULL
} eg2_dm_cfg_t;

/** Runs the contract above; returns the checks' verdict. */
bool eg2_dm_serve(const eg2_dm_cfg_t *cfg);

/** Runs kernel `kind` with its set-up and checks (also usable from an action). */
bool eg2_dm_run_kernel(const eg2_dm_cfg_t *cfg, uint32_t kind, uint32_t max);

/** The stop word the bench sets over SBA to end a kernel or an action's loop. */
volatile uint32_t *eg2_dm_stop(void);

/** The scratch area the bench may read and write over SBA (kEg2DmScratchWords words). */
volatile uint32_t *eg2_dm_scratch(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DM_CORNERS_H_
