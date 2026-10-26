// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_cap_ops (EG2 verification spec, "Capability instructions"; REQ_CIB_06).
 *
 * Every CHERIoT capability instruction, on capabilities derived from the roots, each result
 * checked against the test's model of the architecture (eg2_cap_model.h, from CHERIoT-Sail): the
 * whole register (tag, high word, address) and, where the instruction reads them, CGetBase,
 * CGetTop, CGetLen, CGetPerm and CGetType. The scoreboard checks every instruction as well.
 *
 * Directed phases first, so that every run reaches the edges:
 *  1. roots: MTDC, PCC, MScratchC and MTCC as the handover leaves them (REQ_SOC_07);
 *  2. bounds at every exponent transition: lengths 2^k - 1, 2^k, 2^k + 1 for k = 0..32 and the
 *     9-bit mantissa's edges (511, 512, 513, 1001, ...), on bases aligned, odd, at 0 and ending at
 *     2^32, with CSetBounds, CSetBoundsExact, CSetBoundsRoundDown, CSetBoundsImm, CRRL and CRAM;
 *  3. representability: CSetAddr, CIncAddr and CIncAddrImm to the base, below it, the top, past
 *     it, the last representable address and the first one outside, 0, 2^32 - 1 and across 2^32,
 *     for exponents 0, 1, 6, 14 and 24;
 *  4. sealing: every object type 0-15 on data and executable capabilities, the authority's
 *     faults (no SE, no US, untagged, sealed, zero length, out of bounds), Global through CUnseal,
 *     and every address and bounds operation on a sealed capability;
 *  5. CAndPerm with every single permission cleared, on every permission format;
 *  6. CSetHigh / CGetHigh over every permission format and exponent encoding, CTestSubset,
 *     CSetEqualExact, CSub, CMove, CClearTag;
 *  7. CSpecialRW of MTDC, MScratchC, MTCC and MEPCC, with MTCC's and MEPCC's legalisation;
 *  8. AUIPCC and AUICGP with every immediate class under small and large bounds.
 * Then `n_ops` random operations by family weight (bounds 20, address 20, CGet* 15, seal 10,
 * CSetHigh/CSetEqualExact/CTestSubset 10, AUIPCC/AUICGP 10, CRRL/CRAM 5, CSpecialRW 5,
 * CMove/CClearTag/CAndPerm 5, each moved by up to 50 % per seed), their operands by shape, cursor
 * and permission class (the suite's weight rule), with the invariants: CGetHigh(CSetHigh(c, x)) =
 * x, a tagged CSetBounds result is a subset of its source, CAndPerm never adds a permission.
 * Liveness: operations completed against planned.
 *
 * Built with EG2_RAND_CAP_OPS_INJECT_FAIL, phase 2 expects CSetBoundsExact of length 1001 (not
 * representable: E = 1 and odd) to keep its tag; the run must fail there (`MISMATCH
 * CSetBoundsExact`), with or without `model_gaps`.
 *
 * `model_gaps` (eg2sim only; 0 on the chip, where every check counts): the differences of
 * eg2sim's MPACT-CHERIoT from CHERIoT-Sail (status/dv_todo.md C2) are compared and printed but not
 * counted, per check:
 *  - CSetBounds, CSetBoundsExact, CSetBoundsRoundDown, CSetBoundsImm and the narrower-source cases
 *    of phase 2 (round-up after the exponent bump, the 14 -> 24 step); CRRL and CRAM count;
 *  - every check of phases 3 (representability: address operations on sealed or untagged sources),
 *    4 (seal: CUnseal's address, CAndPerm on sealed), 5 (andperm) and 6 (high: CSetEqualExact,
 *    CGetTop and CGetLen unsaturated);
 *  - in the random phase the bounds, address, CGet*, seal, CSetHigh and move families; AUIPCC,
 *    AUICGP, CRRL, CRAM and CSpecialRW count;
 *  - no capability based at or above 2^31 is made (addr_hi 0: MPACT faults a CLC of one), and a
 *    random operation whose operand or model result is based there is skipped.
 * Phases 1 (roots), 7 (CSpecialRW) and 8 (AUIPCC, AUICGP) count in full.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

#ifndef __CHERIOT__
#error "chip_sw_eg2_rand_cap_ops runs in CHERIoT mode only"
#endif

EG2_TEST_CONFIG();

// n_ops 0 and boost 0: drawn by the software from the seed (eg2_cheriot_rand.h).
// addr_hi 0 leaves out capabilities based at or above 2^31 (stimulus only, never an expectation):
// eg2sim's MPACT-CHERIoT faults a CLC of one (see the suite's findings in status/dv_todo.md).
#define CAP_OPS_PARAMS(X) \
  X(n_ops, 0, 0, 4096)    \
  X(boost, 0, 0, 255)     \
  X(addr_hi, 1, 0, 1)     \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(CAP_OPS_PARAMS);

// model_gaps: the checks of MPACT-CHERIoT's known differences go to `gap_tally` (see above).
static bool gaps;
static eg2_tally_t gap_tally;
// Checks never subject to model_gaps (the inject-fail build's).
static eg2_tally_t kept;

static eg2_tally_t *gap(eg2_tally_t *t) { return gaps ? &gap_tally : t; }

static bool addr_hi(void) { return EG2_PARAM(addr_hi) != 0 && !gaps; }

extern char _eg2_cheriot_trap[];

// --------------------------------------------------------------------------------------------

static uint32_t hw_crrl(uint32_t len) {
  uint32_t r;
  __asm__ volatile("crrl %0, %1" : "=r"(r) : "r"(len));
  return r;
}

static uint32_t hw_cram(uint32_t len) {
  uint32_t r;
  __asm__ volatile("cram %0, %1" : "=r"(r) : "r"(len));
  return r;
}

static uint32_t hw_subset(void *a, void *b) {
  uint32_t r;
  __asm__ volatile("ctestsubset %0, %1, %2" : "=r"(r) : "C"(a), "C"(b));
  return r;
}

static uint32_t hw_seqx(void *a, void *b) {
  uint32_t r;
  __asm__ volatile("csetequalexact %0, %1, %2" : "=r"(r) : "C"(a), "C"(b));
  return r;
}

// CSub, by its encoding (the assembler emits an integer `sub` for `csub`).
static uint32_t hw_csub(void *a, void *b) {
  uint32_t r;
  __asm__ volatile(".insn r 0x5b, 0, 0x14, %0, %1, %2" : "=r"(r) : "C"(a), "C"(b));
  return r;
}

static uint32_t hw_high(void *a) {
  uint32_t r;
  __asm__ volatile("cgethigh %0, %1" : "=r"(r) : "C"(a));
  return r;
}

static uint32_t hw_perm(void *a) {
  uint32_t r;
  __asm__ volatile("cgetperm %0, %1" : "=r"(r) : "C"(a));
  return r;
}

// Swaps `v` into the SCR, reads it back and restores the old value, in one block.
static void *swap_mtdc(void *v) {
  void *old;
  void *rd;
  __asm__ volatile("cspecialrw %0, mtdc, %2\n cspecialrw %1, mtdc, %0"
                   : "=&C"(old), "=&C"(rd)
                   : "C"(v));
  return rd;
}

static void *swap_mscratchc(void *v) {
  void *old;
  void *rd;
  __asm__ volatile("cspecialrw %0, mscratchc, %2\n cspecialrw %1, mscratchc, %0"
                   : "=&C"(old), "=&C"(rd)
                   : "C"(v));
  return rd;
}

// A data capability [base, base + len) from the memory root.
static eg2_cp_t data_cap(uint32_t base, uint32_t len) {
  return eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_mem(), base), len, kEg2BoundsInexact);
}

// An executable capability [base, base + len) from PCC, with its address at `addr`.
static eg2_cp_t exe_cap(uint32_t base, uint32_t len, uint32_t addr) {
  eg2_cp_t c = eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_exe(), base), len, kEg2BoundsInexact);
  return eg2_cp_set_addr(c, addr);
}

// ------------------------------------- 1. Roots ----------------------------------------------

static void phase_roots(eg2_tally_t *t) {
  eg2_cp_check_full(t, "root MTDC", 0, eg2_cp_root_mem());
  eg2_cp_check_full(t, "root PCC", 0, eg2_cp_root_exe());
  eg2_cp_check_full(t, "root MScratchC", 0, eg2_cp_root_seal());
  eg2_cp_t tcc;
  __asm__ volatile("cspecialr %0, mtcc" : "=C"(tcc.hw));
  tcc.m = eg2_cap_root_exe(EG2_ADDR(_eg2_cheriot_trap));
  eg2_cp_check_full(t, "root MTCC", 0, tcc);
  t->done += 4;
}

// ------------------------------ 2. Exponent transitions ---------------------------------------

enum { kSweepPow = 32 * 3 + 1, kSweepExtra = 10, kSweepLens = kSweepPow + kSweepExtra };

static uint32_t sweep_len(uint32_t i) {
  if (i < kSweepPow) {
    if (i == kSweepPow - 1) {
      return 0xffffffffu;  // 2^32 - 1
    }
    uint32_t k = i / 3;
    uint32_t p = 1u << k;
    return i % 3 == 0 ? p - 1 : (i % 3 == 1 ? p : p + 1);
  }
  switch (i - kSweepPow) {
    case 0:
      return 510;
    case 1:
      return 1001;
    case 2:
      return 1022;
    case 3:
      return 1023;
    case 4:
      return 0x3fe00;  // 511 * 2^9: the largest length at E = 9
    case 5:
      return 0x3fe01;
    case 6:
      return 0x7fc000u;  // 511 * 2^14: the largest length below E = 24
    case 7:
      return 0x7fc001u;
    case 8:
      return 0x00ffffffu;
    default:
      return eg2_rand();
  }
}

static uint32_t sweep_base(uint32_t j, uint32_t len) {
  switch (j) {
    case 0:
      return kEg2SecBase;  // 2^16 aligned
    case 1:
      return kEg2SecBase + 1;  // odd
    case 2:
      return 0;
    case 3:  // top exactly 2^32
      return (addr_hi() || len > 0x80000000u) ? 0u - len : 0x10000000u;
    default:
      return kEg2SecBase + (eg2_rand() & 0xfff8u);
  }
}

static void phase_exponents(eg2_tally_t *t) {
  eg2_cp_t root = eg2_cp_root_mem();
  uint32_t idx = 0;
  for (uint32_t i = 0; i < kSweepLens; ++i) {
    uint32_t len = sweep_len(i);
    eg2_check(t, "CRRL", i, hw_crrl(len), eg2_cap_crrl(len));
    eg2_check(t, "CRAM", i, hw_cram(len), eg2_cap_cram(len));
    for (uint32_t j = 0; j < 5; ++j) {
      eg2_cp_t at = eg2_cp_set_addr(root, sweep_base(j, len));
      for (uint32_t k = 0; k < 3; ++k) {
        eg2_bounds_kind_t kind = (eg2_bounds_kind_t)k;
        eg2_cp_t r = eg2_cp_bounds(at, len, kind);
        eg2_tally_t *ct = gap(t);
#ifdef EG2_RAND_CAP_OPS_INJECT_FAIL
        if (len == 1001 && j == 0 && kind == kEg2BoundsExact) {
          r.m.tag = !r.m.tag;  // a wrong expectation: 1001 is not representable
          ct = &kept;
        }
#endif
        const char *what = k == 0 ? "CSetBounds" : (k == 1 ? "CSetBoundsExact" : "CSetBoundsRoundDown");
        if (i % 3 == 1) {
          eg2_cp_check_full(ct, what, idx, r);
        } else {
          eg2_cp_check(ct, what, idx, r);
        }
        ++idx;
      }
    }
    ++t->done;
  }
  // The immediate form, every immediate class, on each base class.
  for (uint32_t w = 0; w < kEg2BoundsImmCount; ++w) {
    for (uint32_t j = 0; j < 5; ++j) {
      eg2_cp_t at = eg2_cp_set_addr(root, sweep_base(j, eg2_bounds_imm_value(w)));
      eg2_cp_check_full(gap(t), "CSetBoundsImm", w * 5 + j, eg2_cp_bounds_imm(at, w));
    }
  }
  // A narrower source: the requested bounds one byte past it clear the tag.
  eg2_cp_t src = data_cap(kEg2SecBase + 0x100, 0x100);
  eg2_cp_check(gap(t), "CSetBounds within", 0, eg2_cp_bounds(src, 0x100, kEg2BoundsInexact));
  eg2_cp_check(gap(t), "CSetBounds past top", 0, eg2_cp_bounds(src, 0x101, kEg2BoundsInexact));
  eg2_cp_check(gap(t), "CSetBounds below base", 0,
               eg2_cp_bounds(eg2_cp_inc(src, 0xffffffffu), 2, kEg2BoundsInexact));
  eg2_cp_check(gap(t), "CSetBounds at top len 0", 0,
               eg2_cp_bounds(eg2_cp_inc(src, 0x100), 0, kEg2BoundsExact));
  eg2_cp_check(gap(t), "CSetBounds at top len 1", 0,
               eg2_cp_bounds(eg2_cp_inc(src, 0x100), 1, kEg2BoundsExact));
  t->done += 1;
}

// ------------------------------- 3. Representability ----------------------------------------

typedef struct shape_def {
  uint32_t base, len;
} shape_def_t;

static shape_def_t repr_shape(uint32_t s) {
  shape_def_t d;
  switch (s) {
    case 0:
      d.base = kEg2SecBase + 0x100;
      d.len = 100;  // E 0
      break;
    case 1:
      d.base = kEg2SecBase + 0x200;
      d.len = 1001;  // E 1, rounded
      break;
    case 2:
      d.base = kEg2SecBase + 0x1000;
      d.len = 0x4000;  // E 6
      break;
    case 3:
      d.base = 0x10000000u;
      d.len = 1u << 22;  // E 14
      break;
    case 4:
      d.base = 0;
      d.len = 0x01000000u;  // E 24 at 0
      break;
    case 5:
      d.base = 0xffffff00u;
      d.len = 0x100;  // ends at 2^32
      break;
    default:
      d.base = 0;
      d.len = 16;  // starts at 0
      break;
  }
  return d;
}

static uint32_t repr_target(eg2_cap_t c, uint32_t k) {
  uint32_t base = eg2_cap_base(c);
  uint32_t top = (uint32_t)eg2_cap_top(c);  // 2^32 wraps to 0
  uint32_t limit = (uint32_t)eg2_cap_repr_limit(c);
  switch (k) {
    case 0:
      return base;
    case 1:
      return base - 1;
    case 2:
      return top - 1;
    case 3:
      return top;
    case 4:
      return top + 1;
    case 5:
      return limit - 1;
    case 6:
      return limit;
    case 7:
      return limit + 1;
    case 8:
      return 0;
    case 9:
      return 0xffffffffu;
    case 10:
      return base + 0x80000000u;
    default:
      return base + eg2_rand_range(0, 0x3ff);
  }
}

enum { kReprShapes = 7, kReprTargets = 12 };

static void phase_repr(eg2_tally_t *t) {
  uint32_t idx = 0;
  for (uint32_t s = 0; s < kReprShapes; ++s) {
    if (s == 5 && !addr_hi()) {
      ++t->done;  // the shape ending at 2^32 is based above 2^31
      continue;
    }
    shape_def_t d = repr_shape(s);
    eg2_cp_t c = data_cap(d.base, d.len);
    eg2_cp_check_full(t, "repr shape", s, c);
    for (uint32_t k = 0; k < kReprTargets; ++k) {
      uint32_t a = repr_target(c.m, k);
      eg2_cp_check(t, "CSetAddr", idx, eg2_cp_set_addr(c, a));
      eg2_cp_check(t, "CIncAddr", idx, eg2_cp_inc(c, a - c.m.addr));
      // From the target, every immediate (when the target kept its tag the step is from there).
      eg2_cp_t at = eg2_cp_set_addr(c, a);
      eg2_cp_check(t, "CIncAddrImm", idx, eg2_cp_inc_imm(at, idx % kEg2IncImmCount));
      ++idx;
    }
    for (uint32_t w = 0; w < kEg2IncImmCount; ++w) {
      eg2_cp_t top = eg2_cp_set_addr(c, (uint32_t)eg2_cap_top(c.m));
      eg2_cp_check(t, "CIncAddrImm top", w + 16 * s, eg2_cp_inc_imm(top, w));
      eg2_cp_t base = eg2_cp_set_addr(c, eg2_cap_base(c.m));
      eg2_cp_check(t, "CIncAddrImm base", w + 16 * s, eg2_cp_inc_imm(base, w));
    }
    ++t->done;
  }
}

// ----------------------------------------- 4. Sealing -----------------------------------------

static void phase_seal(eg2_tally_t *t) {
  eg2_cp_t seal_root = eg2_cp_root_seal();
  eg2_cp_t data = data_cap(kEg2SecBase + 0x300, 64);
  eg2_cp_t code = exe_cap(EG2_ADDR(eg2_stub_arrive), 64, EG2_ADDR(eg2_stub_arrive));
  uint32_t idx = 0;
  for (uint32_t ot = 0; ot < 16; ++ot) {
    eg2_cp_t auth = eg2_cp_set_addr(seal_root, ot);
    eg2_cp_t sd = eg2_cp_seal(data, auth);
    eg2_cp_t sc = eg2_cp_seal(code, auth);
    eg2_cp_check_full(t, "CSeal data", ot, sd);
    eg2_cp_check_full(t, "CSeal code", ot, sc);
    eg2_cp_check_full(t, "CUnseal data", ot, eg2_cp_unseal(sd, auth));
    eg2_cp_check_full(t, "CUnseal code", ot, eg2_cp_unseal(sc, auth));
    // Sealed: every address and bounds operation clears the tag; sealing twice too.
    eg2_cp_check(t, "sealed CSetAddr", ot, eg2_cp_set_addr(sd, sd.m.addr));
    eg2_cp_check(t, "sealed CIncAddrImm", ot, eg2_cp_inc_imm(sc, 0));
    eg2_cp_check(t, "sealed CSetBounds", ot, eg2_cp_bounds(sd, 8, kEg2BoundsInexact));
    eg2_cp_check(t, "sealed CSeal", ot, eg2_cp_seal(sd, auth));
    eg2_cp_check(t, "sealed CAndPerm ~GL", ot, eg2_cp_and_perm(sd, ~(uint32_t)kEg2PermGl));
    eg2_cp_check(t, "sealed CAndPerm ~LD", ot, eg2_cp_and_perm(sd, ~(uint32_t)kEg2PermLd));
    ++t->done;
  }
  // The authority's faults, for a data type (9) and a sentry type (1).
  for (uint32_t v = 0; v < 2; ++v) {
    uint32_t ot = v == 0 ? 9 : 1;
    eg2_cp_t victim = v == 0 ? data : code;
    eg2_cp_t a = eg2_cp_set_addr(seal_root, ot);
    eg2_cp_t auths[8] = {
        eg2_cp_and_perm(a, ~(uint32_t)kEg2PermSe),
        eg2_cp_and_perm(a, ~(uint32_t)kEg2PermUs),
        eg2_cp_clear_tag(a),
        eg2_cp_seal(eg2_cp_set_addr(seal_root, 10), eg2_cp_set_addr(seal_root, 10)),
        eg2_cp_bounds(a, 0, kEg2BoundsExact),
        eg2_cp_bounds(a, 1, kEg2BoundsExact),
        eg2_cp_inc(eg2_cp_bounds(a, 1, kEg2BoundsExact), 1),
        eg2_cp_and_perm(a, ~(uint32_t)kEg2PermGl),
    };
    eg2_cp_t sealed = eg2_cp_seal(victim, a);
    for (uint32_t k = 0; k < 8; ++k) {
      eg2_cp_check(t, "auth", idx, auths[k]);
      eg2_cp_check_full(t, "CSeal auth", idx, eg2_cp_seal(victim, auths[k]));
      eg2_cp_check_full(t, "CUnseal auth", idx, eg2_cp_unseal(sealed, auths[k]));
      ++idx;
    }
    // Global through CUnseal: a local sealed capability stays local.
    eg2_cp_t local = eg2_cp_seal(eg2_cp_and_perm(victim, ~(uint32_t)kEg2PermGl), a);
    eg2_cp_check_full(t, "CUnseal local", v, eg2_cp_unseal(local, a));
    // Unsealing an unsealed capability.
    eg2_cp_check_full(t, "CUnseal unsealed", v, eg2_cp_unseal(victim, a));
    ++t->done;
  }
}

// ----------------------------------------- 5. CAndPerm ---------------------------------------

static void phase_andperm(eg2_tally_t *t) {
  eg2_cp_t seal_root = eg2_cp_root_seal();
  eg2_cp_t caps[6] = {
      eg2_cp_root_mem(),
      eg2_cp_root_exe(),
      seal_root,
      data_cap(kEg2SecBase + 0x400, 32),
      eg2_cp_seal(data_cap(kEg2SecBase + 0x400, 32), eg2_cp_set_addr(seal_root, 12)),
      eg2_cp_seal(exe_cap(EG2_ADDR(eg2_stub_arrive), 8, EG2_ADDR(eg2_stub_arrive)),
                  eg2_cp_set_addr(seal_root, 1)),
  };
  uint32_t idx = 0;
  for (uint32_t c = 0; c < 6; ++c) {
    for (uint32_t m = 0; m < 16; ++m) {
      uint32_t mask = m < 12 ? ~(1u << m) : (m == 12 ? 0xfffu : (m == 13 ? 0 : eg2_rand()));
      eg2_cp_t r = eg2_cp_and_perm(caps[c], mask);
      eg2_cp_check_full(t, "CAndPerm", idx, r);
      // Never adds a permission.
      eg2_check(t, "CAndPerm adds", idx, hw_perm(r.hw) & ~hw_perm(caps[c].hw), 0);
      ++idx;
    }
    ++t->done;
  }
}

// ----------------------------- 6. Raw encodings and comparisons ------------------------------

static void phase_high(eg2_tally_t *t) {
  eg2_cp_t base = data_cap(kEg2SecBase + 0x500, 40);
  for (uint32_t i = 0; i < 96; ++i) {
    uint32_t x;
    if (i < 64) {
      // Every permission encoding, with exponents and mantissas that move around.
      x = i << 25 | (eg2_rand() & 0x81ffffffu);
    } else if (i < 80) {
      x = (eg2_rand() & ~(0xfu << 18)) | (i - 64) << 18;  // every exponent encoding
    } else {
      x = i == 80 ? 0 : (i == 81 ? 0xffffffffu : eg2_rand());
    }
    eg2_cp_t r = eg2_cp_set_high(base, x);
    eg2_check(t, "CGetHigh(CSetHigh)", i, hw_high(r.hw), x);
    eg2_cp_check_full(t, "CSetHigh", i, r);
  }
  ++t->done;

  eg2_cp_t a = data_cap(kEg2SecBase + 0x600, 64);
  eg2_cp_t pairs[8][2] = {
      {eg2_cp_root_mem(), a},
      {a, eg2_cp_root_mem()},
      {a, a},
      {a, eg2_cp_bounds(eg2_cp_inc(a, 8), 8, kEg2BoundsExact)},
      {a, eg2_cp_clear_tag(a)},
      {a, eg2_cp_inc(a, 4)},
      {a, eg2_cp_and_perm(a, ~(uint32_t)kEg2PermSd)},
      {eg2_cp_and_perm(a, ~(uint32_t)kEg2PermSd), a},
  };
  for (uint32_t i = 0; i < 8; ++i) {
    void *x = pairs[i][0].hw;
    void *y = pairs[i][1].hw;
    eg2_check(t, "CTestSubset", i, hw_subset(x, y),
              eg2_cap_test_subset(pairs[i][0].m, pairs[i][1].m));
    eg2_check(t, "CSetEqualExact", i, hw_seqx(x, y),
              eg2_cap_equal_exact(pairs[i][0].m, pairs[i][1].m));
    eg2_check(t, "CSub", i, hw_csub(x, y), pairs[i][0].m.addr - pairs[i][1].m.addr);
    eg2_cp_check(t, "CMove", i, eg2_cp_move(pairs[i][1]));
    eg2_cp_check(t, "CClearTag", i, eg2_cp_clear_tag(pairs[i][1]));
  }
  ++t->done;
}

// ------------------------------------------ 7. SCRs ------------------------------------------

static void phase_scr(eg2_tally_t *t) {
  eg2_cp_t seal_root = eg2_cp_root_seal();
  eg2_cp_t d = data_cap(kEg2SecBase + 0x700, 48);
  eg2_cp_t r = {.hw = swap_mtdc(d.hw), .m = d.m};
  eg2_cp_check(t, "CSpecialRW MTDC", 0, r);
  r.hw = swap_mscratchc(d.hw);
  eg2_cp_check(t, "CSpecialRW MScratchC", 0, r);
  // MTCC and MEPCC: validated and legalised on write.
  uint32_t stub = EG2_ADDR(eg2_stub_arrive);
  eg2_cp_t x = exe_cap(stub & ~63u, 128, stub);
  eg2_cp_t vals[7] = {
      x,
      eg2_cp_inc(x, 1),
      eg2_cp_inc(x, 2),
      eg2_cp_inc(x, 3),
      eg2_cp_seal(x, eg2_cp_set_addr(seal_root, kEg2OtypeSentry)),
      d,
      eg2_cp_clear_tag(x),
  };
  for (uint32_t i = 0; i < 7; ++i) {
    for (uint32_t s = 0; s < 2; ++s) {
      uint32_t scr = s == 0 ? 28 : 31;
      eg2_cp_t w = {.hw = eg2_probe_scr_write(scr, vals[i].hw),
                    .m = eg2_cap_scr_legalise(scr, vals[i].m)};
      eg2_cp_check(t, s == 0 ? "CSpecialRW MTCC" : "CSpecialRW MEPCC", i, w);
    }
  }
  ++t->done;
}

// --------------------------------------- 8. AUIPCC, AUICGP -----------------------------------

static char *auipcc_stub(uint32_t k) {
  switch (k) {
    case 0:
      return eg2_stub_auipcc_0;
    case 1:
      return eg2_stub_auipcc_1;
    case 2:
      return eg2_stub_auipcc_m1;
    case 3:
      return eg2_stub_auipcc_max;
    default:
      return eg2_stub_auipcc_min;
  }
}

static char *auicgp_stub(uint32_t k) {
  switch (k) {
    case 0:
      return eg2_stub_auicgp_0;
    case 1:
      return eg2_stub_auicgp_1;
    case 2:
      return eg2_stub_auicgp_m1;
    case 3:
      return eg2_stub_auicgp_max;
    default:
      return eg2_stub_auicgp_min;
  }
}

static uint32_t aui_imm(uint32_t k) {
  static const uint32_t kImm[5] = {0, 1, 0xfffff, 0x7ffff, 0x80000};
  return kImm[k % 5];
}

// AUIPCC under a PCC of bounds class `p`: the root, 2 KiB around the stub, the stub alone.
static void run_auipcc(eg2_tally_t *t, uint32_t k, uint32_t p, uint32_t idx) {
  uint32_t s = EG2_ADDR(auipcc_stub(k));
  eg2_cp_t pcc = p == 0 ? eg2_cp_set_addr(eg2_cp_root_exe(), s)
                        : (p == 1 ? exe_cap((s & ~0x3ffu) - 0x400, 0x800, s) : exe_cap(s, 8, s));
  eg2_probe_call(NULL, NULL, NULL, pcc.hw);
  eg2_cp_t r = {.hw = eg2_trap_rec.cap.p, .m = eg2_cap_auipcc(pcc.m, s, aui_imm(k))};
  eg2_cp_check(t, "AUIPCC", idx, r);
}

static void run_auicgp(eg2_tally_t *t, uint32_t k, eg2_cp_t cgp, uint32_t idx) {
  eg2_probe_call(cgp.hw, NULL, NULL, auicgp_stub(k));
  eg2_cp_t r = {.hw = eg2_trap_rec.cap.p, .m = eg2_cap_auicgp(cgp.m, aui_imm(k))};
  eg2_cp_check(t, "AUICGP", idx, r);
}

static eg2_cp_t cgp_operand(uint32_t c) {
  switch (c) {
    case 0:
      return data_cap(kEg2SecBase + 0x800, 100);
    case 1:
      return eg2_cp_root_mem();
    case 2:
      return eg2_cp_set_addr(data_cap(0x10000000u, 1u << 22), 0x10200000u);
    default:
      return eg2_cp_seal(data_cap(kEg2SecBase + 0x800, 100),
                         eg2_cp_set_addr(eg2_cp_root_seal(), 9));
  }
}

static void phase_aui(eg2_tally_t *t) {
  for (uint32_t k = 0; k < 5; ++k) {
    for (uint32_t p = 0; p < 3; ++p) {
      run_auipcc(t, k, p, k * 3 + p);
    }
    for (uint32_t c = 0; c < 4; ++c) {
      run_auicgp(t, k, cgp_operand(c), k * 4 + c);
    }
  }
  ++t->done;
}

// --------------------------------------- Random operations ----------------------------------

enum {
  kFamBounds,
  kFamAddr,
  kFamGet,
  kFamSeal,
  kFamHigh,
  kFamAui,
  kFamCrrl,
  kFamScr,
  kFamMove,
  kFamCount,
};
static const uint8_t kFamDefault[kFamCount] = {20, 20, 15, 10, 10, 10, 5, 5, 5};

enum {
  kShapeRoot,
  kShapeE0,
  kShapeSmallE,
  kShapeE24,
  kShapeZero,
  kShapeUntagged,
  kShapeSealed,
  kShapeSentry,
  kShapeReduced,
  kShapeCount,
};
// Plain shapes: the root, exponent 0, small exponents.
enum { kShapePlain = 1u << kShapeRoot | 1u << kShapeE0 | 1u << kShapeSmallE };

enum {
  kCurBelowBase,
  kCurAtBase,
  kCurInside,
  kCurLastByte,
  kCurAtTop,
  kCurPastTop,
  kCurAboveTop,
  kCurZero,
  kCurAllOnes,
  kCurCount,
};
enum { kCurPlain = 1u << kCurInside };

enum {
  kPermAll,
  kPermNoLd,
  kPermNoSd,
  kPermNoMc,
  kPermNoSl,
  kPermNoGl,
  kPermNoEx,
  kPermNoSr,
  kPermNoSeal,
  kPermNone,
  kPermCount,
};
enum { kPermPlain = 1u << kPermAll };

typedef struct rand_ctx {
  eg2_family_t fam, shape, cursor, perm;
  eg2_cp_t pool[8];
} rand_ctx_t;

static eg2_cp_t build_shape(uint32_t shape) {
  eg2_cp_t seal_root = eg2_cp_root_seal();
  uint32_t base = kEg2SecBase + (eg2_rand() & 0xfff8u);
  switch (shape) {
    case kShapeRoot:
      return eg2_cp_root_mem();
    case kShapeE0:
      return data_cap(base, eg2_rand_range(1, 511));
    case kShapeSmallE: {
      uint32_t e = eg2_rand_range(1, 14);
      uint32_t len = eg2_rand_range((1u << (e + 8)) + 1, 511u << e);
      return eg2_cp_set_addr(data_cap(0x10000000u, len), 0x10000000u);
    }
    case kShapeE24:
      return data_cap(eg2_rand() & (addr_hi() ? 0xff000000u : 0x7f000000u),
                      0x01000000u << eg2_rand_range(0, 3));
    case kShapeZero:
      return data_cap(base, 0);
    case kShapeUntagged:
      return eg2_cp_clear_tag(data_cap(base, 64));
    case kShapeSealed:
      return eg2_cp_seal(data_cap(base, 64), eg2_cp_set_addr(seal_root, eg2_rand_range(9, 15)));
    case kShapeSentry: {
      uint32_t s = EG2_ADDR(eg2_stub_arrive);
      return eg2_cp_seal(exe_cap(s, 8, s), eg2_cp_set_addr(seal_root, eg2_rand_range(1, 5)));
    }
    default:
      return eg2_cp_and_perm(data_cap(base, 64), eg2_rand() & kEg2PermAll);
  }
}

static uint32_t cursor_addr(eg2_cap_t c, uint32_t cls) {
  uint32_t base = eg2_cap_base(c);
  uint32_t top = (uint32_t)eg2_cap_top(c);
  uint32_t w = 1u << eg2_rand_range(0, 3);
  switch (cls) {
    case kCurBelowBase:
      return base - 1;
    case kCurAtBase:
      return base;
    case kCurInside:
      return base + eg2_rand_range(0, eg2_cap_len32(c));
    case kCurLastByte:
      return top - 1;
    case kCurAtTop:
      return top;
    case kCurPastTop:
      return top + w;
    case kCurAboveTop:
      return top + eg2_rand_range(9, 0x10000);
    case kCurZero:
      return 0;
    default:
      return 0xffffffffu;
  }
}

static uint32_t perm_mask(uint32_t cls) {
  switch (cls) {
    case kPermAll:
      return kEg2PermAll;
    case kPermNoLd:
      return ~(uint32_t)kEg2PermLd;
    case kPermNoSd:
      return ~(uint32_t)kEg2PermSd;
    case kPermNoMc:
      return ~(uint32_t)kEg2PermMc;
    case kPermNoSl:
      return ~(uint32_t)kEg2PermSl;
    case kPermNoGl:
      return ~(uint32_t)kEg2PermGl;
    case kPermNoEx:
      return ~(uint32_t)kEg2PermEx;
    case kPermNoSr:
      return ~(uint32_t)kEg2PermSr;
    case kPermNoSeal:
      return ~(uint32_t)(kEg2PermSe | kEg2PermUs);
    default:
      return 0;
  }
}

static uint32_t edge_len(void) {
  switch (eg2_rand_range(0, 7)) {
    case 0:
      return sweep_len(eg2_rand_range(0, kSweepLens - 1));
    case 1:
      return eg2_rand_range(0, 16);
    case 2:
      return eg2_rand_range(500, 520);
    default:
      return eg2_rand() >> eg2_rand_range(0, 31);
  }
}

// model_gaps: a capability based at or above 2^31 (MPACT faults a CLC of one).
static bool high_based(eg2_cap_t c) { return gaps && eg2_cap_base(c) >= 0x80000000u; }

static void rand_op(eg2_tally_t *real, rand_ctx_t *x, uint32_t n) {
  uint32_t i = eg2_pick(8);
  uint32_t j = eg2_pick(8);
  eg2_cp_t c = x->pool[i];
  eg2_cp_t r = c;
  uint32_t fam = eg2_family_pick(&x->fam);
  // The families of MPACT-CHERIoT's differences (model_gaps; the header lists them).
  bool c2 = !(fam == kFamAui || fam == kFamCrrl || fam == kFamScr);
  eg2_tally_t *t = c2 ? gap(real) : real;
  uint32_t failed = t->failed;
  if (high_based(c.m)) {
    return;  // model_gaps: an operand MPACT cannot load
  }
  switch (fam) {
    case kFamBounds: {
      uint32_t a = cursor_addr(c.m, eg2_family_pick(&x->cursor));
      if (gaps && a >= 0x80000000u) {
        return;  // model_gaps: the result would be based there
      }
      eg2_cp_t at = eg2_cp_set_addr(c, a);
      uint32_t kind = eg2_rand_range(0, 3);
      r = kind == 3 ? eg2_cp_bounds_imm(at, eg2_pick(kEg2BoundsImmCount))
                    : eg2_cp_bounds(at, edge_len(), (eg2_bounds_kind_t)kind);
      eg2_cp_check(t, "rand bounds", n, r);
      if (r.m.tag) {
        eg2_check(t, "rand bounds subset", n, hw_subset(at.hw, r.hw), 1);
      }
      break;
    }
    case kFamAddr: {
      uint32_t a = cursor_addr(c.m, eg2_family_pick(&x->cursor));
      uint32_t how = eg2_rand_range(0, 2);
      r = how == 0 ? eg2_cp_set_addr(c, a)
                   : (how == 1 ? eg2_cp_inc(c, a - c.m.addr) : eg2_cp_inc_imm(c, eg2_pick(7)));
      eg2_cp_check(t, "rand addr", n, r);
      break;
    }
    case kFamGet:
      eg2_cp_check_full(t, "rand get", n, c);
      break;
    case kFamSeal: {
      eg2_cp_t auth = eg2_cp_and_perm(eg2_cp_set_addr(eg2_cp_root_seal(), eg2_rand_range(0, 16)),
                                      perm_mask(eg2_family_pick(&x->perm)));
      r = eg2_cap_is_sealed(c.m) ? eg2_cp_unseal(c, auth) : eg2_cp_seal(c, auth);
      eg2_cp_check_full(t, "rand seal", n, r);
      break;
    }
    case kFamHigh: {
      uint32_t hi = eg2_rand();
      for (uint32_t k = 0; k < 16 && high_based(eg2_cap_set_high(c.m, hi)); ++k) {
        hi = eg2_rand();
      }
      if (high_based(eg2_cap_set_high(c.m, hi))) {
        return;
      }
      r = eg2_cp_set_high(c, hi);
      eg2_check(t, "rand CGetHigh(CSetHigh)", n, hw_high(r.hw), hi);
      eg2_cp_check(t, "rand CSetHigh", n, r);
      eg2_cp_t d = x->pool[j];
      eg2_check(t, "rand CTestSubset", n, hw_subset(c.hw, d.hw), eg2_cap_test_subset(c.m, d.m));
      eg2_check(t, "rand CSetEqualExact", n, hw_seqx(c.hw, d.hw),
                eg2_cap_equal_exact(c.m, d.m));
      break;
    }
    case kFamAui:
      if (eg2_rand_range(0, 1) == 0) {
        run_auipcc(t, eg2_pick(5), eg2_pick(3), n);
      } else {
        run_auicgp(t, eg2_pick(5), c, n);
      }
      break;
    case kFamCrrl: {
      uint32_t len = edge_len();
      eg2_check(t, "rand CRRL", n, hw_crrl(len), eg2_cap_crrl(len));
      eg2_check(t, "rand CRAM", n, hw_cram(len), eg2_cap_cram(len));
      break;
    }
    case kFamScr: {
      eg2_cp_t rd = {.hw = eg2_rand_range(0, 1) ? swap_mtdc(c.hw) : swap_mscratchc(c.hw),
                     .m = c.m};
      eg2_cp_check(t, "rand CSpecialRW", n, rd);
      break;
    }
    default: {
      uint32_t how = eg2_rand_range(0, 2);
      r = how == 0 ? eg2_cp_move(c)
                   : (how == 1 ? eg2_cp_clear_tag(c)
                               : eg2_cp_and_perm(c, perm_mask(eg2_family_pick(&x->perm))));
      eg2_cp_check(t, "rand move", n, r);
      eg2_check(t, "rand CAndPerm adds", n, hw_perm(r.hw) & ~hw_perm(c.hw), 0);
      break;
    }
  }
  // Results feed later operations; a slot is rebuilt from the roots now and then, and after a
  // mismatch (rebuilt, never copied from the hardware), so one disagreement is reported once.
  bool rebuild = t->failed != failed || eg2_rand_range(0, 7) == 0;
  x->pool[j] = (rebuild || high_based(r.m)) ? build_shape(eg2_family_pick(&x->shape)) : r;
  if (t->failed != failed) {
    x->pool[i] = build_shape(eg2_family_pick(&x->shape));
  }
}

static void phase_random(eg2_tally_t *t, uint32_t n_ops) {
  rand_ctx_t x;
  uint32_t boost = EG2_PARAM(boost);
  x.fam.n = kFamCount;
  x.fam.plain_mask = 0;
  x.fam.boosted = kFamCount;
  for (uint32_t f = 0; f < kFamCount; ++f) {
    x.fam.w[f] = eg2_rand_range(kFamDefault[f] / 2u, kFamDefault[f] * 3u / 2u);
  }
  eg2_family_draw(&x.shape, kShapeCount, kShapePlain, boost);
  eg2_family_draw(&x.cursor, kCurCount, kCurPlain, boost);
  eg2_family_draw(&x.perm, kPermCount, kPermPlain, boost);
  eg2_family_print("family", &x.fam);
  eg2_family_print("shape", &x.shape);
  eg2_family_print("cursor", &x.cursor);
  eg2_family_print("perm", &x.perm);
  for (uint32_t i = 0; i < 8; ++i) {
    x.pool[i] = build_shape(i < kShapeCount ? i : eg2_family_pick(&x.shape));
  }
  t->planned += n_ops;
  for (uint32_t n = 0; n < n_ops; ++n) {
    rand_op(t, &x, n);
    ++t->done;
  }
}

// --------------------------------------------------------------------------------------------

static bool run_phase(const char *name, void (*fn)(eg2_tally_t *), uint32_t planned, bool c2,
                      eg2_tally_t *total) {
  eg2_tally_t t = {.planned = planned};
  fn(&t);
  eg2_tally_print(name, &t);
  if (gaps && c2) {
    // model_gaps: this phase's differences are MPACT-CHERIoT's (the header lists them).
    gap_tally.checks += t.checks;
    gap_tally.failed += t.failed;
    t.checks = 0;
    t.failed = 0;
  }
  total->checks += t.checks;
  total->failed += t.failed;
  total->planned += t.planned;
  total->done += t.done;
  return eg2_tally_ok(&t);
}

bool eg2_test_main(void) {
  eg2_tally_t total = {0};
  gaps = EG2_PARAM(model_gaps) != 0;
  eg2_cp_save_sealing_root();
  bool ok = run_phase("roots", phase_roots, 4, false, &total);
  if (!eg2_sec_sram_init()) {
    eg2_puts("FAIL: no main SRAM arena\r\n");
    return false;
  }
  eg2_probe_begin();
  ok = run_phase("exponents", phase_exponents, kSweepLens + 1, false, &total) && ok;
  ok = run_phase("representability", phase_repr, kReprShapes, true, &total) && ok;
  ok = run_phase("seal", phase_seal, 16 + 2, true, &total) && ok;
  ok = run_phase("andperm", phase_andperm, 6, true, &total) && ok;
  ok = run_phase("high", phase_high, 2, true, &total) && ok;
  ok = run_phase("scr", phase_scr, 1, false, &total) && ok;
  ok = run_phase("aui", phase_aui, 1, false, &total) && ok;
  uint32_t n_ops = eg2_draw_count(EG2_PARAM(n_ops), 256, 2048, 4096);
  eg2_tally_t rt = {0};
  phase_random(&rt, n_ops);
  eg2_tally_print("random", &rt);
  ok = eg2_tally_ok(&rt) && ok;
  eg2_probe_end();
  total.checks += rt.checks + kept.checks;
  total.failed += rt.failed + kept.failed;
  total.planned += rt.planned;
  total.done += rt.done;
  ok = kept.failed == 0 && ok;
  if (gaps) {
    eg2_tally_print("model gaps (not counted)", &gap_tally);
  }
  eg2_tally_print("total", &total);
  return ok;
}
