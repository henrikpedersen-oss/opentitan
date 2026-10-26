// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The chip bench's side of the EG2 noise source (eg2_rng_source.h): `+eg2_rng=<source>`, read by
// tb/eg2_rng_src.svh, which forces entropy_src's noise input with these symbols. Compiled only in
// build mode en_eg2_rng (chip_eg2_rng_tests.hjson). One source per simulation.

#include <stdio.h>

#include "eg2_rng_source.h"

static eg2_rng_t g_rng;
static int g_active;

/** Selects the source; 0 on success, -1 (with the reason on stderr) if the description is bad. */
int eg2_rng_select(const char *spec) {
  char err[256] = {0};
  if (eg2_rng_parse(&g_rng, spec, err, sizeof(err)) != 0) {
    fprintf(stderr, "EG2_RNG: +eg2_rng=%s: %s\n", spec, err);
    g_active = 0;
    return -1;
  }
  g_active = g_rng.kind != kEg2RngModel;
  printf("EG2_RNG source %s%s\n", spec,
         g_rng.kind == kEg2RngHost ? (g_rng.use_rdrand ? " (RDRAND)" : " (getrandom)") : "");
  fflush(stdout);
  return 0;
}

/** Whether a source other than AST's model is selected. */
int eg2_rng_active(void) { return g_active; }

/** The next symbol (0..15); -1 when a recorded stream has ended. */
int eg2_rng_symbol(void) {
  int s = eg2_rng_next(&g_rng);
  if (g_rng.n == 64) {
    printf("EG2_RNG first 64 symbols of %s: %08x %08x %08x %08x %08x %08x %08x %08x\n",
           g_rng.desc, g_rng.first_words[0], g_rng.first_words[1], g_rng.first_words[2],
           g_rng.first_words[3], g_rng.first_words[4], g_rng.first_words[5],
           g_rng.first_words[6], g_rng.first_words[7]);
    fflush(stdout);
  }
  return s;
}

/** The number of symbols given out since the last select. */
long long eg2_rng_count(void) { return (long long)g_rng.n; }

/** Closes a recording; called at the end of the run. */
void eg2_rng_finish(void) {
  if (g_active) {
    printf("EG2_RNG %s: %llu symbols given out\n", g_rng.desc, (unsigned long long)g_rng.n);
    fflush(stdout);
  }
  eg2_rng_close(&g_rng);
}
