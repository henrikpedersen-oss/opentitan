// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_PARAMS_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_PARAMS_H_

/**
 * @file
 * @brief Constrained-random test parameters and the software seed (EG2 framework, part 1).
 *
 * A test declares its knobs as one X-macro list, each with a default and the range the test
 * accepts:
 *
 *   #define MY_PARAMS(X) \
 *     X(num_words, 64, 1, 1024) \
 *     X(burst,      4, 1,   16)
 *   EG2_TEST_PARAMS(MY_PARAMS);
 *
 *   ... EG2_PARAM(num_words) ...
 *
 * Each knob is a 32-bit constant `kEg2Param_<name>` in .rodata. The chip bench's vseq
 * (`chip_sw_eg2_base_vseq` and the test's own subclass) draws the values under SystemVerilog
 * constraints and writes them into the image before the CPU starts; `+eg2_param_<name>=<n>` pins
 * one. With no bench (FPGA, silicon) the defaults apply. The constraints live with the bench; the
 * range here is the test's own limit, checked before the test body runs, so a bench and a test
 * that disagree fail at once instead of testing something else.
 *
 * The software seed `kEg2Seed` is written the same way, from the run's seed. `eg2_rand()` draws
 * from it, so every random choice the software makes is reproduced by rerunning the seed.
 * (OpenTitan's `rand_testutils` reseeds from the entropy hardware; it is for values whose
 * reproduction does not matter.)
 *
 * Parameters are plain 32-bit words and hold no pointers: an object with a pointer is not in
 * .rodata in the CHERIoT build, and the bench can only write .rodata.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/base/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eg2_param {
  const char *name;
  const volatile uint32_t *value;
  uint32_t min;
  uint32_t max;
} eg2_param_t;

#define EG2_PARAM_DEFINE_(name, dflt, lo, hi) \
  OT_USED OT_SECTION(".rodata") static volatile const uint32_t kEg2Param_##name = (dflt);
#define EG2_PARAM_ROW_(name, dflt, lo, hi) {#name, &kEg2Param_##name, (lo), (hi)},

/** Defines a test's parameters from an X-macro list of X(name, default, min, max). */
#define EG2_TEST_PARAMS(LIST)                                     \
  LIST(EG2_PARAM_DEFINE_)                                         \
  const eg2_param_t kEg2Params[] = {LIST(EG2_PARAM_ROW_)};        \
  const size_t kEg2ParamCount = sizeof(kEg2Params) / sizeof(kEg2Params[0])

/** A parameter's value, as the bench wrote it (or its default). */
#define EG2_PARAM(name) (kEg2Param_##name)

/** The test's parameter table; empty unless the test uses `EG2_TEST_PARAMS`. */
extern const eg2_param_t kEg2Params[];
extern const size_t kEg2ParamCount;

/**
 * Prints the seed and every parameter, and checks each parameter against its range.
 * Returns false if one is out of range. Called by the framework before the test body.
 */
bool eg2_params_begin(void);

/** Size of the mutation name the bench writes (kEg2MutationName), terminator included. */
enum { kEg2MutationNameSize = 48 };

/**
 * The bench's active mutation (`+eg2_mutate=<name>`, EG2 spec "Mutation"), or "" without one.
 * A mutation changes the hardware, never what a test checks; tests use this only to log it.
 */
const char *eg2_mutation_active(void);

/** The software seed the bench wrote; 0 if none did. */
uint32_t eg2_seed(void);

/** The next value of the software's random sequence (xorshift32 from the seed). */
uint32_t eg2_rand(void);

/** A value in [lo, hi] from the software's random sequence. */
uint32_t eg2_rand_range(uint32_t lo, uint32_t hi);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_PARAMS_H_
