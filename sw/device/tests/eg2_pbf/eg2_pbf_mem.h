// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_MEM_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_MEM_H_

/**
 * @file
 * @brief What the memory, debug, ROM_EXT and CHERIoT post-boot tests share (REQ_PBF_14, 19, 20,
 * 22).
 *
 *  - Bench steps: the software asks the chip bench for a stimulus by writing
 *    `kEg2PbfBenchCode | step` to the DV status word (rv_core_ibex DV_SIM_WINDOW[0], the bench's
 *    sw_test_status), waits, and writes "in test" back. The bench (the test's vseq) acts in the
 *    same simulation step as it sees the code, so the software's next instruction already sees
 *    the stimulus. In the SystemC model nobody answers: the step is announced and the test skips
 *    what depends on it (`model_gaps`).
 *  - Memories the tests may use freely, by bus address from the generated top description:
 *    retention SRAM above `retention_sram_t` (its upper 4 KiB, which no boot stage uses), the
 *    secure SRAM, the main SRAM between the globals and the stack, the OTTF non-volatile scratch
 *    area of the RRAM.
 *  - The RRAM controller (rram_ctrl programmer's guide): a data region of the test's own, program,
 *    read, rewrite.
 *  - The SRAM controllers' key renewal and initialisation (sram_ctrl theory of operation).
 *  - The life cycle state and whether it enables debug (lc_ctrl theory of operation, the
 *    HW_DEBUG_EN column of the life cycle signal table).
 *  - Escalation straight to the escalation reset (no NMI: the CHERIoT build ends the test on any
 *    trap), the alert and CPU crash dumps of rstmgr.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  /** The DV status code of a bench step: kEg2PbfBenchCode | step (step < 0x100). */
  kEg2PbfBenchCode = 0x5000u,  // the bench sees the low 16 bits (sw_test_status_e)
  /** Retention SRAM above retention_sram_t: 4 KiB no boot stage uses. */
  kEg2PbfRetFree = TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 0x1000u,
  kEg2PbfRetFreeSize = 0x0f00u,  // below eg2_memsys' record at the top 256 bytes
  /** Main SRAM between the globals (RISC-V: static_critical, .data and .bss end below 0x2200;
   * CHERIoT: under 4 KiB) and the stack (the top 16 KiB). */
  kEg2PbfMainFree = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR + 0x3000u,
  kEg2PbfMainFreeSize = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES - 0x3000u - 0x5000u,
  kEg2PbfRramPage = 512,
};

/** Asks the bench for step `step` (see above). */
void eg2_pbf_bench_step(uint32_t step);

/** The life cycle state (lc_ctrl LC_STATE) and its name. */
uint32_t eg2_pbf_lc_state(void);
const char *eg2_pbf_lc_name(uint32_t lc_state);
/** Whether the state enables debug (TEST_UNLOCKED*, DEV, RMA: HW_DEBUG_EN on). */
bool eg2_pbf_lc_debug_enabled(uint32_t lc_state);

/** The OTTF non-volatile scratch area of the RRAM (bus address) and its size. */
uint32_t eg2_pbf_nv_scratch(void);
uint32_t eg2_pbf_nv_scratch_size(void);

/**
 * A data region over `npages` pages from the page of bus address `addr`, read and write enabled,
 * scrambling and ECC as given: the highest-numbered region that is neither locked nor enabled.
 * Returns false if none is free (the boot stages took or locked them all).
 */
bool eg2_pbf_rram_region(uint32_t addr, uint32_t npages, bool scramble, bool ecc);

/** Programs `n` words (a multiple of 4) at bus address `addr` (16-byte aligned). */
bool eg2_pbf_rram_program(uint32_t addr, const uint32_t *words, uint32_t n);
/** Reads `n` words through the controller (not the host window). */
bool eg2_pbf_rram_ctrl_read(uint32_t addr, uint32_t *words, uint32_t n);
/** A Rewrite of the RRAM word at `addr`. */
bool eg2_pbf_rram_rewrite(uint32_t addr);
/** rram_ctrl ERR_CODE, and clears it. */
uint32_t eg2_pbf_rram_err_code(void);

/**
 * sram_ctrl CTRL with RENEW_SCR_KEY and/or INIT; waits for SCR_KEY_VALID (renew) and INIT_DONE
 * (init). Returns false if the controller did not get there or reported INIT_ERROR.
 */
bool eg2_pbf_sram_op(eg2_dev_id_t regs, bool renew_key, bool init);

/** Arms rstmgr's alert and CPU crash dumps for the next reset. */
void eg2_pbf_crashdump_arm(void);
/** The alert crash dump's cause bit of `alert` (after the reset). */
bool eg2_pbf_crashdump_alert(size_t alert);
/** Word `i` of the CPU crash dump (0 mtval, 1 mepc, 2 last data address, 3 next PC, 4 PC). */
uint32_t eg2_pbf_crashdump_cpu(uint32_t i);

/**
 * Class A escalates the given alerts on their first occurrence, phase 0 straight to escalation
 * signal 3 (the escalation reset) after `cycles`, the crash dump latched at phase 0, locked.
 */
void eg2_pbf_escalate_reset(const uint32_t *alerts, size_t n, uint32_t cycles);

/** The alert's index by name; fails the test (returns 0 and prints) if it does not exist. */
uint32_t eg2_pbf_alert(const char *name);

/** A 32-bit word at a bus address (a window capability in CHERIoT mode). */
static inline volatile uint32_t *eg2_pbf_word(uint32_t addr) {
  return (volatile uint32_t *)eg2_rm_ptr(addr);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_MEM_H_
