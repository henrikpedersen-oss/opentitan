// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_periph_cap_random (EG2 spec "Memory, tags and devices"; REQ_PER_01, REQ_PER_02,
 * REQ_PBF_13).
 *
 * Random accesses to every device window through a capability with the window's exact bounds,
 * each one either legal or with one fault (`w_fault_<f>`): legal 40, above the window 15, below it
 * 15, missing permission (no LD) 15, misaligned 10, untagged 5. The window is drawn uniformly over
 * the register blocks (eg2_regmem's tables, generated from the IPs' hjson), the register among the
 * window's side-effect-free ones whose reset value hardware does not change, its first and last at
 * 25 % each.
 *
 * Expected (CHERIoT ISA, through the test's capability model from CHERIoT-Sail; reggen README
 * "Error responses": a read narrower than a word returns the register's bytes without error):
 *  - legal: the register's reset value in the bits the table predicts;
 *  - a capability fault: mcause 28 and mtval with the cause and the authority register (a0) for
 *    the cause the model gives (bounds, permission, tag);
 *  - misaligned: a halfword load one byte into the register, in bounds, returns its bytes 1-2.
 * Not checked here (bench): that a faulting access makes no bus transaction (the path check,
 * decision 2026-10-24).
 *
 * Built with EG2_PERIPH_CAP_RANDOM_INJECT_FAIL, the first access above a window is expected not to
 * fault: the run must fail with `MISMATCH mcause`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem_tables.h"

EG2_TEST_CONFIG();

#define PERIPH_CAP_RANDOM_PARAMS(X)  \
  X(n_accesses, 0, 0, 2048)          \
  X(w_fault_legal, 40, 1, 100)       \
  X(w_fault_above, 15, 1, 100)       \
  X(w_fault_below, 15, 1, 100)       \
  X(w_fault_perm, 15, 1, 100)        \
  X(w_fault_misaligned, 10, 1, 100)  \
  X(w_fault_untagged, 5, 1, 100)     \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(PERIPH_CAP_RANDOM_PARAMS);

enum {
  kFaultLegal = 0,
  kFaultAbove,
  kFaultBelow,
  kFaultPerm,
  kFaultMisaligned,
  kFaultUntagged,
  kFaultCount,
};

typedef struct window {
  uint32_t base;
  uint32_t size;
} window_t;

#define EG2_WINDOW_ROW_(name, base, size) {(base), (size)},
static const window_t kWindows[] = {EG2_WINDOW_TABLE(EG2_WINDOW_ROW_)};
#undef EG2_WINDOW_ROW_

static eg2_tally_t tally;
#ifdef EG2_PERIPH_CAP_RANDOM_INJECT_FAIL
static bool injected;
#endif

static uint32_t window_size(uint32_t base) {
  for (uint32_t i = 0; i < ARRAYSIZE(kWindows); ++i) {
    if (kWindows[i].base == base) {
      return kWindows[i].size;
    }
  }
  return 0;
}

static bool readable(const eg2_rm_reg_t *r) {
  uint32_t skip = kEg2RmNoRead | kEg2RmNoReset | (EG2_PARAM(model_gaps) ? kEg2RmGapR : 0);
  return !(r->flags & skip);
}

/** A readable register of block `b` (its first, its last or any); 0xffff if it has none. */
static uint32_t pick_reg(const eg2_rm_block_t *b) {
  uint32_t first = 0xffff, last = 0xffff, count = 0;
  for (uint32_t i = 0; i < b->count; ++i) {
    if (readable(&kEg2RmRegs[b->first + i])) {
      first = first == 0xffff ? i : first;
      last = i;
      ++count;
    }
  }
  if (count == 0) {
    return 0xffff;
  }
  uint32_t d = eg2_rand_range(0, 99);
  if (d < 25) {
    return b->first + first;
  }
  if (d < 50) {
    return b->first + last;
  }
  uint32_t k = eg2_pick(count);
  for (uint32_t i = 0; i < b->count; ++i) {
    if (readable(&kEg2RmRegs[b->first + i]) && k-- == 0) {
      return b->first + i;
    }
  }
  return b->first + last;
}

static void one_access(uint32_t idx, uint32_t fault) {
  const eg2_rm_block_t *b = &kEg2RmBlocks[eg2_pick(EG2_RM_BLOCK_COUNT)];
  uint32_t ri = pick_reg(b);
  uint32_t size = window_size(b->base);
  if (ri == 0xffff || size == 0) {
    return;
  }
  ++tally.planned;
  const eg2_rm_reg_t *r = &kEg2RmRegs[ri];
  const eg2_rm_masks_t *m = &kEg2RmMasks[r->masks];
  uint32_t reg = b->base + r->offset;

  // The authority: the window's exact bounds, then the fault's change.
  eg2_cp_t c = eg2_cp_root_mem();
  c = eg2_cp_set_addr(c, b->base);
  c = eg2_cp_bounds(c, size, kEg2BoundsExact);
  uint32_t addr = reg, width = 4;
  bool half = false;
  switch (fault) {
    case kFaultAbove:
      addr = b->base + size + 4 * eg2_pick(4);
      break;
    case kFaultBelow:
      addr = b->base - 4 - 4 * eg2_pick(4);
      break;
    case kFaultPerm:
      c = eg2_cp_and_perm(c, ~(uint32_t)kEg2PermLd);
      break;
    case kFaultMisaligned:
      addr = reg + 1;
      width = 2;
      half = true;
      break;
    case kFaultUntagged:
      c = eg2_cp_clear_tag(c);
      break;
    default:
      break;
  }
  c = eg2_cp_set_addr(c, addr);
  int cause = eg2_cap_check_access(c.m, addr, width, kEg2AccessLoad, false);
  bool trapped = eg2_probe_call(c.hw, NULL, NULL, half ? eg2_stub_lhu : eg2_stub_lw);
  uint32_t exp_mcause = cause > 0 ? (uint32_t)kEg2McauseCheri : 0;
#ifdef EG2_PERIPH_CAP_RANDOM_INJECT_FAIL
  if (fault == kFaultAbove && !injected) {
    injected = true;
    exp_mcause = 0;
  }
#endif
  uint32_t failed = tally.failed;
  if (eg2_check(&tally, "mcause", idx, trapped ? eg2_trap_rec.mcause : 0, exp_mcause)) {
    if (cause > 0) {
      eg2_check(&tally, "mtval", idx, eg2_trap_rec.mtval, eg2_cap_mtval(10, (uint32_t)cause));
    } else {
      uint32_t got = eg2_trap_rec.value;
      uint32_t exp = r->resval & m->known;
      if (half) {
        // Bytes 1-2 of the register, as far as the table predicts them.
        uint32_t known = (m->known >> 8) & 0xffff;
        eg2_check(&tally, "half value", idx, got & known, (exp >> 8) & known);
      } else {
        eg2_check(&tally, "value", idx, got & m->known, exp);
      }
    }
  }
  if (tally.failed != failed && failed < kEg2MaxReports) {
    eg2_put_kv("  window", b->base);
    eg2_put_kv(" register", reg);
    eg2_put_kv(" fault class", fault);
    eg2_puts("\r\n");
  }
  ++tally.done;
}

bool eg2_test_main(void) {
  uint32_t n = EG2_PARAM(n_accesses) != 0 ? EG2_PARAM(n_accesses) : eg2_rand_range(128, 2048);
  eg2_family_t f = {.n = kFaultCount, .plain_mask = 0, .boosted = kFaultCount};
  f.w[kFaultLegal] = EG2_PARAM(w_fault_legal);
  f.w[kFaultAbove] = EG2_PARAM(w_fault_above);
  f.w[kFaultBelow] = EG2_PARAM(w_fault_below);
  f.w[kFaultPerm] = EG2_PARAM(w_fault_perm);
  f.w[kFaultMisaligned] = EG2_PARAM(w_fault_misaligned);
  f.w[kFaultUntagged] = EG2_PARAM(w_fault_untagged);
  eg2_family_print("fault", &f);
  eg2_put_kv("EG2 periph_cap_random n_accesses", n);
  eg2_puts("\r\n");
  eg2_probe_begin();
  // Every fault class once first, then the draws.
  for (uint32_t i = 0; i < kFaultCount + n; ++i) {
    one_access(i, i < kFaultCount ? i : eg2_family_pick(&f));
  }
  eg2_probe_end();
  eg2_tally_print("periph_cap_random", &tally);
  return eg2_tally_ok(&tally);
}
