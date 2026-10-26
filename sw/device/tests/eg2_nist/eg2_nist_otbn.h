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
 * The SystemC model has no OTBN (decision 6: its reference is the Python ISS, which
 * `make nist-otbn-ref` runs on every vector): with `model_gaps` = 1 every vector is reported
 * skipped ("model_gap: no OTBN in the SystemC model"), and the inject-fail build then fails
 * because no vector took the corruption (eg2_nist_run). The chip runs and their `_inject_fail`
 * builds are these tests' evidence.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/tests/eg2_nist/eg2_nist.h"

/** Counts the boundary vectors of each group. */
void eg2_nist_otbn_budget_init(uint32_t per_group);

/** Whether the vector runs within the budget; else it is reported skipped (budget). */
bool eg2_nist_otbn_take(const eg2_nist_vec_t *v);

/** Skips a vector: the model gap or the budget. Returns kEg2NistSkipped. */
eg2_nist_result_t eg2_nist_otbn_skip(const eg2_nist_vec_t *v, bool model_gap);

/** Little-endian words from a vector's field (the generator wrote the integer little endian). */
void eg2_nist_otbn_words(const eg2_nist_vec_t *v, int field, uint32_t *words, uint32_t nwords);

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_OTBN_H_
