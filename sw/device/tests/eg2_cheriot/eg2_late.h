// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_LATE_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_LATE_H_

/**
 * @file
 * @brief The late-switch start-up of the CHERIoT suite: tests that act in RISC-V mode before the
 * mode switch, and check the switch itself (chip_sw_cheriot_handover, _epmp_handover,
 * chip_sw_rv_core_ibex_cheriot_switch, chip_sw_cheriot_reset_state, _meta_init_after_reset).
 *
 * The framework's CHERIoT start-up code (eg2/cheriot/eg2_cheriot_start.S) switches before any of
 * the test runs. A late-switch image is a CHERIoT (purecap) image with its own start-up code
 * (eg2_late_start.S): `_ottf_start` jumps to the test's `eg2_late_pre`, assembly that runs in
 * RISC-V mode with only the encodings both modes share. It either switches and locks CHERIoT mode
 * and continues at `eg2_late_cheriot` (stack, globals, capability relocations, then
 * `eg2_late_main`: the framework's parameter check, status module, the test's `eg2_test_main`,
 * the final sweep and the verdict), or it ends the test in RISC-V mode itself (`eg2_late_rv_end`).
 *
 * The test's knobs are read by its assembly before any C runs, so they are defined in assembly
 * (`EG2_LATE_PARAM_ASM_`, global symbols in .rodata, where the bench writes them) and declared to
 * C with `EG2_LATE_TEST_PARAMS`, from one X-macro list shared by both.
 *
 * The capture area (main SRAM, `_stack_start - 0x1000`, below the stack and above the globals,
 * which no start-up step writes): what a test's assembly records around the switch for its C to
 * check, with each register stored whole by CSC so its tag is kept.
 */

#define EG2_LATE_AREA_BELOW_STACK 0x1000

// Capture area layout (byte offsets).
#define EG2_LATE_X(i) (8 * (i))      // x1..x15 as CSC stored them (slot 0 unused)
#define EG2_LATE_PCC 128             // AUIPCC 0 in the capture code
#define EG2_LATE_MTCC 136
#define EG2_LATE_MEPCC 144
#define EG2_LATE_MTDC 152            // its address set to the capture area by the capture code
#define EG2_LATE_MSCRATCHC 160
#define EG2_LATE_INFO 168            // 22 words of the test's own
#define EG2_LATE_FILL 256            // 32 words: the values x0..x31 were given before the switch
#define EG2_LATE_AREA_SIZE 384

#ifdef __ASSEMBLER__

// A knob in .rodata, global so the test's C (and the bench) find it.
#define EG2_LATE_PARAM_ASM_(name, dflt, lo, hi) \
  .globl kEg2Param_##name;                      \
  .type kEg2Param_##name, @object;              \
  .balign 4;                                    \
  kEg2Param_##name : .word dflt;                \
  .size kEg2Param_##name, 4;

#else  // !__ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_params.h"

#define EG2_LATE_PARAM_EXTERN_(name, dflt, lo, hi) extern volatile const uint32_t kEg2Param_##name;
#define EG2_LATE_PARAM_ROW_(name, dflt, lo, hi) {#name, &kEg2Param_##name, (lo), (hi)},

/** The knob table of a late-switch test (the knobs themselves are its assembly's). */
#define EG2_LATE_TEST_PARAMS(LIST)                         \
  LIST(EG2_LATE_PARAM_EXTERN_)                             \
  const eg2_param_t kEg2Params[] = {LIST(EG2_LATE_PARAM_ROW_)}; \
  const size_t kEg2ParamCount = sizeof(kEg2Params) / sizeof(kEg2Params[0])

/** The test's body, as in a framework test. */
bool eg2_test_main(void);

/** The capture area's address. */
uint32_t eg2_late_area(void);

/** A word of the capture area. */
uint32_t eg2_late_word(uint32_t offset);

#ifdef __CHERIOT__
/** A capability of the capture area, loaded with CLC (tag kept). */
void *eg2_late_cap(uint32_t offset);
#endif

#endif  // __ASSEMBLER__

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_LATE_H_
