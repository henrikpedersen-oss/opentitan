// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DM_CAP_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DM_CAP_H_

/**
 * @file
 * @brief What chip_eg2_rv_dm_cap_bypass, chip_eg2_rv_dm_halt_revocation and chip_eg2_rv_dm_priv
 * share (EG2 spec "Debug Controller" / "Corner cases"): stages in which the core waits, with
 * known registers, for the bench to halt it and run program buffers against them.
 *
 * The tests run under eg2_dm_serve (eg2_dm_corners.h): each phase is the action
 * kEg2DmAction with arg = the phase, so the bench sends `cmd`, waits for the start line, does its
 * part, writes `stop` and waits for the end line. A phase's stage is called through
 * eg2_probe_call (eg2_cheriot/eg2_probe.h), so a trap after resume (a bad DEPCC, an ecall from the
 * stage) is recorded and returns into C.
 *
 * Stage protocol: the stage writes the address of its wait loop to the flag word (a scratch word
 * of eg2_dm_corners, whose address the ready line gives the bench) and spins until `stop` is set.
 * The bench polls the flag over SBA, halts, checks that dpc is in the wait loop, and only then
 * uses the registers below. Registers while a stage waits (ca0 is always the context):
 *
 *   ca0  the context (eg2_dmx_ctx_t)        ca4  the landing capability (DEPCC tests)
 *   a1, a2, t0, t1, t2  the debugger's      a3, a5, ra, sp, gp, tp, s0, s1  untouched
 *
 * eg2_probe_call's own registers (ca3 the stub, ca5 its return) are never written by a stage.
 * Offsets are fixed: the bench (env/seq_lib/chip_eg2_rv_dm_cap_bypass_vseq.sv) encodes them in
 * its program buffers, and the assembly (eg2_dm_cap_stage.S) uses them; the C side checks them.
 */

#define EG2_DMX_STOP 0
#define EG2_DMX_FLAG 8
#define EG2_DMX_AUTH_OOB 16
#define EG2_DMX_AUTH_UT 24
#define EG2_DMX_AUTH_NP 32
#define EG2_DMX_AUTH_MARK 40
#define EG2_DMX_SEALER 48
#define EG2_DMX_AUTH_CLEAN 56
#define EG2_DMX_OUT 64
#define EG2_DMX_LANDING 128
#define EG2_DMX_N 132
#define EG2_DMX_ERRS 136
#define EG2_DMX_SIZE 144

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  /** The tests' ids (kEg2DebugMagic + id ends a test); after eg2_dm_corners.h's. */
  kEg2DebugIdCapBypass = 0x30,
  kEg2DebugIdHaltRevocation = 0x31,
  kEg2DebugIdPriv = 0x32,
  /** The scratch words of eg2_dm_corners the stages and phases publish in (bench reads by SBA). */
  kEg2DmxScratchFlag = 0,
  kEg2DmxScratchArg0 = 1,
  kEg2DmxScratchArg1 = 2,
  kEg2DmxScratchArg2 = 3,
  kEg2DmxScratchArg3 = 4,
  kEg2DmxOutSlots = 8,
};

/** The context a stage runs on; eight-byte slots in both modes. */
typedef struct eg2_dmx_ctx {
  eg2_probe_slot_t stop;        // 0: volatile uint32_t *, the stop word
  eg2_probe_slot_t flag;        // 8: volatile uint32_t *, the stage writes its wait loop's address
  eg2_probe_slot_t auth_oob;    // 16: a tagged authority whose bounds end before the accesses
  eg2_probe_slot_t auth_ut;     // 24: an untagged authority
  eg2_probe_slot_t auth_np;     // 32: a tagged authority with GL only
  eg2_probe_slot_t auth_mark;   // 40: an authority over a slot holding a capability whose base is
                                //     marked in the revocation bitmap
  eg2_probe_slot_t sealer;      // 48: a sealing capability, address = the object type
  eg2_probe_slot_t auth_clean;  // 56: an authority over a slot holding an unmarked capability
  eg2_probe_slot_t out[kEg2DmxOutSlots];  // 64: the program buffers' CSC targets
  uint32_t landing;             // 128: the landing address (written by the stage)
  uint32_t n;                   // 132: iterations (CLC loop)
  uint32_t errs;                // 136: tag errors (CLC loop)
  uint32_t pad;
} eg2_dmx_ctx_t;

_Static_assert(offsetof(eg2_dmx_ctx_t, flag) == EG2_DMX_FLAG, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, auth_oob) == EG2_DMX_AUTH_OOB, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, auth_ut) == EG2_DMX_AUTH_UT, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, auth_np) == EG2_DMX_AUTH_NP, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, auth_mark) == EG2_DMX_AUTH_MARK, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, sealer) == EG2_DMX_SEALER, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, auth_clean) == EG2_DMX_AUTH_CLEAN, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, out) == EG2_DMX_OUT, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, landing) == EG2_DMX_LANDING, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, n) == EG2_DMX_N, "ctx layout");
_Static_assert(offsetof(eg2_dmx_ctx_t, errs) == EG2_DMX_ERRS, "ctx layout");
_Static_assert(sizeof(eg2_dmx_ctx_t) == EG2_DMX_SIZE, "ctx layout");

// Stages (eg2_dm_cap_stage.S); call them with eg2_probe_call(ctx, op1, NULL, stage).
/** Waits for `stop`, then returns. */
extern char eg2_dmx_park[];
/** Waits for `stop`, then executes `ecall` (its cause tells the privilege it ran at). */
extern char eg2_dmx_park_ecall[];
/** The instruction a DEPCC test's dret must never reach (an `ecall`). */
extern char eg2_dmx_landing[];
#ifdef __CHERIOT__
/**
 * Waits until t1 != 0 (op1 is t1's first value; the bench writes it), then CLC from auth_mark,
 * CLC from auth_clean, CLC from auth_mark, and CSC of the three to out[0..2].
 */
extern char eg2_dmx_step_clc[];
/** CLC from auth_mark and from auth_clean until `stop`; counts in n, tag errors in errs. */
extern char eg2_dmx_clc_loop[];
/** One CSpecialRW of SCR 24, 25 or 26 (read into ca2, or write from ca1), then cret. */
extern char eg2_dmx_scr24_r[], eg2_dmx_scr25_r[], eg2_dmx_scr26_r[];
extern char eg2_dmx_scr24_w[], eg2_dmx_scr25_w[], eg2_dmx_scr26_w[];
#endif

/** Clears the flag word and points the context at the stop and flag words. */
void eg2_dmx_ctx_init(eg2_dmx_ctx_t *ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // __ASSEMBLER__

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_DEBUG_EG2_DM_CAP_H_
