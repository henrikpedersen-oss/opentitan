// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_REGMEM_EG2_REGMEM_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_REGMEM_EG2_REGMEM_H_

/**
 * @file
 * @brief What the register, memory and connectivity tests share (eg2_regmem).
 *
 *  - `eg2_rm_ptr`: the pointer for a bus address, in both execution modes (a capability bounded
 *    to the address's bus window in CHERIoT mode, from the framework's window table).
 *  - `eg2_rm_probe`: one load or store that may fault. The framework's trap handler ends the test
 *    on any trap, so the probe puts its own handler in place (`mtvec` in RISC-V mode, MTCC in
 *    CHERIoT mode) for the one access and the framework's back after it. A trap resumes after the
 *    access with `mcause` returned and `mtval` recorded; interrupts must be off (the handler
 *    takes no interrupt).
 *  - `eg2_rm_pick`: a weighted draw from the software's seed.
 *
 * The tables are in the generated `eg2_regmem_tables.h`, which only a test's own source includes
 * (they are large: each test binary has one copy).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/cheriot_mmio.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Probe operations: the load or store `eg2_rm_probe` performs. */
typedef enum eg2_rm_op {
  kEg2RmLb = 0,
  kEg2RmLh = 1,
  kEg2RmLw = 2,
  kEg2RmLbu = 3,
  kEg2RmLhu = 4,
  kEg2RmSb = 5,
  kEg2RmSh = 6,
  kEg2RmSw = 7,
} eg2_rm_op_t;

/** What a probe returns besides the trap cause. */
typedef struct eg2_rm_out {
  uint32_t value;  // the loaded value (0 if the load trapped)
  uint32_t mtval;  // mtval of the trap (0 without one)
} eg2_rm_out_t;

/**
 * Performs one access `op` at `addr` (store data `wdata`). Returns 0, or `mcause` if the access
 * trapped. Interrupts must be disabled.
 */
uint32_t eg2_rm_probe(volatile void *addr, uint32_t op, uint32_t wdata, eg2_rm_out_t *out);

#ifdef __CHERIOT__
/** A capability load (`op` 0, CLC) or store (`op` 1, CSC of `wcap`), as eg2_rm_probe. */
typedef struct eg2_rm_cap_out {
  void *cap;       // the loaded capability
  uint32_t mtval;  // mtval of the trap (0 without one)
} eg2_rm_cap_out_t;
uint32_t eg2_rm_probe_cap(volatile void *addr, uint32_t op, void *wcap, eg2_rm_cap_out_t *out);
#endif

/** RISC-V and CHERIoT trap causes the tests expect. */
enum {
  kEg2RmCauseLoadFault = 5,
  kEg2RmCauseStoreFault = 7,
  kEg2RmCauseCheri = 0x1c,
};

/** The pointer for a bus address; in CHERIoT mode bounded to the address's window. */
static inline volatile void *eg2_rm_ptr(uint32_t addr) { return OT_MMIO_ADDR_TO_PTR(addr); }

/** Reads and writes a 32-bit word at a bus address. */
static inline uint32_t eg2_rm_rd32(uint32_t addr) {
  return *(volatile uint32_t *)eg2_rm_ptr(addr);
}
static inline void eg2_rm_wr32(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *)eg2_rm_ptr(addr) = value;
}

/** Disables interrupts (mstatus.MIE): the probes' handler takes none. */
static inline void eg2_rm_irq_off(void) { __asm__ volatile("csrci mstatus, 8"); }

/** A draw from `n` weights (0 = never); returns n if every weight is 0. */
static inline uint32_t eg2_rm_pick(const uint32_t *weights, uint32_t n) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < n; ++i) {
    total += weights[i];
  }
  if (total == 0) {
    return n;
  }
  uint32_t r = eg2_rand_range(0, total - 1);
  for (uint32_t i = 0; i < n; ++i) {
    if (r < weights[i]) {
      return i;
    }
    r -= weights[i];
  }
  return n - 1;
}

/** Prints a label and a hex value. */
static inline void eg2_rm_hex(const char *label, uint32_t value) {
  eg2_puts(label);
  eg2_puts("0x");
  eg2_puthex(value);
}

/**
 * Test parameters without the framework's parameter table.
 *
 * `EG2_TEST_PARAMS` keeps a table of pointers (name, value) in .data: 24 bytes per knob in the
 * CHERIoT build, whose globals must stay under 4 KiB (the framework already uses most of it; see
 * the CGP note in doc/eg2_test_framework.md). These tests have many knobs, so they declare them
 * with `EG2_RM_PARAMS(LIST)` instead: the same `kEg2Param_<name>` constants in .rodata, which the
 * bench's vseq and eg2sim's `--param` write by symbol, and the same `EG2 param` lines and range
 * check, made by code (`eg2_rm_params_begin()`, first thing in the test's body) rather than from a
 * table. `EG2_PARAM(name)` reads them as usual.
 */
bool eg2_rm_param(const char *name, uint32_t value, uint32_t min, uint32_t max);

#define EG2_RM_PARAM_DEFINE_(name, dflt, lo, hi) \
  OT_USED OT_SECTION(".rodata") static volatile const uint32_t kEg2Param_##name = (dflt);
#define EG2_RM_PARAM_CHECK_(name, dflt, lo, hi) \
  ok = eg2_rm_param(#name, kEg2Param_##name, (lo), (hi)) && ok;
#define EG2_RM_PARAMS(LIST)                     \
  LIST(EG2_RM_PARAM_DEFINE_)                    \
  static bool eg2_rm_params_begin(void) {       \
    bool ok = true;                             \
    LIST(EG2_RM_PARAM_CHECK_)                   \
    return ok;                                  \
  }

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_REGMEM_EG2_REGMEM_H_
