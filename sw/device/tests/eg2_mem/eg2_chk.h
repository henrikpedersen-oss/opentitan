// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_MEM_EG2_CHK_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_MEM_EG2_CHK_H_

/**
 * @file
 * @brief What the memory, RRAM, debug and bus tests share (eg2_mem, eg2_rram, eg2_debug, eg2_tl).
 *
 *  - Checks with one console format per area: `<AREA> <step> 0x<v>`, and on a mismatch
 *    `<AREA> FAIL <what> exp 0x<e> got 0x<g>`, then `<AREA> summary checks <n> failed <m>`.
 *    Built with `EG2_TEST_INJECT_FAIL` (every `_inject_fail` build of these areas) the first value
 *    check of the run is made against its expected value with bit 0 flipped, so the run must fail
 *    with `<AREA> FAIL` (REQ_STE_03). Each test source states `EG2_CHK_INJECT_FAIL_DEFINE;`.
 *  - Skips: a check the SystemC model cannot evaluate is skipped with `model_gaps` = 1 and logged
 *    as `<AREA> model_gaps skip <what>`; on the chip `model_gaps` stays 0.
 *  - Bench synchronisation: `EG2_CHK_SYNC("...")` writes the literal to the DV log
 *    (`LOG_INFO`), which the test's vseq waits for (`cfg.sw_logger_vif.printed_log`) before it
 *    injects a fault or drives JTAG. The software then waits `eg2_chk_delay()` before it uses the
 *    injected state, so the bench's zero-time backdoor access is done first.
 *  - Calls that may trap: `eg2_chk_call` jumps to code at a bus address (an executable capability
 *    from PCC, the executable root, in CHERIoT mode) under its own trap handler; a trap is
 *    recorded and the call returns. `eg2_chk_sram_call` does the same from a copy of itself in
 *    sec SRAM, with a register written before the jump and restored after it: the RRAM `EXEC`
 *    tests switch off fetching from the memory the test itself runs from.
 *
 * Interrupts must be off around a call (`eg2_chk_irq_off`): the handlers take none.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/cheriot_mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EG2_TEST_INJECT_FAIL
#define EG2_CHK_INJECT_FAIL_DEFINE const bool kEg2ChkInjectFail = true
#else
#define EG2_CHK_INJECT_FAIL_DEFINE const bool kEg2ChkInjectFail = false
#endif
extern const bool kEg2ChkInjectFail;

/** Writes the literal `msg` to the DV log; the test's vseq waits for exactly this text. */
#define EG2_CHK_SYNC(msg) LOG_INFO(msg)

/** Names the area in every line (`RRAM`, `MEM`, `DEBUG`, `TL`); call first. */
void eg2_chk_area(const char *area);
/** A step on the console: `<AREA> <what> 0x<value>`. */
void eg2_chk_step(const char *what, uint32_t value);
/** A value check; see the file comment for the injected failure. */
bool eg2_chk_eq(const char *what, uint32_t exp, uint32_t got);
/** A condition check (never injected): `<AREA> FAIL <what>` when false. */
bool eg2_chk_true(const char *what, bool cond);
/** Logs a skipped check (model_gaps). */
void eg2_chk_skip(const char *what);
/** `<AREA> summary checks <n> failed <m>`; true if none failed and at least `min_checks` ran. */
bool eg2_chk_summary(uint32_t min_checks);

/** Spins about `cycles` core cycles (at least 2 per iteration). */
void eg2_chk_delay(uint32_t cycles);

/** A pointer for a bus address; in CHERIoT mode bounded to the address's bus window. */
static inline volatile uint32_t *eg2_chk_ptr(uint32_t addr) {
  return (volatile uint32_t *)OT_MMIO_ADDR_TO_PTR(addr);
}
static inline uint32_t eg2_chk_rd32(uint32_t addr) { return *eg2_chk_ptr(addr); }
static inline void eg2_chk_wr32(uint32_t addr, uint32_t v) { *eg2_chk_ptr(addr) = v; }

/** The address a pointer holds, as an integer (a capability's address in CHERIoT mode). */
#ifdef __CHERIOT__
#define eg2_chk_addr_of(p) ((uint32_t)__builtin_cheri_address_get((const volatile void *)(p)))
#else
#define eg2_chk_addr_of(p) ((uint32_t)(uintptr_t)(p))
#endif

/** mstatus.MIE off / on. */
static inline void eg2_chk_irq_off(void) { __asm__ volatile("csrci mstatus, 8"); }
static inline void eg2_chk_irq_on(void) { __asm__ volatile("csrsi mstatus, 8"); }

/** fence.i: the iCache and the prefetch buffer drop what they hold. */
static inline void eg2_chk_fence_i(void) { __asm__ volatile("fence.i" ::: "memory"); }

/** The cycle counter (mcycle, low word). */
static inline uint32_t eg2_chk_cycles(void) {
  uint32_t c;
  __asm__ volatile("csrr %0, mcycle" : "=r"(c));
  return c;
}

/** The result of a call that may trap. */
typedef struct eg2_chk_trap {
  uint32_t mcause;  // 0: no trap (the code returned)
  uint32_t value;   // a0 the code returned (0 after a trap)
  uint32_t mtval;   // mtval of the trap (0 without one)
} eg2_chk_trap_t;

/**
 * Calls the code at bus address `target` with a0 = `arg` (`jalr ra` / `cjalr cra`, an executable
 * capability from PCC in CHERIoT mode), under a trap handler of its own. The code returns with
 * `ret` (`c.jr ra`, the same encoding in both modes) or traps. Returns `mcause`, 0 without a trap.
 */
uint32_t eg2_chk_call(uint32_t target, uint32_t arg, eg2_chk_trap_t *out);

/** The loads and stores of `eg2_chk_access`. */
typedef enum eg2_chk_op {
  kEg2ChkLb = 0,
  kEg2ChkLh = 1,
  kEg2ChkLw = 2,
  kEg2ChkLbu = 3,
  kEg2ChkLhu = 4,
  kEg2ChkSb = 5,
  kEg2ChkSh = 6,
  kEg2ChkSw = 7,
} eg2_chk_op_t;

/** RISC-V and CHERIoT trap causes. */
enum {
  kEg2ChkCauseFetchFault = 1,
  kEg2ChkCauseIllegal = 2,
  kEg2ChkCauseLoadFault = 5,
  kEg2ChkCauseStoreFault = 7,
  kEg2ChkCauseCheri = 0x1c,
};

/**
 * One load or store `op` at `addr` (store data `wdata`) under a trap handler of its own. Returns
 * `mcause`, 0 without a trap; `out->value` is the loaded value.
 */
uint32_t eg2_chk_access(volatile void *addr, uint32_t op, uint32_t wdata, eg2_chk_trap_t *out);

/**
 * Copies the SRAM-call routine to sec SRAM at `sram_addr` (256-byte aligned, at most
 * `kEg2ChkSramCallBytes`), with `fence.i`. Sec SRAM's execution enable must be on.
 */
enum { kEg2ChkSramCallBytes = 512 };
bool eg2_chk_sram_call_install(uint32_t sram_addr);

/**
 * From the copy at `sram_addr`: writes `value` to the register at bus address `reg`, `fence.i`,
 * calls `target` as eg2_chk_call does, then writes `restore` to `reg` and `fence.i` again before
 * it returns into the caller. Returns `mcause`, 0 without a trap.
 */
uint32_t eg2_chk_sram_call(uint32_t sram_addr, uint32_t reg, uint32_t value, uint32_t restore,
                           uint32_t target, eg2_chk_trap_t *out);

/**
 * A routine to call: `addi a0, a0, 1; c.jr ra` as raw words, the same in both modes, so a test
 * can copy it to RRAM or SRAM and check it ran (a0 + 1 returned).
 */
enum {
  kEg2ChkStubAddi = 0x00150513,     // addi a0, a0, 1
  kEg2ChkStubRetNop = 0x00018082,   // c.jr ra ; c.nop
};

/** The address of the test image's own `eg2_chk_stub_inc` (the routine above, in .text). */
uint32_t eg2_chk_stub_inc_addr(void);

/**
 * Test parameters without the framework's table (eg2_regmem's `EG2_RM_PARAMS`, repeated here so
 * these areas do not depend on eg2_regmem): `kEg2Param_<name>` constants in .rodata, which the
 * vseq and eg2sim's `--param` write by symbol, printed and range-checked by
 * `eg2_chk_params_begin()`, the first call of the test's body.
 */
bool eg2_chk_param(const char *name, uint32_t value, uint32_t min, uint32_t max);

#define EG2_CHK_PARAM_DEFINE_(name, dflt, lo, hi) \
  OT_USED OT_SECTION(".rodata") static volatile const uint32_t kEg2Param_##name = (dflt);
#define EG2_CHK_PARAM_CHECK_(name, dflt, lo, hi) \
  ok = eg2_chk_param(#name, kEg2Param_##name, (lo), (hi)) && ok;
#define EG2_CHK_PARAMS(LIST)                \
  LIST(EG2_CHK_PARAM_DEFINE_)               \
  static bool eg2_chk_params_begin(void) {  \
    bool ok = true;                         \
    LIST(EG2_CHK_PARAM_CHECK_)              \
    return ok;                              \
  }

/** A weighted pick from `n` weights with eg2_rand(); n if every weight is 0. */
uint32_t eg2_chk_pick(const uint32_t *weights, uint32_t n);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_MEM_EG2_CHK_H_
