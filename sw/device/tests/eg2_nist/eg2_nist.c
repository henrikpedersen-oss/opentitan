// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_nist/eg2_nist.h"

#include "sw/device/lib/testing/eg2/eg2_test.h"

enum {
  kMaxFiles = 64,
  kMaxVecs = 1024,
};

// Kept small: in the CHERIoT build every global beyond 2 KiB from CGP is reached through
// `ct.auicgp`, which the toolchain's linker resolves wrongly (REQ_STE_12, `make check-cheriot`); the
// framework's globals already take 3.5 KiB of the 4 KiB that stay clear of it.
static uint8_t passed[kMaxFiles], failed[kMaxFiles], skipped[kMaxFiles];
static uint8_t ran[kMaxVecs / 8];

static bool was_run(uint32_t i) { return ran[i / 8] & (1u << (i % 8)); }

static void put_vec(const eg2_nist_vec_t *v) {
  eg2_puts("NIST ");
  eg2_puts(&kEg2NistFileNames[kEg2NistFileName[v->file]]);
  eg2_puts(v->flags & kEg2NistMonte ? " Monte COUNT " : " COUNT/Len ");
  eg2_putdec(v->count);
}

static void put_bytes(const uint8_t *b, uint32_t len) {
  static const char kHex[] = "0123456789abcdef";
  char s[3] = {0, 0, 0};
  for (uint32_t i = 0; i < len; ++i) {
    s[0] = kHex[b[i] >> 4];
    s[1] = kHex[b[i] & 0xf];
    eg2_puts(s);
  }
}

eg2_nist_result_t eg2_nist_skip(const eg2_nist_vec_t *v, const char *why) {
  put_vec(v);
  eg2_puts(": skipped, ");
  eg2_puts(why);
  eg2_puts("\r\n");
  return kEg2NistSkipped;
}

bool eg2_nist_error(const eg2_nist_vec_t *v, const char *what) {
  put_vec(v);
  eg2_puts(": ");
  eg2_puts(what);
  eg2_puts("\r\n");
  return false;
}

bool eg2_nist_expect(const eg2_nist_vec_t *v, int field, const uint8_t *got, uint32_t len,
                     bool corrupt) {
  const uint8_t *exp = eg2_nist_data(v, field);
  bool ok = true;
  for (uint32_t i = 0; i < len; ++i) {
    uint8_t e = exp[i];
    if (corrupt && i == 0) {
      e ^= 1;
    }
    if (got[i] != e) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    put_vec(v);
    eg2_puts(": mismatch\r\n  got      ");
    put_bytes(got, len);
    eg2_puts("\r\n  expected ");
    if (corrupt && len > 0) {
      eg2_puts("(the NIST value; compared with its first bit flipped, -D..._INJECT_FAIL) ");
    }
    put_bytes(exp, len);
    eg2_puts("\r\n");
  }
  return ok;
}

uint32_t eg2_nist_word(const uint8_t *b, uint32_t nbytes, uint32_t i) {
  uint32_t at = nbytes - 4 * (i + 1);
  return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) | ((uint32_t)b[at + 2] << 8) |
         b[at + 3];
}

static bool run_one(eg2_nist_run_fn_t fn, uint32_t idx, bool *corrupt) {
  const eg2_nist_vec_t *v = &kEg2NistVecs[idx];
  // One line per vector before it runs, so a run that times out shows how far it got (2026-10-27:
  // aes_kw and aes_gcm printed nothing between their parameters and the 1 h timeout).
  eg2_puts("NIST vector ");
  eg2_putdec(idx);
  eg2_puts(" ");
  eg2_puts(&kEg2NistFileNames[kEg2NistFileName[v->file]]);
  eg2_puts("\r\n");
  eg2_nist_result_t r = fn(v, *corrupt);
  ran[idx / 8] = (uint8_t)(ran[idx / 8] | (1u << (idx % 8)));
  if (r == kEg2NistSkipped) {
    ++skipped[v->file];
    return true;  // the corruption waits for a vector that runs
  }
  *corrupt = false;
  if (r == kEg2NistPassed) {
    ++passed[v->file];
  } else {
    ++failed[v->file];
  }
  return r == kEg2NistPassed;
}

bool eg2_nist_run(const char *engine, eg2_nist_run_fn_t fn, uint32_t boundary_draws,
                  uint32_t random_draws, bool inject_fail) {
  if (kEg2NistNumFiles > kMaxFiles || kEg2NistNumVecs > kMaxVecs) {
    eg2_puts("NIST table larger than the runner's limits\r\n");
    return false;
  }
  bool corrupt = inject_fail;
  bool ok = true;
  uint32_t total = 0, nrandom = 0, nboundary = 0;
  for (uint32_t i = 0; i < kEg2NistNumVecs; ++i) {
    if (kEg2NistVecs[i].flags & kEg2NistBoundary) {
      ++nboundary;
    } else {
      ++nrandom;
    }
  }
  // The boundary vectors: all of them with boundary_draws 0 (or at least their number), else that many
  // drawn by the seed, so that runs with different seeds cover the set between them. All of them in
  // one run did not fit a simulation (2026-10-27: aes_kw has 43, aes_gcm 213; on Verilator, about
  // 16M cycles an hour, and on Xcelium both timed out at 8 h).
  eg2_puts("NIST ");
  eg2_puts(engine);
  if (boundary_draws == 0 || boundary_draws >= nboundary) {
    eg2_puts(" boundary: all ");
    eg2_putdec(nboundary);
    eg2_puts("\r\n");
    for (uint32_t i = 0; i < kEg2NistNumVecs; ++i) {
      if (kEg2NistVecs[i].flags & kEg2NistBoundary) {
        ok = run_one(fn, i, &corrupt) && ok;
        ++total;
      }
    }
  } else {
    eg2_puts(" boundary: ");
    eg2_putdec(boundary_draws);
    eg2_puts(" of ");
    eg2_putdec(nboundary);
    eg2_puts(" drawn\r\n");
    for (uint32_t d = 0; d < boundary_draws; ++d) {
      uint32_t pick = eg2_rand_range(0, nboundary - d - 1);
      for (uint32_t i = 0; i < kEg2NistNumVecs; ++i) {
        if ((kEg2NistVecs[i].flags & kEg2NistBoundary) && !was_run(i)) {
          if (pick-- == 0) {
            ok = run_one(fn, i, &corrupt) && ok;
            ++total;
            break;
          }
        }
      }
    }
  }
  // The random draws: distinct vectors from the rest of the pool, chosen by the seed.
  eg2_puts("NIST ");
  eg2_puts(engine);
  eg2_puts(" draws:");
  for (uint32_t d = 0; d < random_draws && d < nrandom; ++d) {
    uint32_t pick = eg2_rand_range(0, nrandom - d - 1);
    for (uint32_t i = 0; i < kEg2NistNumVecs; ++i) {
      if (!(kEg2NistVecs[i].flags & kEg2NistBoundary) && !was_run(i)) {
        if (pick-- == 0) {
          eg2_puts(" ");
          eg2_putdec(i);
          ok = run_one(fn, i, &corrupt) && ok;
          ++total;
          break;
        }
      }
    }
  }
  eg2_puts("\r\n");

  uint32_t nfail = 0;
  for (uint32_t f = 0; f < kEg2NistNumFiles; ++f) {
    eg2_puts(failed[f] ? "NIST FAIL " : passed[f] ? "NIST pass " : "NIST skip ");
    eg2_puts(&kEg2NistFileNames[kEg2NistFileName[f]]);
    eg2_puts(": ");
    eg2_putdec(passed[f]);
    eg2_puts(" passed, ");
    eg2_putdec(failed[f]);
    eg2_puts(" failed, ");
    eg2_putdec(skipped[f]);
    eg2_puts(" skipped\r\n");
    nfail += failed[f];
    if (passed[f] + failed[f] + skipped[f] == 0) {
      eg2_puts("NIST FAIL: a vector file has no vector\r\n");
      ok = false;
    }
  }
  // The algorithm's vectors this test does not run: reported, never passed.
  const char *nr = kEg2NistNotRun;
  for (uint32_t i = 0; i < kEg2NistNotRunCount; ++i) {
    eg2_puts("NIST not run ");
    eg2_puts(nr);
    eg2_puts("\r\n");
    while (*nr != '\0') {
      ++nr;
    }
    ++nr;
  }
  if (corrupt) {
    // An inject-fail build in which no vector ran has checked nothing: it must not pass.
    eg2_puts("NIST FAIL: inject-fail build, but no vector ran to take the corrupted value\r\n");
    ok = false;
  }
  eg2_puts("NIST ");
  eg2_puts(engine);
  eg2_puts(": ");
  eg2_putdec(total);
  eg2_puts(" vectors, ");
  eg2_putdec(nfail);
  eg2_puts(" failed\r\n");
  return ok;
}
