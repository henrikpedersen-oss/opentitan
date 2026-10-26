// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_REVOKE_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_REVOKE_H_

/**
 * @file
 * @brief `eg2_revoke`: what the revocation corner cases share (EG2 spec "Revocation", "Corner
 * cases").
 *
 *  - The load filter's expression, the oracle of the corner cases (the TRVK and TBRE software test
 *    plan, quoted in the spec): for a capability `c` loaded from a tagged region,
 *
 *      tag = Tag(c) & !(!Sealing(c) & Revocable(Base(c)) & REVBM(Base(c)))
 *
 *    Sealing(c): sealing-format permissions with at least one of SE, US or U0 (not "sealed").
 *    Revocable(b): b in the revocable window, main and sec SRAM, 0x1000_0000 to 0x1003_0000.
 *    REVBM(b): the bitmap bit of b's granule (cheriot_mem_sys theory_of_operation.md: bit b of
 *    word w covers the 8 bytes at 0x1000_0000 + (w * 32 + b) * 8). Where `c` is stored does not
 *    enter it. Tag(c) is the tag after CHERIoT-Sail's LoadCapImm steps before the revocation
 *    check (an authority without MC clears it; GL/LG and SD/LM are attenuated on the tag read
 *    from memory, eg2_cap_clc_result), so the attenuation follows the tag before revocation.
 *  - The bench steps of the corner cases (chip_sw_eg2_revocation_bench_vseq): the request counts
 *    on the core's data and bitmap ports, the response delays on the SRAM link, the SBA traffic,
 *    the TBRE's bus-port watch and the TBRE/core-write snoop watch. Without a bench (the SystemC
 *    model, `model_gaps`) a step is announced and skipped, and its result is not checked.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True if `base` lies in the revocable window (main and sec SRAM). */
bool eg2_rv_revocable(uint32_t base);

/** Sealing(c): sealing-format permissions with SE, US or U0. */
bool eg2_rv_sealing(eg2_cap_t c);

/**
 * The capability a CLC through `auth` of the memory value `mem` returns, with `revbm` the bitmap
 * bit of the base's granule: CHERIoT-Sail's LoadCapImm (eg2_cap_clc_result), then the expression.
 */
eg2_cap_t eg2_rv_expect(eg2_cap_t auth, eg2_cap_t mem, bool revbm);

/** True if the expression revokes `c` (as loaded, before revocation) with bitmap bit `revbm`. */
bool eg2_rv_revokes(eg2_cap_t c, bool revbm);

// --------------------------------------- Bench steps -----------------------------------------

/** The request counts the bench took between eg2_rv_count_begin and eg2_rv_count_end. */
typedef struct eg2_rv_counts {
  uint32_t core;    // core data port (rv_core_ibex to cheriot_mem_sys), A handshakes in the window
  uint32_t xbar;    // cheriot_mem_sys to the crossbar, A handshakes in the window
  uint32_t bitmap;  // the core's bitmap port (corerevbm), A handshakes
  bool valid;       // a bench answered (false without a bench)
} eg2_rv_counts_t;

/**
 * The bitmap reads the bench counts from now on: those of the bitmap words covering the granules
 * of [lo, hi) (heap addresses), so that the test's own capability loads (stack, globals, the trap
 * record) are not counted. Without it, every bitmap read is counted.
 */
bool eg2_rv_count_revbm(uint32_t lo, uint32_t hi, bool bench);
/** Starts counting requests to [lo, hi) and bitmap reads. */
bool eg2_rv_count_begin(uint32_t lo, uint32_t hi, bool bench);
/** Stops counting; `out` gets the counts. */
bool eg2_rv_count_end(eg2_rv_counts_t *out, bool bench);

/** The bench's response delays on the SRAM link (main, sec and meta SRAM ports), on or off. */
bool eg2_rv_delays(bool on, bool bench);

/** The bench's SBA reads of sec SRAM in the background (crossbar contention), on or off. */
bool eg2_rv_sba_noise(bool on, bool bench);

/** The bench watches the TBRE's bus port: reads inside and outside [lo, hi). */
bool eg2_rv_tbre_watch_begin(uint32_t lo, uint32_t hi, bool bench);
/** Stops the watch; `inside` and `outside` get the read counts. */
bool eg2_rv_tbre_watch_end(uint32_t *inside, uint32_t *outside, bool bench);

/**
 * The bench counts core writes to a capability the TBRE has in flight (from the read of its lower
 * word until its words are resolved, approximated by the bench as 16 cycles after the read).
 */
bool eg2_rv_snoop_watch_begin(bool bench);
bool eg2_rv_snoop_watch_end(uint32_t *hits, bool bench);

// ---------------------------------------- Op windows -----------------------------------------
//
// A bitmap bit the test sets must be set only while no call runs: a callee that spills and
// restores a register holding a capability based in a marked granule strips it (the RTOS
// revocation tests' rule). An op window is a list of stores, loads, CLCs and polls that
// eg2_rv_run (eg2_revoke.S) runs with no call and no other code in between: the test builds it,
// runs it, and checks its out words afterwards, once every bit it set is clear again. CHERIoT
// mode only.

/** What an op does; ops that produce a value write it to the next out word. */
typedef enum eg2_rv_op_kind {
  kEg2RvOpSw = 1,         // store `value` at `addr`
  kEg2RvOpLw = 2,         // load `addr` -> out
  kEg2RvOpClcTag = 3,     // CLC from `addr`, CGetTag -> out
  kEg2RvOpPollClear = 4,  // load `addr` until (word & value) == 0 -> out: polls left (0: never)
  kEg2RvOpPollEq = 5,     // load `addr` until word == value -> out: polls left (0: never)
  kEg2RvOpSpin = 6,       // `value` loop iterations
  kEg2RvOpMcycle = 7,     // mcycle -> out
  kEg2RvOpPollSet = 8,    // load `addr` until (word & value) != 0 -> out: polls left (0: never)
  kEg2RvOpWfi = 9,        // wfi
} eg2_rv_op_kind_t;

/** The authority an op's address is set on. */
typedef enum eg2_rv_auth {
  kEg2RvAuthHeap = 0,   // main and sec SRAM (0x1000_0000 to 0x1003_0000)
  kEg2RvAuthRevbm = 1,  // the revocation bitmap window (kEg2MsMetaBase, 3 KiB)
  kEg2RvAuthRegs = 2,   // cheriot_mem_sys's registers
  kEg2RvAuthAck = 3,    // the bench's acknowledgement words (kEg2BenchAck, in RRAM)
  kEg2RvAuthPlic = 4,   // rv_plic
  kEg2RvAuthTimer = 5,  // rv_timer
  kEg2RvAuthRstmgr = 6, // rstmgr
  kEg2RvAuthIbex = 7,   // rv_core_ibex's configuration registers
  kEg2RvAuthPwrmgr = 8, // pwrmgr
  kEg2RvAuthAon = 9,    // aon_timer
  kEg2RvAuthCount = 16,
} eg2_rv_auth_t;

/** A window under construction: its ops are in test-owned memory at `base`, three words each. */
typedef struct eg2_rv_ops {
  uint32_t base;
  uint32_t n;
  uint32_t max;
  uint32_t outs;  // out words the window writes
  bool full;      // an op did not fit (the test fails it)
} eg2_rv_ops_t;

void eg2_rv_ops_init(eg2_rv_ops_t *o, uint32_t base, uint32_t max_ops);
/** Appends an op; returns the index of the out word it writes (0xffffffff if none). */
uint32_t eg2_rv_op(eg2_rv_ops_t *o, eg2_rv_op_kind_t kind, eg2_rv_auth_t auth, uint32_t addr,
                   uint32_t value);
/** Appends the writes of a bitmap word. */
void eg2_rv_op_revbm(eg2_rv_ops_t *o, uint32_t word, uint32_t value);
/** Appends a TBRE start (TBRE_BASE_ADDR, TBRE_NUM_CAPS, TBRE_START). */
void eg2_rv_op_sweep(eg2_rv_ops_t *o, uint32_t base, uint32_t n_caps);
/** Appends a wait for TBRE_STATUS.busy to clear; returns its out word. */
uint32_t eg2_rv_op_sweep_wait(eg2_rv_ops_t *o);
/** Runs the window; its out words go to `out` (test-owned memory). */
void eg2_rv_ops_run(const eg2_rv_ops_t *o, uint32_t out);
/** The address of the bench's acknowledgement words (kEg2BenchAck: [0] step, [1] result). */
uint32_t eg2_rv_ack_addr(void);

// ------------------------- Bench steps of the later corner cases ----------------------------
// (chip_sw_eg2_revocation_corner_bench_vseq, chip_sw_eg2_revocation_corners_vseqs.sv)

enum {
  /** What the bench writes to kEg2BenchAck[1] once a TBRE hold has taken effect. */
  kEg2RvHoldMagic = 0x484f4c44u,
};

/**
 * The bench writes bitmap word `word_addr` (revbm window) over SBA, alternately with bit `bit`
 * set and with 0, until eg2_rv_race_toggle_end, and records the bit as each of the core's bitmap
 * reads of that word saw it.
 */
bool eg2_rv_race_toggle_begin(uint32_t word_addr, uint32_t bit, bool bench);
/**
 * Stops the writes (the word is 0 again when it returns); `reads` gets the core's bitmap reads
 * of the word since the begin, `seen` bit k the bit in read k's response (k < 32).
 */
bool eg2_rv_race_toggle_end(uint32_t *reads, uint32_t *seen, bool bench);

/**
 * Arms a hold of the TBRE: when it presents a read at or above `addr`, the bench holds its
 * request (the A channel of cheriot_mem_sys's tbre port, back pressure only) and writes
 * kEg2RvHoldMagic to kEg2BenchAck[1]; a core write to `release_addr` lets it go on.
 */
bool eg2_rv_tbre_hold_arm(uint32_t addr, uint32_t release_addr, bool bench);
/** How long the last hold lasted, in cycles (0: it never took effect). */
bool eg2_rv_tbre_hold_end(uint32_t *cycles, bool bench);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_REVOKE_H_
