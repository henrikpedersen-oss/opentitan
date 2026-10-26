// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_nist/eg2_nist_otbn.h"

#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"

enum {
  kMaxGroups = 32,
  kGroupFlags = kEg2NistFail | kEg2NistSign,
};

// Small: the CHERIoT build's globals stay under 4 KiB.
static uint32_t group_key[kMaxGroups];
static uint16_t group_left[kMaxGroups];
static uint16_t group_taken[kMaxGroups];
static uint32_t num_groups;
static uint32_t budget;

static uint32_t key_of(const eg2_nist_vec_t *v) {
  return ((uint32_t)v->file << 24) | ((uint32_t)v->param << 8) | (v->flags & kGroupFlags);
}

static uint32_t group_of(const eg2_nist_vec_t *v) {
  uint32_t k = key_of(v);
  for (uint32_t g = 0; g < num_groups; ++g) {
    if (group_key[g] == k) {
      return g;
    }
  }
  if (num_groups < kMaxGroups) {
    group_key[num_groups] = k;
    group_left[num_groups] = 0;
    group_taken[num_groups] = 0;
    return num_groups++;
  }
  return kMaxGroups - 1;
}

void eg2_nist_otbn_budget_init(uint32_t per_group) {
  budget = per_group;
  num_groups = 0;
  for (uint32_t i = 0; i < kEg2NistNumVecs; ++i) {
    if (kEg2NistVecs[i].flags & kEg2NistBoundary) {
      ++group_left[group_of(&kEg2NistVecs[i])];
    }
  }
}

bool eg2_nist_otbn_take(const eg2_nist_vec_t *v) {
  uint32_t g = group_of(v);
  uint32_t need = budget > group_taken[g] ? budget - group_taken[g] : 0;
  bool take;
  if (!(v->flags & kEg2NistBoundary)) {
    take = need > 0;  // a random draw of the runner: within the budget
  } else {
    // Selection sampling: `need` of the `left` boundary vectors, each subset equally likely.
    uint32_t left = group_left[g];
    take = need > 0 && left > 0 && eg2_rand_range(0, left - 1) < need;
    if (left > 0) {
      --group_left[g];
    }
  }
  if (take) {
    ++group_taken[g];
  }
  return take;
}

eg2_nist_result_t eg2_nist_otbn_skip(const eg2_nist_vec_t *v, bool model_gap) {
  return eg2_nist_skip(v, model_gap ? "model_gap: no OTBN in the SystemC model" : "budget");
}

void eg2_nist_otbn_words(const eg2_nist_vec_t *v, int field, uint32_t *words, uint32_t nwords) {
  memset(words, 0, nwords * sizeof(uint32_t));
  uint32_t n = eg2_nist_len(v, field);
  if (n > nwords * sizeof(uint32_t)) {
    n = nwords * sizeof(uint32_t);
  }
  memcpy(words, eg2_nist_data(v, field), n);
}
