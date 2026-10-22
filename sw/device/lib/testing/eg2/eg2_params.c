// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_params.h"

#include "sw/device/lib/testing/eg2/eg2_result.h"

// Written by chip_sw_eg2_base_vseq before the CPU starts.
OT_USED OT_SECTION(".rodata") volatile const uint32_t kEg2Seed = 0;
// The bench's active mutation (+eg2_mutate), NUL-padded; empty without one.
OT_USED OT_SECTION(".rodata") volatile const char kEg2MutationName[kEg2MutationNameSize] = {0};
static char mutation_name[kEg2MutationNameSize];

// A test without parameters gets an empty table.
OT_WEAK const eg2_param_t kEg2Params[1];
OT_WEAK const size_t kEg2ParamCount = 0;

static uint32_t rand_state;

uint32_t eg2_seed(void) { return kEg2Seed; }

uint32_t eg2_rand(void) {
  // xorshift32: the state is never 0.
  uint32_t x = rand_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rand_state = x;
  return x;
}

uint32_t eg2_rand_range(uint32_t lo, uint32_t hi) {
  uint32_t span = hi - lo + 1;
  return span == 0 ? eg2_rand() : lo + eg2_rand() % span;
}

static void put_line(const char *what, const char *name, uint32_t value) {
  eg2_puts("EG2 ");
  eg2_puts(what);
  eg2_puts(name);
  eg2_puts(" = ");
  eg2_putdec(value);
  eg2_puts(" (0x");
  eg2_puthex(value);
  eg2_puts(")\r\n");
}

const char *eg2_mutation_active(void) {
  for (size_t i = 0; i + 1 < kEg2MutationNameSize; ++i) {
    mutation_name[i] = kEg2MutationName[i];
  }
  mutation_name[kEg2MutationNameSize - 1] = '\0';
  return mutation_name;
}

bool eg2_params_begin(void) {
  if (eg2_mutation_active()[0] != '\0') {
    eg2_puts("EG2 mutation ");
    eg2_puts(mutation_name);
    eg2_puts("\r\n");
  }
  uint32_t seed = kEg2Seed;
  rand_state = seed != 0 ? seed : 1;
  put_line("seed", seed != 0 ? "" : " (none written, default 1)", seed);

  bool ok = true;
  for (size_t i = 0; i < kEg2ParamCount; ++i) {
    const eg2_param_t *p = &kEg2Params[i];
    uint32_t value = *p->value;
    put_line("param ", p->name, value);
    if (value < p->min || value > p->max) {
      eg2_puts("EG2 param ");
      eg2_puts(p->name);
      eg2_puts(" out of range [");
      eg2_putdec(p->min);
      eg2_puts(", ");
      eg2_putdec(p->max);
      eg2_puts("]\r\n");
      ok = false;
    }
  }
  return ok;
}
