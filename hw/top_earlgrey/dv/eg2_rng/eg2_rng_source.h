// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The noise source of the EG2 random-number tests (eg2_top_verification spec "Random Number
// Generation Test Suite", REQ_PBF_24): the 4-bit symbols that drive entropy_src's noise input
// (RNG_BUS_WIDTH 4) in place of AST's behavioural model. One header, used by the chip bench's
// DPI (eg2_rng_dpi.c, `+eg2_rng=<source>`) and by the SystemC model (`eg2sim --rng <source>`), so
// both take the same symbols from the same description.
//
// Sources:
//   model             AST's behavioural model (the bench forces nothing; the model keeps its own
//                     generator).
//   host              The simulation host's CPU random generator: RDRAND where the CPU has it,
//                     else getrandom(). Each byte gives two symbols, its low nibble first.
//   file:<path>       A recorded stream (as `record` writes it): two symbols per byte, low first.
//   pattern:<name>    A deterministic pattern (a xorshift64 generator, seed `seed=<n>` or 1, fills
//                     the lanes a pattern leaves free):
//     stuck0, stuck1           every symbol 0x0, or 0xf
//     repcnt:<n>               lane 0 alternates runs of exactly n ones and n zeros; lanes 1-3
//                              free. The longest run of one value on a lane is n.
//     repcnts:<n>              runs of exactly n equal symbols, each run's symbol the previous
//                              one inverted on every lane (so every lane also runs exactly n).
//     adaptp:<ones>:<w>        windows of w symbols; in each, every lane is 1 in exactly `ones`
//                              symbols (the first `ones` of the window) and 0 in the rest.
//     bucket:<count>:<w>       windows of w symbols; in each, symbol 0x0 appears `count` times
//                              (first), the others cycle 0x1..0xf; no other symbol appears more
//                              often than 0x0 when count >= w / 16.
//     markov:<sw>:<w>          windows of w symbols (w / 2 non-overlapping pairs per lane); on
//                              every lane, `sw` pairs are switching ("01") and the rest "00".
//     biased:<pct>             each bit 1 with probability pct / 100.
//     periodic:<p>             a block of p free symbols repeated.
//     random                   free symbols only (the pattern generator, not the host).
//   Options, comma-separated after the source: `seed=<n>`, `record=<path>` (every symbol given
//   out is written there, two per byte, low nibble first).
//
// The windows count symbols from the first symbol the source gives out; a test that needs them
// aligned with entropy_src's health-test windows restarts the source (eg2_rng_select) when it
// enables entropy_src.

#ifndef EG2_RNG_EG2_RNG_SOURCE_H_
#define EG2_RNG_EG2_RNG_SOURCE_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum eg2_rng_kind {
  kEg2RngModel = 0,
  kEg2RngHost,
  kEg2RngFile,
  kEg2RngStuck0,
  kEg2RngStuck1,
  kEg2RngRepcnt,
  kEg2RngRepcnts,
  kEg2RngAdaptp,
  kEg2RngBucket,
  kEg2RngMarkov,
  kEg2RngBiased,
  kEg2RngPeriodic,
  kEg2RngRandom,
};

typedef struct eg2_rng {
  int kind;
  uint64_t a, b;       // the pattern's parameters
  uint64_t n;          // symbols given out
  uint64_t prng;       // xorshift64 state
  uint8_t buf[4096];   // host or file bytes
  size_t buf_len, buf_pos;
  int nibble_hi;       // the next symbol is the high nibble of buf[buf_pos]
  int use_rdrand;
  FILE *in;            // file:
  FILE *rec;           // record=
  uint8_t rec_byte;
  int rec_half;
  uint8_t period[4096];  // periodic: the repeated block (one symbol per byte)
  unsigned last_sym;     // repcnts
  char desc[256];
  uint32_t first_words[8];  // the first 64 symbols, for the run log
} eg2_rng_t;

static inline uint64_t eg2_rng_xorshift(eg2_rng_t *r) {
  uint64_t x = r->prng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  r->prng = x;
  return x;
}

static inline unsigned eg2_rng_free_sym(eg2_rng_t *r) {
  return (unsigned)(eg2_rng_xorshift(r) >> 60) & 0xf;
}

static inline int eg2_rng_cpu_has_rdrand(void) {
#if defined(__x86_64__) || defined(__i386__)
  unsigned a = 1, b = 0, c = 0, d = 0;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
  return (c >> 30) & 1;
#else
  return 0;
#endif
}

/** Fills the host buffer: RDRAND (retried as Intel's guide says) or getrandom(). */
static inline int eg2_rng_host_fill(eg2_rng_t *r) {
#if defined(__x86_64__)
  if (r->use_rdrand) {
    for (size_t i = 0; i < sizeof(r->buf); i += 8) {
      uint64_t v = 0;
      unsigned char ok = 0;
      for (int t = 0; t < 10 && !ok; ++t) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
      }
      if (!ok) {
        r->use_rdrand = 0;  // fall back to getrandom() for the rest of the run
        break;
      }
      memcpy(r->buf + i, &v, 8);
    }
    if (r->use_rdrand) {
      r->buf_len = sizeof(r->buf);
      r->buf_pos = 0;
      return 1;
    }
  }
#endif
#if defined(__linux__)
  size_t got = 0;
  while (got < sizeof(r->buf)) {
    ssize_t k = getrandom(r->buf + got, sizeof(r->buf) - got, 0);
    if (k <= 0) return 0;
    got += (size_t)k;
  }
  r->buf_len = got;
  r->buf_pos = 0;
  return 1;
#else
  return 0;
#endif
}

static inline int eg2_rng_file_fill(eg2_rng_t *r) {
  if (!r->in) return 0;
  r->buf_len = fread(r->buf, 1, sizeof(r->buf), r->in);
  r->buf_pos = 0;
  return r->buf_len > 0;
}

/** The next symbol from the byte buffer (host, file); -1 at the end of a file. */
static inline int eg2_rng_byte_sym(eg2_rng_t *r) {
  if (r->buf_pos >= r->buf_len) {
    int ok = r->kind == kEg2RngHost ? eg2_rng_host_fill(r) : eg2_rng_file_fill(r);
    if (!ok) return -1;
  }
  unsigned b = r->buf[r->buf_pos];
  unsigned s = r->nibble_hi ? (b >> 4) : (b & 0xf);
  if (r->nibble_hi) ++r->buf_pos;
  r->nibble_hi = !r->nibble_hi;
  return (int)s;
}

static inline void eg2_rng_close(eg2_rng_t *r) {
  if (r->rec) {
    if (r->rec_half) fputc(r->rec_byte, r->rec);
    fclose(r->rec);
  }
  if (r->in) fclose(r->in);
  r->rec = NULL;
  r->in = NULL;
}

static inline uint64_t eg2_rng_num(const char **p) {
  uint64_t v = strtoull(*p, (char **)p, 0);
  if (**p == ':') ++*p;
  return v;
}

/**
 * Parses a source description; 0 on success, else -1 with the reason in err. Resets the
 * stream: the windows and the symbol count start again.
 */
static inline int eg2_rng_parse(eg2_rng_t *r, const char *spec, char *err, size_t errlen) {
  eg2_rng_close(r);
  memset(r, 0, sizeof(*r));
  r->prng = 1;
  snprintf(r->desc, sizeof(r->desc), "%s", spec);
  char head[256];
  snprintf(head, sizeof(head), "%s", spec);
  // Options after the first comma.
  char *opts = strchr(head, ',');
  if (opts) *opts++ = '\0';
  while (opts && *opts) {
    char *next = strchr(opts, ',');
    if (next) *next++ = '\0';
    if (strncmp(opts, "seed=", 5) == 0) {
      r->prng = strtoull(opts + 5, NULL, 0);
      if (r->prng == 0) r->prng = 1;
    } else if (strncmp(opts, "record=", 7) == 0) {
      r->rec = fopen(opts + 7, "wb");
      if (!r->rec) {
        snprintf(err, errlen, "cannot write %s", opts + 7);
        return -1;
      }
    } else {
      snprintf(err, errlen, "unknown option %s", opts);
      return -1;
    }
    opts = next;
  }
  const char *p = head;
  if (strcmp(p, "model") == 0) {
    r->kind = kEg2RngModel;
  } else if (strcmp(p, "host") == 0) {
    r->kind = kEg2RngHost;
    r->use_rdrand = eg2_rng_cpu_has_rdrand();
  } else if (strncmp(p, "file:", 5) == 0) {
    r->kind = kEg2RngFile;
    r->in = fopen(p + 5, "rb");
    if (!r->in) {
      snprintf(err, errlen, "cannot read %s", p + 5);
      return -1;
    }
  } else if (strncmp(p, "pattern:", 8) == 0) {
    p += 8;
    struct {
      const char *name;
      int kind;
      int nargs;
    } const kPatterns[] = {
        {"stuck0", kEg2RngStuck0, 0},   {"stuck1", kEg2RngStuck1, 0},
        {"repcnt:", kEg2RngRepcnt, 1},  {"repcnts:", kEg2RngRepcnts, 1},
        {"adaptp:", kEg2RngAdaptp, 2},  {"bucket:", kEg2RngBucket, 2},
        {"markov:", kEg2RngMarkov, 2},  {"biased:", kEg2RngBiased, 1},
        {"periodic:", kEg2RngPeriodic, 1}, {"random", kEg2RngRandom, 0},
    };
    size_t i;
    for (i = 0; i < sizeof(kPatterns) / sizeof(kPatterns[0]); ++i) {
      size_t len = strlen(kPatterns[i].name);
      if (strncmp(p, kPatterns[i].name, len) == 0 &&
          (kPatterns[i].nargs > 0 || p[len] == '\0')) {
        r->kind = kPatterns[i].kind;
        p += len;
        if (kPatterns[i].nargs >= 1) r->a = eg2_rng_num(&p);
        if (kPatterns[i].nargs >= 2) r->b = eg2_rng_num(&p);
        break;
      }
    }
    if (i == sizeof(kPatterns) / sizeof(kPatterns[0]) || *p != '\0') {
      snprintf(err, errlen, "unknown pattern %s", head + 8);
      return -1;
    }
    if ((r->kind == kEg2RngRepcnt || r->kind == kEg2RngRepcnts || r->kind == kEg2RngPeriodic) &&
        r->a == 0) {
      snprintf(err, errlen, "%s: the length must be at least 1", head);
      return -1;
    }
    if ((r->kind == kEg2RngAdaptp || r->kind == kEg2RngBucket || r->kind == kEg2RngMarkov) &&
        (r->b == 0 || r->a > r->b || (r->kind == kEg2RngMarkov && 2 * r->a > r->b))) {
      snprintf(err, errlen, "%s: the count must fit in the window", head);
      return -1;
    }
    if (r->kind == kEg2RngPeriodic) {
      if (r->a > sizeof(r->period)) {
        snprintf(err, errlen, "%s: at most %zu symbols", head, sizeof(r->period));
        return -1;
      }
      for (uint64_t k = 0; k < r->a; ++k) r->period[k] = (uint8_t)eg2_rng_free_sym(r);
    }
    if (r->kind == kEg2RngRepcnts) r->last_sym = eg2_rng_free_sym(r);
  } else {
    snprintf(err, errlen, "unknown source %s (model, host, file:<path>, pattern:<name>)", head);
    return -1;
  }
  return 0;
}

/** The next 4-bit symbol; -1 when a recorded stream ends. */
static inline int eg2_rng_next(eg2_rng_t *r) {
  int s;
  uint64_t n = r->n;
  switch (r->kind) {
    case kEg2RngHost:
    case kEg2RngFile:
      s = eg2_rng_byte_sym(r);
      break;
    case kEg2RngStuck0:
      s = 0x0;
      break;
    case kEg2RngStuck1:
      s = 0xf;
      break;
    case kEg2RngRepcnt:
      s = (int)((eg2_rng_free_sym(r) & 0xe) | (((n / r->a) & 1) ? 0u : 1u));
      break;
    case kEg2RngRepcnts:
      if (n != 0 && n % r->a == 0) r->last_sym ^= 0xf;
      s = (int)r->last_sym;
      break;
    case kEg2RngAdaptp:
      s = (n % r->b) < r->a ? 0xf : 0x0;
      break;
    case kEg2RngBucket: {
      uint64_t i = n % r->b;
      s = i < r->a ? 0x0 : (int)(1 + (i - r->a) % 15);
      break;
    }
    case kEg2RngMarkov: {
      // Pair k of the window: "01" for k < a, else "00".
      uint64_t i = n % r->b;
      s = ((i / 2) < r->a && (i & 1)) ? 0xf : 0x0;
      break;
    }
    case kEg2RngBiased: {
      unsigned v = 0;
      for (int bit = 0; bit < 4; ++bit) {
        if ((eg2_rng_xorshift(r) >> 11) % 100 < r->a) v |= 1u << bit;
      }
      s = (int)v;
      break;
    }
    case kEg2RngPeriodic:
      s = r->period[n % r->a];
      break;
    case kEg2RngRandom:
    default:
      s = (int)eg2_rng_free_sym(r);
      break;
  }
  if (s < 0) return s;
  if (n < 64) r->first_words[n / 8] |= (uint32_t)s << (4 * (n % 8));
  if (r->rec) {
    if (!r->rec_half) {
      r->rec_byte = (uint8_t)s;
    } else {
      fputc(r->rec_byte | (uint8_t)(s << 4), r->rec);
    }
    r->rec_half = !r->rec_half;
  }
  ++r->n;
  return s;
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // EG2_RNG_EG2_RNG_SOURCE_H_
