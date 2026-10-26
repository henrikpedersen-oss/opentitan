// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_CHERIOT_RAND_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_CHERIOT_RAND_H_

/**
 * @file
 * @brief `eg2_cheriot_rand`, the CHERIoT suite's shared library (EG2 spec "CHERIoT Test Suite",
 * "Shared classes").
 *
 *  - Class weights by the suite's weight rule, drawn with `eg2_rand()` so a seed reproduces them,
 *    and class picks by weight.
 *  - The test's tally: operations planned and completed (liveness), checks failed, and one
 *    console line per mismatch (the first `kEg2MaxReports`).
 *  - Memory the test owns, outside the image's globals: sec SRAM, a slice of main SRAM below the
 *    stack and the top KiB of retention SRAM, reached by address in RISC-V mode and through a
 *    capability derived from the memory root in CHERIoT mode.
 *  - CHERIoT mode: capability pairs, the hardware's capability next to the model's
 *    (eg2_cap_model.h), changed only by instructions that run on both, and checked field by
 *    field.
 *
 * Globals are kept small: the CHERIoT toolchain's linker mis-resolves CGP-relative accesses once
 * an image's data and BSS pass 4 KiB, and the framework already uses most of that.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------- Weights -------------------------------------------

enum { kEg2FamilyMax = 16 };

/** A class family's weights. `plain_mask` marks the plain (non-boundary) classes. */
typedef struct eg2_family {
  uint32_t n;
  uint32_t plain_mask;
  uint32_t boosted;  // the boosted class, or n if none
  uint32_t w[kEg2FamilyMax];
} eg2_family_t;

/** The `boost` knob: 0 draws (one run in five boosts a class), this value boosts none. */
enum { kEg2BoostNone = 255 };

/**
 * Draws a family's weights by the suite's weight rule: every class at least 1; with a boost, the
 * boosted class is half of the total; otherwise the boundary classes at least 60 % and the plain
 * ones at most 30 % and at least 5 %. `boost` is the knob: 0 draws, kEg2BoostNone boosts nothing,
 * k boosts class k - 1 (modulo the family's size).
 */
void eg2_family_draw(eg2_family_t *f, uint32_t n, uint32_t plain_mask, uint32_t boost);

/** A class by weight. */
uint32_t eg2_family_pick(const eg2_family_t *f);

/** Prints `EG2 weights <name>: w0 w1 ... [boost k]`. */
void eg2_family_print(const char *name, const eg2_family_t *f);

/**
 * An operation count: `param` if the bench (or a pin) set it, else drawn as the suite's tables
 * say for `n_ops`: lo at 20 %, lo + 1 to mid at 60 %, mid + 1 to hi at 20 %.
 */
uint32_t eg2_draw_count(uint32_t param, uint32_t lo, uint32_t mid, uint32_t hi);

/** One of `n` values uniformly (0 for n <= 1). */
uint32_t eg2_pick(uint32_t n);

// ----------------------------------------- Tally ---------------------------------------------

// Mismatch lines printed per tally (the UART costs about 1,000 instructions a character).
#ifndef EG2_MAX_REPORTS
#define EG2_MAX_REPORTS 4
#endif
enum { kEg2MaxReports = EG2_MAX_REPORTS };

typedef struct eg2_tally {
  uint32_t checks;
  uint32_t failed;
  uint32_t planned;
  uint32_t done;
} eg2_tally_t;

/** Counts a check; on a mismatch prints `MISMATCH <what> #<idx>: got <got> expected <exp>`. */
bool eg2_check(eg2_tally_t *t, const char *what, uint32_t idx, uint32_t got, uint32_t exp);

/** Prints one phase's line: `EG2 <phase>: <done>/<planned> done, <checks> checks, <failed> failed`. */
void eg2_tally_print(const char *phase, const eg2_tally_t *t);

/** True if nothing failed and every planned operation was done (liveness). */
bool eg2_tally_ok(const eg2_tally_t *t);

void eg2_put_kv(const char *key, uint32_t value);

// ----------------------------------------- Memory --------------------------------------------

/** The test-owned memories (EG2 spec memory map). */
enum {
  kEg2SecBase = 0x10020000u,
  kEg2SecSize = 0x10000u,
  kEg2MainBase = 0x10000000u,
  kEg2MainSize = 0x20000u,
  kEg2RetBase = 0x40600000u,
  kEg2RetSize = 0x2000u,
  kEg2RomBase = 0x40000u,
  kEg2RomSize = 0x30000u,
  kEg2RramBase = 0x30000000u,
  kEg2RramSize = 0x200000u,
  // Unmapped: below rv_dm's memory, and above the last window.
  kEg2UnmappedLow = 0x00000000u,
  kEg2UnmappedHigh = 0xfffff000u,
  // sec SRAM layout used by the suite: arena A at the start, the work area in the middle, arena
  // B at the end. The arenas include sec SRAM's first and last granules.
  kEg2ArenaSize = 0x2000u,
  kEg2ArenaA = kEg2SecBase,
  kEg2ArenaB = kEg2SecBase + kEg2SecSize - kEg2ArenaSize,
  kEg2WorkBase = kEg2SecBase + kEg2ArenaSize,
  kEg2WorkSize = kEg2SecSize - 2 * kEg2ArenaSize,
  // The top KiB of retention SRAM (the owner area; untagged).
  kEg2RetArena = kEg2RetBase + kEg2RetSize - 0x400u,
  kEg2RetArenaSize = 0x400u,
  kEg2MainArenaSize = 0x800u,
};

/**
 * Zeroes sec SRAM through sram_ctrl_sec's hardware initialisation, so that no read or sub-word
 * write meets uninitialised memory. Returns false if the initialisation failed.
 */
bool eg2_sec_sram_init(void);

/** A pointer to [addr, addr + len): a bounded capability from the memory root in CHERIoT mode. */
void *eg2_mem(uint32_t addr, uint32_t len);

/** The main SRAM arena: kEg2MainArenaSize bytes below the stack, above the image's BSS. */
uint32_t eg2_main_arena(void);

/** Linker symbols, as addresses. */
uint32_t eg2_sym_stack_start(void);
uint32_t eg2_sym_stack_end(void);
uint32_t eg2_sym_data_start(void);
uint32_t eg2_sym_bss_end(void);

/** The address of a code or data symbol (no capability). */
#ifdef __CHERIOT__
#define EG2_ADDR(sym) ((uint32_t)__builtin_cheri_address_get(sym))
#else
#define EG2_ADDR(sym) ((uint32_t)(uintptr_t)(sym))
#endif

// --------------------------------- CHERIoT capability pairs -----------------------------------

#ifdef __CHERIOT__

/** The hardware's capability and the model's. */
typedef struct eg2_cp {
  void *hw;
  eg2_cap_t m;
} eg2_cp_t;

/** What the hardware holds: CGetHigh, CGetAddr, CGetTag. */
eg2_cap_t eg2_hw_read(void *c);

/** The roots: MTDC, PCC (AUIPCC 0), and the sealing root MScratchC held at the test's start. */
eg2_cp_t eg2_cp_root_mem(void);
eg2_cp_t eg2_cp_root_exe(void);
eg2_cp_t eg2_cp_root_seal(void);
/** Saves the sealing root; call once at the start, before eg2_probe_begin. */
void eg2_cp_save_sealing_root(void);
/** A hardware value the model knows nothing about yet (taken as the model's starting point). */
eg2_cp_t eg2_cp_adopt(void *hw);

eg2_cp_t eg2_cp_set_addr(eg2_cp_t c, uint32_t addr);
eg2_cp_t eg2_cp_inc(eg2_cp_t c, uint32_t delta);
/** CIncAddrImm with immediate class `which` (0..kEg2IncImmCount - 1). */
enum { kEg2IncImmCount = 7 };
eg2_cp_t eg2_cp_inc_imm(eg2_cp_t c, uint32_t which);
uint32_t eg2_inc_imm_value(uint32_t which);
eg2_cp_t eg2_cp_bounds(eg2_cp_t c, uint32_t len, eg2_bounds_kind_t kind);
/** CSetBoundsImm with immediate class `which` (0..kEg2BoundsImmCount - 1). */
enum { kEg2BoundsImmCount = 7 };
eg2_cp_t eg2_cp_bounds_imm(eg2_cp_t c, uint32_t which);
uint32_t eg2_bounds_imm_value(uint32_t which);
eg2_cp_t eg2_cp_and_perm(eg2_cp_t c, uint32_t mask);
eg2_cp_t eg2_cp_seal(eg2_cp_t c, eg2_cp_t auth);
eg2_cp_t eg2_cp_unseal(eg2_cp_t c, eg2_cp_t auth);
eg2_cp_t eg2_cp_set_high(eg2_cp_t c, uint32_t hi);
eg2_cp_t eg2_cp_clear_tag(eg2_cp_t c);
eg2_cp_t eg2_cp_move(eg2_cp_t c);

/** Checks the hardware against the model: tag, high word and address (the whole register). */
bool eg2_cp_check(eg2_tally_t *t, const char *what, uint32_t idx, eg2_cp_t c);
/** As eg2_cp_check, and CGetBase, CGetTop, CGetLen, CGetPerm, CGetType against the model. */
bool eg2_cp_check_full(eg2_tally_t *t, const char *what, uint32_t idx, eg2_cp_t c);

/** Prints a model capability: `tag hi addr [base, top) perms otype`. */
void eg2_cap_print(const char *what, eg2_cap_t c);

#endif  // __CHERIOT__

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_CHERIOT_RAND_H_
