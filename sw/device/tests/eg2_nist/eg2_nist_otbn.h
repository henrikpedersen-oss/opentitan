// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_OTBN_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_OTBN_H_

/**
 * @file
 * @brief What the NIST CAVP tests on OTBN share: the vector budget and the model gap.
 *
 * One OTBN operation takes 0.3 to 2.2 million OTBN cycles (spec "Reference level for OTBN"),
 * so a chip run cannot take every boundary vector. The vectors are grouped by file, curve or
 * modulus, and operation and expected result (sign, valid, invalid); of each group at most
 * `per_group` run, drawn with the seed among the group's boundary vectors, so the regression's
 * seeds cover the boundary set together. Every vector left out is reported "skipped: budget" by
 * the runner, never passed.
 *
 * On the SystemC model the vectors run on its OTBN (eg2_model/ip/eg2_otbn.h); `make
 * nist-otbn-ref` runs every vector on the Python ISS as well.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/tests/eg2_nist/eg2_nist.h"

/** Counts the boundary vectors of each group. */
void eg2_nist_otbn_budget_init(uint32_t per_group);

/** Whether the vector runs within the budget; else it is reported skipped (budget). */
bool eg2_nist_otbn_take(const eg2_nist_vec_t *v);

/** Skips a vector left out by the budget. Returns kEg2NistSkipped. */
eg2_nist_result_t eg2_nist_otbn_skip(const eg2_nist_vec_t *v);

/** Little-endian words from a vector's field (the generator wrote the integer little endian). */
void eg2_nist_otbn_words(const eg2_nist_vec_t *v, int field, uint32_t *words, uint32_t nwords);

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_OTBN_H_
