// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_regs (EG2 verification spec, "Registers and memories"; REQ_PER_01, REQ_PER_02,
 * REQ_TL_07, REQ_SEC_02): every register of every register window of the top, accessed by the
 * core in random order, in either execution mode (one source, two builds).
 *
 * The expected behaviour of each register is the generated table eg2_regmem_tables.h (from the
 * IPs' hjson, scripts/gen_eg2_regmem_tables.py): reset value, access type per bit, the bytes that
 * hold fields, shadowed or not, its REGWEN, and what may not be done to it with the reason.
 *
 * Per window (`n_windows` of them, in `order`), optionally first a reset read of every register
 * (`reset_read_first`), then per register (at most `max_regs`) one operation drawn by the
 * `w_op_*` weights:
 *   - reset: a read (word, half or byte, `w_width_*`) compared with the reset value;
 *   - write: `n_patterns` patterns (`w_data_*`: all zeros, all ones on the bus, one walking bit,
 *     every field at its maximum, random) written and read back: rw bits as written, rw1c bits
 *     cleared by a 1, rw0c bits by a 0, read-as-zero bits zero, the rest unchanged; a half or
 *     byte write that leaves a field byte out must answer a bus error and change nothing;
 *     multi-bit booleans are only ever written True or False;
 *   - walk: walking one and walking zero over the rw bits;
 *   - alias: a pattern written, a sample of `alias_reads` other registers of the window read
 *     before and after it: unchanged;
 *   - shadow (shadowed registers): one write leaves the register as it was, a second equal write
 *     updates it;
 *   - regwen (registers behind a REGWEN): kept for the lock phase.
 * Every write is undone, the register written back to what it held before. At the end, the lock
 * phase locks the REGWENs drawn (a 0 written: it reads 0 and a 1 does not reopen it) and checks
 * that a write to the locked register changes nothing.
 *
 * `model_gaps` (default 0) skips what the EG2 SystemC model does not model (the table's gaps,
 * shadowed staging outside the alert handler, the bus error of a narrow write): set by eg2sim
 * runs only, and counted.
 *
 * Built with `EG2_REGMEM_INJECT_FAIL`, the first value compared is expected with one bit flipped:
 * the run must fail with `REG FAIL`.
 */

#include "sw/device/tests/eg2_regmem/eg2_regmem.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem_tables.h"

EG2_TEST_CONFIG();

#define RAND_REGS_PARAMS(X)                                 \
  X(n_windows, EG2_RM_BLOCK_COUNT, 1, EG2_RM_BLOCK_COUNT) \
  X(max_regs, 1024, 1, 1024)                              \
  X(n_patterns, 2, 1, 8)                                  \
  X(w_op_reset, 15, 0, 100)                               \
  X(w_op_write, 35, 0, 100)                               \
  X(w_op_walk, 15, 0, 100)                                \
  X(w_op_alias, 15, 0, 100)                               \
  X(w_op_regwen, 10, 0, 100)                              \
  X(w_op_shadow, 10, 0, 100)                              \
  X(w_data_zeros, 20, 0, 100)                             \
  X(w_data_ones, 20, 0, 100)                              \
  X(w_data_walk, 20, 0, 100)                              \
  X(w_data_max, 20, 0, 100)                               \
  X(w_data_rand, 20, 0, 100)                              \
  X(w_width_word, 70, 0, 100)                             \
  X(w_width_half, 15, 0, 100)                             \
  X(w_width_byte, 15, 0, 100)                             \
  X(order, 2, 0, 2)                                       \
  X(reset_read_first, 1, 0, 1)                            \
  X(alias_reads, 8, 0, 64)                                \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(RAND_REGS_PARAMS)

enum {
  kOpReset,
  kOpWrite,
  kOpWalk,
  kOpAlias,
  kOpRegwen,
  kOpShadow,
  kOpCount,
};
enum { kDataZeros, kDataOnes, kDataWalk, kDataMax, kDataRand, kDataCount };
enum { kWidthWord, kWidthHalf, kWidthByte, kWidthCount };
enum {
  kMaxLocks = 64,
  kMaxFailLines = 200,      // failure lines printed in a run; the rest are counted
  kMaxBlockFailLines = 8,  // and in one window
  kNoReg = 0xffff,
};

// The run's state, on the stack: no mutable globals (the CHERIoT build's globals stay small).
typedef struct ctx {
  bool gaps;            // model_gaps
  bool injected;        // EG2_REGMEM_INJECT_FAIL: the one flipped expectation is used
  uint32_t checks;      // compared reads
  uint32_t fails;
  uint32_t block_lines;  // failure lines printed for the current window
  uint32_t bus_errors;  // narrow writes that answered the expected error
  uint32_t gap_skips;   // checks not made because the model does not model them
  uint32_t ops[kOpCount];
  uint32_t excl_regs;   // registers left out of every operation (never read)
  uint32_t nlock;
  uint16_t locks[kMaxLocks];
  uint32_t dirty[(EG2_RM_REG_COUNT + 31) / 32];  // no longer holds its reset value
  uint32_t locked[(EG2_RM_REG_COUNT + 31) / 32]; // REGWENs this run locked
} ctx_t;

static const char *name_of(uint32_t off) { return &kEg2RmNames[off]; }

static bool bit_get(const uint32_t *set, uint32_t i) { return (set[i / 32] >> (i % 32)) & 1; }
static void bit_set(uint32_t *set, uint32_t i) { set[i / 32] |= 1u << (i % 32); }

static const eg2_rm_masks_t *masks_of(const eg2_rm_reg_t *r) { return &kEg2RmMasks[r->masks]; }

static void put_reg(const eg2_rm_block_t *b, const eg2_rm_reg_t *r) {
  eg2_puts(name_of(b->name));
  eg2_puts(".");
  eg2_puts(name_of(r->name));
}

// Compares `got` with `exp` in `mask`; reports a mismatch. The injected failure flips the first
// expectation that is compared at all.
static bool check(ctx_t *c, const eg2_rm_block_t *b, const eg2_rm_reg_t *r, const char *what,
                  uint32_t got, uint32_t exp, uint32_t mask) {
  if (mask == 0) {
    return true;
  }
#ifdef EG2_REGMEM_INJECT_FAIL
  if (!c->injected) {
    c->injected = true;
    exp ^= mask & (~mask + 1);
  }
#endif
  ++c->checks;
  if (((got ^ exp) & mask) == 0) {
    return true;
  }
  if (c->fails++ < kMaxFailLines && c->block_lines++ < kMaxBlockFailLines) {
    eg2_puts("REG FAIL ");
    put_reg(b, r);
    eg2_puts(" ");
    eg2_puts(what);
    eg2_rm_hex(": got ", got);
    eg2_rm_hex(" expected ", exp);
    eg2_rm_hex(" mask ", mask);
    eg2_puts("\r\n");
  }
  return false;
}

static void fail_fault(ctx_t *c, const eg2_rm_block_t *b, const eg2_rm_reg_t *r,
                       const char *what, uint32_t mcause, uint32_t mtval) {
  if (c->fails++ < kMaxFailLines && c->block_lines++ < kMaxBlockFailLines) {
    eg2_puts("REG FAIL ");
    put_reg(b, r);
    eg2_puts(" ");
    eg2_puts(what);
    eg2_rm_hex(": mcause ", mcause);
    eg2_rm_hex(" mtval ", mtval);
    eg2_puts("\r\n");
  }
}

// A word read; a fault is a failure (and reads as 0).
static uint32_t rd(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                   const eg2_rm_reg_t *r) {
  eg2_rm_out_t out;
  uint32_t cause = eg2_rm_probe(blk + r->offset, kEg2RmLw, 0, &out);
  if (cause != 0) {
    fail_fault(c, b, r, "read faulted", cause, out.mtval);
  }
  return out.value;
}

// A word write; a fault is a failure. Shadowed registers get their two writes.
static void wr(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b, const eg2_rm_reg_t *r,
               uint32_t value) {
  eg2_rm_out_t out;
  int n = (r->flags & kEg2RmShadowed) ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    uint32_t cause = eg2_rm_probe(blk + r->offset, kEg2RmSw, value, &out);
    if (cause != 0) {
      fail_fault(c, b, r, "write faulted", cause, out.mtval);
    }
  }
}

// The value a write leaves unchanged: rw1c, r0w1c and rw1s bits 0, the rest as they are (rw0c
// bits read 0 stay 0, read 1 stay 1).
static uint32_t neutral(const eg2_rm_masks_t *m, uint32_t cur) { return cur & ~m->neutral0; }

// What the register holds after `data` is written over `old`, in the predicted bits.
static uint32_t after_write(const eg2_rm_masks_t *m, uint32_t old, uint32_t data) {
  uint32_t v = (old & ~m->rw) | (data & m->rw);
  v &= ~(data & m->w1c);
  v |= data & m->w1s;
  v &= ~(~data & m->rw0c);
  return v & ~m->rdzero;
}

// Multi-bit booleans: True or False only.
static uint32_t fix_mubi(const eg2_rm_masks_t *m, uint32_t v) {
  if (m->mubi == 0) {
    return v;
  }
  uint32_t enc = (eg2_rand() & 1) ? m->mtrue : (~m->mtrue & m->mubi);
  return (v & ~m->mubi) | enc;
}

// A data pattern of class `cls` over the register's current value.
static uint32_t pattern(const eg2_rm_masks_t *m, uint32_t cur, uint32_t cls) {
  uint32_t base = neutral(m, cur);
  uint32_t bits = m->rw & ~m->mubi;
  uint32_t v;
  switch (cls) {
    case kDataZeros:
      v = base & ~bits;
      break;
    case kDataOnes:
      // Every bit on the bus, field or not: read-only and unused bits must ignore it. rw1s bits
      // stay 0 (a set cannot be undone); rw1c bits are cleared.
      v = ~(m->neutral0 & ~m->w1c);
      break;
    case kDataWalk: {
      v = base & ~bits;
      if (bits != 0) {
        uint32_t n = eg2_rand_range(0, (uint32_t)__builtin_popcount(bits) - 1);
        for (uint32_t i = 0; i < 32; ++i) {
          if ((bits >> i) & 1) {
            if (n-- == 0) {
              v |= 1u << i;
              break;
            }
          }
        }
      }
      break;
    }
    case kDataMax:
      v = base | bits;
      break;
    default:
      v = (base & ~bits) | (eg2_rand() & bits);
      break;
  }
  return fix_mubi(m, v);
}

static bool gate_open(volatile uint8_t *blk, const eg2_rm_reg_t *r) {
  if (r->regwen == kNoReg) {
    return true;
  }
  uint32_t v = *(volatile uint32_t *)(blk + kEg2RmRegs[r->regwen].offset);
  return (r->flags & kEg2RmWenMubi) ? (v & 0xf) == 0x6 : (v & 1) != 0;
}

// The byte lanes of a write of `width` at byte `lane`.
static uint32_t lanes(uint32_t width, uint32_t lane) {
  return width == kWidthWord ? 0xf : width == kWidthHalf ? (0x3u << lane) : (0x1u << lane);
}
static uint32_t lane_mask(uint32_t be) {
  uint32_t m = 0;
  for (int i = 0; i < 4; ++i) {
    if ((be >> i) & 1) {
      m |= 0xffu << (8 * i);
    }
  }
  return m;
}

// One write of `data` with a drawn width, then the read back and its check against the
// register's rules. Returns the value read back.
static uint32_t write_check(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                            const eg2_rm_reg_t *r, uint32_t old, uint32_t data,
                            const char *what) {
  static const uint32_t kOps[kWidthCount] = {kEg2RmSw, kEg2RmSh, kEg2RmSb};
  const eg2_rm_masks_t *m = masks_of(r);
  uint32_t ww[kWidthCount] = {EG2_PARAM(w_width_word), EG2_PARAM(w_width_half),
                              EG2_PARAM(w_width_byte)};
  uint32_t width = eg2_rm_pick(ww, kWidthCount);
  if (width >= kWidthCount || (r->flags & kEg2RmShadowed)) {
    width = kWidthWord;  // shadowed registers: two equal word writes
  }
  uint32_t lane = width == kWidthWord ? 0 : width == kWidthHalf ? 2 * eg2_rand_range(0, 1)
                                                               : eg2_rand_range(0, 3);
  uint32_t be = lanes(width, lane);
  bool err = (r->permit & ~be & 0xf) != 0;  // a field byte left out: a bus error (reggen)
  if (err && c->gaps) {
    // The model applies narrow register writes instead of answering the error.
    ++c->gap_skips;
    width = kWidthWord;
    lane = 0;
    be = 0xf;
    err = false;
  }
  bool open = gate_open(blk, r);
  eg2_rm_out_t out;
  int n = (r->flags & kEg2RmShadowed) ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    uint32_t cause = eg2_rm_probe(blk + r->offset + lane, kOps[width],
                                  width == kWidthWord ? data : data >> (8 * lane), &out);
    if (err) {
      if (cause != kEg2RmCauseStoreFault) {
        fail_fault(c, b, r, width == kWidthHalf ? "half write: no bus error"
                                                : "byte write: no bus error",
                   cause, out.mtval);
      } else {
        ++c->bus_errors;
      }
    } else if (cause != 0) {
      fail_fault(c, b, r, "write faulted", cause, out.mtval);
    }
  }
  uint32_t exp = (err || !open) ? old : after_write(m, old, data);
  uint32_t got = rd(c, blk, b, r);
  check(c, b, r, err ? "narrow write changed it" : !open ? "locked, but written" : what, got,
        exp, m->known);
  return got;
}

// Puts back what the register held (the rw bits; cleared rw1c bits stay cleared).
static void restore(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                    const eg2_rm_reg_t *r, uint32_t orig, uint32_t idx) {
  const eg2_rm_masks_t *m = masks_of(r);
  wr(c, blk, b, r, neutral(m, orig));
  uint32_t got = rd(c, blk, b, r);
  uint32_t exp = after_write(m, orig, neutral(m, orig));
  check(c, b, r, "restore", got, exp, m->known);
  if (((exp ^ orig) & m->known) != 0) {
    bit_set(c->dirty, idx);
  }
}

static void op_reset(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                     const eg2_rm_reg_t *r, bool word_only) {
  static const uint32_t kOps[kWidthCount] = {kEg2RmLw, kEg2RmLhu, kEg2RmLbu};
  uint32_t ww[kWidthCount] = {EG2_PARAM(w_width_word), EG2_PARAM(w_width_half),
                              EG2_PARAM(w_width_byte)};
  uint32_t width = word_only ? kWidthWord : eg2_rm_pick(ww, kWidthCount);
  if (width >= kWidthCount) {
    width = kWidthWord;
  }
  uint32_t lane = width == kWidthWord ? 0 : width == kWidthHalf ? 2 * eg2_rand_range(0, 1)
                                                               : eg2_rand_range(0, 3);
  eg2_rm_out_t out;
  uint32_t cause = eg2_rm_probe(blk + r->offset + lane, kOps[width], 0, &out);
  if (cause != 0) {
    fail_fault(c, b, r, "reset read faulted", cause, out.mtval);
    return;
  }
  uint32_t lm = lane_mask(lanes(width, lane));
  check(c, b, r, width == kWidthWord ? "reset value" : "reset value (narrow read)",
        out.value << (8 * lane), r->resval, masks_of(r)->known & lm);
}

static void op_write(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                     const eg2_rm_reg_t *r, uint32_t idx) {
  const eg2_rm_masks_t *m = masks_of(r);
  uint32_t wd[kDataCount] = {EG2_PARAM(w_data_zeros), EG2_PARAM(w_data_ones),
                             EG2_PARAM(w_data_walk), EG2_PARAM(w_data_max),
                             EG2_PARAM(w_data_rand)};
  uint32_t orig = rd(c, blk, b, r);
  uint32_t cur = orig;
  for (uint32_t p = 0; p < EG2_PARAM(n_patterns); ++p) {
    uint32_t cls = eg2_rm_pick(wd, kDataCount);
    cur = write_check(c, blk, b, r, cur, pattern(m, cur, cls >= kDataCount ? kDataRand : cls),
                      "written value");
  }
  restore(c, blk, b, r, orig, idx);
}

static void op_walk(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                    const eg2_rm_reg_t *r, uint32_t idx) {
  const eg2_rm_masks_t *m = masks_of(r);
  uint32_t orig = rd(c, blk, b, r);
  uint32_t bits = m->rw & ~m->mubi;
  uint32_t base = fix_mubi(m, neutral(m, orig));
  uint32_t cur = orig;
  for (uint32_t i = 0; i < 32; ++i) {
    if ((bits >> i) & 1) {
      cur = write_check(c, blk, b, r, cur, (base & ~bits) | (1u << i), "walking one");
      cur = write_check(c, blk, b, r, cur, (base | bits) & ~(1u << i), "walking zero");
    }
  }
  restore(c, blk, b, r, orig, idx);
}

static void op_alias(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                     const eg2_rm_reg_t *r, uint32_t idx) {
  enum { kMaxSample = 64 };
  uint16_t sample[kMaxSample];
  uint32_t before[kMaxSample];
  uint32_t n = 0;
  uint32_t want = EG2_PARAM(alias_reads);
  for (uint32_t tries = 0; n < want && n < kMaxSample && tries < 4 * want; ++tries) {
    uint32_t k = b->first + eg2_rand_range(0, b->count - 1u);
    const eg2_rm_reg_t *o = &kEg2RmRegs[k];
    if (k == idx || (o->flags & kEg2RmNoRead) || masks_of(o)->known == 0) {
      continue;
    }
    sample[n] = (uint16_t)k;
    before[n] = rd(c, blk, b, o);
    ++n;
  }
  const eg2_rm_masks_t *m = masks_of(r);
  uint32_t orig = rd(c, blk, b, r);
  write_check(c, blk, b, r, orig, pattern(m, orig, kDataRand), "written value");
  for (uint32_t i = 0; i < n; ++i) {
    const eg2_rm_reg_t *o = &kEg2RmRegs[sample[i]];
    uint32_t got = rd(c, blk, b, o);
    check(c, b, o, "changed by a write to another register (aliasing)", got, before[i],
          masks_of(o)->known);
  }
  restore(c, blk, b, r, orig, idx);
}

static void op_shadow(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                      const eg2_rm_reg_t *r, uint32_t idx) {
  const eg2_rm_masks_t *m = masks_of(r);
  uint32_t orig = rd(c, blk, b, r);
  uint32_t v = pattern(m, orig, kDataRand);
  if (((v ^ orig) & m->rw) == 0) {
    v ^= m->rw & ~m->mubi & (~(m->rw & ~m->mubi) + 1);  // differ in one bit at least
  }
  bool open = gate_open(blk, r);
  // Outside the alert handler the model applies the first write at once.
  bool gap = c->gaps && b->base != TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR;
  eg2_rm_out_t out;
  uint32_t cause = eg2_rm_probe(blk + r->offset, kEg2RmSw, v, &out);
  if (cause != 0) {
    fail_fault(c, b, r, "first shadow write faulted", cause, out.mtval);
  }
  // A read puts the shadow register back into its first phase (reggen: Shadow Registers).
  uint32_t got = rd(c, blk, b, r);
  if (gap) {
    ++c->gap_skips;
  } else {
    check(c, b, r, "updated by one shadow write", got, orig, m->known);
  }
  cause = eg2_rm_probe(blk + r->offset, kEg2RmSw, v, &out);
  cause |= eg2_rm_probe(blk + r->offset, kEg2RmSw, v, &out);
  if (cause != 0) {
    fail_fault(c, b, r, "shadow writes faulted", cause, out.mtval);
  }
  got = rd(c, blk, b, r);
  check(c, b, r, open ? "two equal shadow writes" : "locked, but written", got,
        open ? after_write(m, orig, v) : orig, m->known);
  restore(c, blk, b, r, orig, idx);
}

static bool readable(const eg2_rm_reg_t *r) { return !(r->flags & kEg2RmNoRead); }
static bool resettable(ctx_t *c, const eg2_rm_reg_t *r, uint32_t idx) {
  return !(r->flags & (kEg2RmNoRead | kEg2RmNoReset)) && !bit_get(c->dirty, idx) &&
         !(c->gaps && (r->flags & kEg2RmGapR));
}
static bool writable(ctx_t *c, const eg2_rm_reg_t *r) {
  return !(r->flags & (kEg2RmNoRead | kEg2RmNoWrite)) &&
         !(c->gaps && (r->flags & kEg2RmGapW));
}

static void one_register(ctx_t *c, volatile uint8_t *blk, const eg2_rm_block_t *b,
                         uint32_t idx) {
  const eg2_rm_reg_t *r = &kEg2RmRegs[idx];
  if (!readable(r)) {
    ++c->excl_regs;
    return;
  }
  uint32_t wo[kOpCount] = {EG2_PARAM(w_op_reset), EG2_PARAM(w_op_write), EG2_PARAM(w_op_walk),
                           EG2_PARAM(w_op_alias), EG2_PARAM(w_op_regwen),
                           EG2_PARAM(w_op_shadow)};
  uint32_t op = eg2_rm_pick(wo, kOpCount);
  if (op >= kOpCount) {
    op = kOpReset;
  }
  if (op != kOpReset && !writable(c, r)) {
    op = kOpReset;
  }
  if (op == kOpShadow && !(r->flags & kEg2RmShadowed)) {
    op = kOpWrite;
  }
  if (op == kOpRegwen) {
    if (r->regwen != kNoReg && (kEg2RmRegs[r->regwen].flags & kEg2RmLockable) &&
        c->nlock < kMaxLocks) {
      c->locks[c->nlock++] = (uint16_t)idx;
    }
    op = kOpWrite;
  }
  ++c->ops[op];
  switch (op) {
    case kOpReset:
      if (resettable(c, r, idx)) {
        op_reset(c, blk, b, r, false);
      } else {
        (void)rd(c, blk, b, r);  // still read: the access must not fault
      }
      break;
    case kOpWrite:
      op_write(c, blk, b, r, idx);
      break;
    case kOpWalk:
      op_walk(c, blk, b, r, idx);
      break;
    case kOpAlias:
      op_alias(c, blk, b, r, idx);
      break;
    default:
      op_shadow(c, blk, b, r, idx);
      break;
  }
}

static void shuffle(uint16_t *a, uint32_t n) {
  for (uint32_t i = n; i > 1; --i) {
    uint32_t j = eg2_rand_range(0, i - 1);
    uint16_t t = a[i - 1];
    a[i - 1] = a[j];
    a[j] = t;
  }
}

// The address that orders entry i: a block's base, or a register's offset.
static uint32_t addr_of(uint16_t i, bool is_reg) {
  return is_reg ? kEg2RmRegs[i].offset : kEg2RmBlocks[i].base;
}

// Address order (`order` 0) or reverse address order (1). Insertion sort: the arrays are short.
static void ordered(uint16_t *a, uint32_t n, bool is_reg) {
  for (uint32_t i = 1; i < n; ++i) {
    uint16_t t = a[i];
    uint32_t j = i;
    while (j > 0 && addr_of(a[j - 1], is_reg) > addr_of(t, is_reg)) {
      a[j] = a[j - 1];
      --j;
    }
    a[j] = t;
  }
  if (EG2_PARAM(order) == 1) {
    for (uint32_t i = 0; i < n / 2; ++i) {
      uint16_t t = a[i];
      a[i] = a[n - 1 - i];
      a[n - 1 - i] = t;
    }
  }
}

// The lock phase: each REGWEN drawn is locked (a 0 written; it must read 0, and a 1 must not
// reopen it), then a write to the register behind it must change nothing.
static void lock_phase(ctx_t *c) {
  for (uint32_t i = 0; i < c->nlock; ++i) {
    uint32_t idx = c->locks[i];
    const eg2_rm_reg_t *r = &kEg2RmRegs[idx];
    const eg2_rm_block_t *b = NULL;
    for (uint32_t k = 0; k < EG2_RM_BLOCK_COUNT; ++k) {
      if (idx >= kEg2RmBlocks[k].first && idx < kEg2RmBlocks[k].first + kEg2RmBlocks[k].count) {
        b = &kEg2RmBlocks[k];
      }
    }
    volatile uint8_t *blk = (volatile uint8_t *)eg2_rm_ptr(b->base);
    const eg2_rm_reg_t *g = &kEg2RmRegs[r->regwen];
    uint32_t gm = masks_of(g)->known;
    if (!bit_get(c->locked, r->regwen)) {
      wr(c, blk, b, g, 0);
      check(c, b, g, "REGWEN after a 0", rd(c, blk, b, g), 0, gm);
      wr(c, blk, b, g, 0xffffffffu);
      check(c, b, g, "REGWEN reopened by a 1", rd(c, blk, b, g), 0, gm);
      bit_set(c->locked, r->regwen);
      bit_set(c->dirty, r->regwen);
    }
    const eg2_rm_masks_t *m = masks_of(r);
    uint32_t orig = rd(c, blk, b, r);
    uint32_t v = fix_mubi(m, neutral(m, orig) ^ (m->rw & ~m->mubi));
    wr(c, blk, b, r, v);
    check(c, b, r, "written behind a locked REGWEN", rd(c, blk, b, r), orig, m->known);
    wr(c, blk, b, r, neutral(m, orig));
    bit_set(c->dirty, idx);
  }
}

bool eg2_test_main(void) {
  eg2_rm_irq_off();
  if (!eg2_rm_params_begin()) {
    return false;
  }
  ctx_t c = {0};
  c.gaps = EG2_PARAM(model_gaps) != 0;

  // The table's exclusions, counted (per reason in the generated header's comment: the UART
  // is slow, at about 10 us a character, so the run prints totals only).
  uint32_t no_write = 0, no_read = 0, no_reset = 0, gaps = 0;
  for (uint32_t i = 0; i < EG2_RM_REG_COUNT; ++i) {
    const eg2_rm_reg_t *r = &kEg2RmRegs[i];
    no_write += (r->flags & kEg2RmNoWrite) != 0;
    no_read += (r->flags & kEg2RmNoRead) != 0;
    no_reset += (r->flags & kEg2RmNoReset) != 0;
    gaps += (r->flags & (kEg2RmGapR | kEg2RmGapW)) != 0;
  }
  eg2_puts("REG table: ");
  eg2_putdec(EG2_RM_BLOCK_COUNT);
  eg2_puts(" windows, ");
  eg2_putdec(EG2_RM_REG_COUNT);
  eg2_puts(" registers; never written ");
  eg2_putdec(no_write);
  eg2_puts(", never read ");
  eg2_putdec(no_read);
  eg2_puts(", no reset check ");
  eg2_putdec(no_reset);
  eg2_puts(", model gaps ");
  eg2_putdec(gaps);
  eg2_puts("\r\n");

  // The windows, then per window its registers.
  uint16_t win[EG2_RM_BLOCK_COUNT];
  for (uint32_t i = 0; i < EG2_RM_BLOCK_COUNT; ++i) {
    win[i] = (uint16_t)i;
  }
  shuffle(win, EG2_RM_BLOCK_COUNT);
  uint32_t nwin = EG2_PARAM(n_windows);
  if (EG2_PARAM(order) != 2) {
    ordered(win, nwin, false);
  }
  uint16_t regs[EG2_RM_MAX_BLOCK_REGS];
  for (uint32_t w = 0; w < nwin; ++w) {
    const eg2_rm_block_t *b = &kEg2RmBlocks[win[w]];
    volatile uint8_t *blk = (volatile uint8_t *)eg2_rm_ptr(b->base);
    uint32_t fails0 = c.fails, checks0 = c.checks;
    c.block_lines = 0;
    for (uint32_t i = 0; i < b->count; ++i) {
      regs[i] = (uint16_t)(b->first + i);
    }
    if (EG2_PARAM(reset_read_first)) {
      for (uint32_t i = 0; i < b->count; ++i) {
        if (resettable(&c, &kEg2RmRegs[regs[i]], regs[i])) {
          op_reset(&c, blk, b, &kEg2RmRegs[regs[i]], true);
        }
      }
    }
    if (EG2_PARAM(order) == 2) {
      shuffle(regs, b->count);
    } else {
      ordered(regs, b->count, true);
    }
    uint32_t n = b->count < EG2_PARAM(max_regs) ? b->count : EG2_PARAM(max_regs);
    for (uint32_t i = 0; i < n; ++i) {
      one_register(&c, blk, b, regs[i]);
    }
    eg2_puts(c.fails == fails0 ? "REG PASS " : "REG FAIL ");
    eg2_puts(name_of(b->name));
    eg2_puts(" ");
    eg2_putdec(n);
    eg2_puts(" regs ");
    eg2_putdec(c.checks - checks0);
    eg2_puts(" checks\r\n");
  }
  c.block_lines = 0;
  lock_phase(&c);

  eg2_puts("REG summary: ");
  eg2_putdec(nwin);
  eg2_puts(" windows, ");
  eg2_putdec(c.checks);
  eg2_puts(" checks, ");
  eg2_putdec(c.fails);
  eg2_puts(" failures; ops reset ");
  eg2_putdec(c.ops[kOpReset]);
  eg2_puts(" write ");
  eg2_putdec(c.ops[kOpWrite]);
  eg2_puts(" walk ");
  eg2_putdec(c.ops[kOpWalk]);
  eg2_puts(" alias ");
  eg2_putdec(c.ops[kOpAlias]);
  eg2_puts(" shadow ");
  eg2_putdec(c.ops[kOpShadow]);
  eg2_puts("; locked ");
  eg2_putdec(c.nlock);
  eg2_puts("; narrow-write bus errors ");
  eg2_putdec(c.bus_errors);
  eg2_puts("; never read ");
  eg2_putdec(c.excl_regs);
  eg2_puts("; model-gap skips ");
  eg2_putdec(c.gap_skips);
  eg2_puts("\r\n");
  return c.fails == 0 && c.checks > 0;
}
