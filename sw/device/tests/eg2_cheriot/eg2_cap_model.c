// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"

// Every function names the CHERIoT-Sail function or instruction clause it follows.

enum {
  kMantissaMask = 0x1ff,  // cap_mantissa_width = 9
  kMantissa10Mask = 0x3ff,
  kMaxE = 24,             // cap_max_E
};

static uint32_t bitlen(uint32_t v) { return v == 0 ? 0 : 32u - (uint32_t)__builtin_clz(v); }

// encCapabilityToCapability.
eg2_capx_t eg2_cap_decode(eg2_cap_t c) {
  eg2_capx_t x;
  uint32_t cperms = (c.hi >> 25) & 0x3f;
  uint32_t cotype = (c.hi >> 22) & 7;
  uint32_t ce = (c.hi >> 18) & 0xf;
  uint32_t low = cperms & 0x1f;
  uint32_t p = (cperms & 0x20) ? kEg2PermGl : 0;
  bool exe = false;
  if ((low >> 3) == 3) {  // 0b11 @ [SL, LM, LG]: mem-rw-cap
    p |= kEg2PermLd | kEg2PermMc | kEg2PermSd;
    p |= (low & 4) ? kEg2PermSl : 0;
    p |= (low & 2) ? kEg2PermLm : 0;
    p |= (low & 1) ? kEg2PermLg : 0;
  } else if ((low >> 2) == 5) {  // 0b101 @ [LM, LG]: mem-ro-cap
    p |= kEg2PermLd | kEg2PermMc;
    p |= (low & 2) ? kEg2PermLm : 0;
    p |= (low & 1) ? kEg2PermLg : 0;
  } else if (low == 0x10) {  // 0b10000: mem-wo-cap
    p |= kEg2PermSd | kEg2PermMc;
  } else if ((low >> 2) == 4) {  // 0b100 @ [LD, SD]: mem-data
    p |= (low & 2) ? kEg2PermLd : 0;
    p |= (low & 1) ? kEg2PermSd : 0;
  } else if ((low >> 3) == 1) {  // 0b01 @ [SR, LM, LG]: executable
    exe = true;
    p |= kEg2PermEx | kEg2PermLd | kEg2PermMc;
    p |= (low & 4) ? kEg2PermSr : 0;
    p |= (low & 2) ? kEg2PermLm : 0;
    p |= (low & 1) ? kEg2PermLg : 0;
  } else {  // 0b00 @ [U0, SE, US]: sealing
    p |= (low & 4) ? kEg2PermU0 : 0;
    p |= (low & 2) ? kEg2PermSe : 0;
    p |= (low & 1) ? kEg2PermUs : 0;
  }
  x.tag = c.tag;
  x.perms = p;
  x.reserved = c.hi >> 31;
  x.e = ce == 0xf ? (uint32_t)kMaxE : ce;
  x.t = (c.hi >> 9) & kMantissaMask;
  x.b = c.hi & kMantissaMask;
  // Executable capabilities have object types 1-7, the others 9-15; unsealed is 0.
  x.otype = (exe || cotype == 0) ? cotype : (8u | cotype);
  x.addr = c.addr;
  return x;
}

static bool has(uint32_t perms, uint32_t p) { return (perms & p) == p; }

// capToEncCap.
eg2_cap_t eg2_cap_encode(const eg2_capx_t *x) {
  uint32_t p = x->perms;
  uint32_t cp = has(p, kEg2PermGl) ? 0x20u : 0u;
  if (has(p, kEg2PermEx | kEg2PermLd | kEg2PermMc)) {
    cp |= 0x08u | (has(p, kEg2PermSr) ? 4u : 0u) | (has(p, kEg2PermLm) ? 2u : 0u) |
          (has(p, kEg2PermLg) ? 1u : 0u);
  } else if (has(p, kEg2PermLd | kEg2PermMc | kEg2PermSd)) {
    cp |= 0x18u | (has(p, kEg2PermSl) ? 4u : 0u) | (has(p, kEg2PermLm) ? 2u : 0u) |
          (has(p, kEg2PermLg) ? 1u : 0u);
  } else if (has(p, kEg2PermLd | kEg2PermMc)) {
    cp |= 0x14u | (has(p, kEg2PermLm) ? 2u : 0u) | (has(p, kEg2PermLg) ? 1u : 0u);
  } else if (has(p, kEg2PermSd | kEg2PermMc)) {
    cp |= 0x10u;
  } else if (has(p, kEg2PermLd) || has(p, kEg2PermSd)) {
    cp |= 0x10u | (has(p, kEg2PermLd) ? 2u : 0u) | (has(p, kEg2PermSd) ? 1u : 0u);
  } else {
    cp |= (has(p, kEg2PermU0) ? 4u : 0u) | (has(p, kEg2PermSe) ? 2u : 0u) |
          (has(p, kEg2PermUs) ? 1u : 0u);
  }
  uint32_t ce = x->e == (uint32_t)kMaxE ? 0xfu : (x->e & 0xfu);
  eg2_cap_t c;
  c.hi = (x->reserved & 1u) << 31 | cp << 25 | (x->otype & 7u) << 22 | ce << 18 |
         (x->t & kMantissaMask) << 9 | (x->b & kMantissaMask);
  c.addr = x->addr;
  c.tag = x->tag;
  return c;
}

// getCapBoundsBits.
static void bounds(const eg2_capx_t *x, uint32_t *base, uint64_t *top) {
  uint32_t e = x->e;
  uint32_t a = x->addr;
  uint32_t a_mid = (a >> e) & kMantissaMask;
  uint32_t a_hi = a_mid < x->b ? 1u : 0u;
  uint32_t t_hi = x->t < x->b ? 1u : 0u;
  uint32_t a_top = (e + 9 >= 32) ? 0u : a >> (e + 9);
  uint32_t c_b = a_top - a_hi;         // (a_top + c_b), 32 bits
  uint32_t c_t = a_top + t_hi - a_hi;  // (a_top + c_t), 32 bits
  *base = (uint32_t)(((uint64_t)c_b << (e + 9)) | ((uint64_t)x->b << e));
  *top = (((uint64_t)c_t << (e + 9)) | ((uint64_t)x->t << e)) & 0x1ffffffffull;
}

uint32_t eg2_cap_base(eg2_cap_t c) {
  eg2_capx_t x = eg2_cap_decode(c);
  uint32_t base;
  uint64_t top;
  bounds(&x, &base, &top);
  return base;
}

uint64_t eg2_cap_top(eg2_cap_t c) {
  eg2_capx_t x = eg2_cap_decode(c);
  uint32_t base;
  uint64_t top;
  bounds(&x, &base, &top);
  return top;
}

// CGetLen: getCapLength, (top - base) mod 2^33, saturated.
uint32_t eg2_cap_len32(eg2_cap_t c) {
  eg2_capx_t x = eg2_cap_decode(c);
  uint32_t base;
  uint64_t top;
  bounds(&x, &base, &top);
  uint64_t len = (top - base) & 0x1ffffffffull;
  return len > 0xffffffffull ? 0xffffffffu : (uint32_t)len;
}

// CGetTop.
uint32_t eg2_cap_top32(eg2_cap_t c) {
  uint64_t top = eg2_cap_top(c);
  return top > 0xffffffffull ? 0xffffffffu : (uint32_t)top;
}

uint32_t eg2_cap_perms(eg2_cap_t c) { return eg2_cap_decode(c).perms; }
uint32_t eg2_cap_otype(eg2_cap_t c) { return eg2_cap_decode(c).otype; }
uint32_t eg2_cap_exp(eg2_cap_t c) { return eg2_cap_decode(c).e; }
bool eg2_cap_is_sealed(eg2_cap_t c) { return eg2_cap_decode(c).otype != 0; }

static bool in_bounds_x(const eg2_capx_t *x, uint32_t addr, uint32_t size) {
  uint32_t base;
  uint64_t top;
  bounds(x, &base, &top);
  return addr >= base && (uint64_t)addr + size <= top;
}

bool eg2_cap_in_bounds(eg2_cap_t c, uint32_t addr, uint32_t size) {
  eg2_capx_t x = eg2_cap_decode(c);
  return in_bounds_x(&x, addr, size);
}

uint64_t eg2_cap_repr_limit(eg2_cap_t c) {
  eg2_capx_t x = eg2_cap_decode(c);
  return (uint64_t)eg2_cap_base(c) + (1ull << (x.e + 9));
}

// max_bounds_cap and the three roots.
static eg2_cap_t root(uint32_t perms, uint32_t addr) {
  eg2_capx_t x = {
      .tag = true,
      .perms = perms | kEg2PermGl,
      .reserved = 0,
      .e = kMaxE,
      .b = 0,
      .t = 0x100,  // cap_reset_T
      .otype = 0,
      .addr = addr,
  };
  return eg2_cap_encode(&x);
}

eg2_cap_t eg2_cap_root_mem(uint32_t addr) {
  return root(kEg2PermMc | kEg2PermLd | kEg2PermSl | kEg2PermLm | kEg2PermSd | kEg2PermLg, addr);
}

eg2_cap_t eg2_cap_root_exe(uint32_t addr) {
  return root(kEg2PermEx | kEg2PermSr | kEg2PermMc | kEg2PermLd | kEg2PermLm | kEg2PermLg, addr);
}

eg2_cap_t eg2_cap_root_seal(uint32_t addr) {
  return root(kEg2PermU0 | kEg2PermSe | kEg2PermUs, addr);
}

eg2_cap_t eg2_cap_null(uint32_t addr) {
  eg2_cap_t c = {.hi = 0, .addr = addr, .tag = false};
  return c;
}

// capBoundsEqual after an address change (setCapAddr, incCapAddr).
static bool representable(const eg2_capx_t *x, uint32_t addr) {
  eg2_capx_t n = *x;
  n.addr = addr;
  uint32_t b0, b1;
  uint64_t t0, t1;
  bounds(x, &b0, &t0);
  bounds(&n, &b1, &t1);
  return b0 == b1 && t0 == t1;
}

// CSetAddr.
eg2_cap_t eg2_cap_set_addr(eg2_cap_t c, uint32_t addr) {
  eg2_capx_t x = eg2_cap_decode(c);
  bool rep = representable(&x, addr);
  x.tag = x.tag && x.otype == 0 && rep;
  x.addr = addr;
  return eg2_cap_encode(&x);
}

// CIncAddr, CIncAddrImmediate.
eg2_cap_t eg2_cap_inc_addr(eg2_cap_t c, uint32_t delta) {
  return eg2_cap_set_addr(c, c.addr + delta);
}

// setCapBounds; returns whether the bounds are exact.
static bool set_bounds_x(eg2_capx_t *x, uint32_t base, uint32_t len) {
  uint64_t top = (uint64_t)base + len;
  uint32_t e = bitlen(len >> 9);
  uint32_t es = e > 14 ? (uint32_t)kMaxE : e;
  uint32_t b = (base >> es) & kMantissa10Mask;
  uint32_t t = (uint32_t)(top >> es) & kMantissa10Mask;
  uint64_t mask = (1ull << es) - 1;
  bool lost_top = (top & mask) != 0;
  if (lost_top) {
    t = (t + 1) & kMantissa10Mask;
  }
  if (((t - b) & kMantissa10Mask) > 0x1ffu) {
    es = es < 14 ? es + 1 : (uint32_t)kMaxE;
    b = (base >> es) & kMantissa10Mask;
    t = (uint32_t)(top >> es) & kMantissa10Mask;
    mask = (1ull << es) - 1;
    lost_top = (top & mask) != 0;
    if (lost_top) {
      t = (t + 1) & kMantissa10Mask;
    }
  }
  x->addr = base;
  x->e = es;
  x->b = b & kMantissaMask;
  x->t = t & kMantissaMask;
  bool lost_base = ((uint64_t)base & mask) != 0;
  return !(lost_base || lost_top);
}

// setCapBoundsRoundDown.
static void set_bounds_round_down_x(eg2_capx_t *x, uint32_t base, uint32_t len) {
  uint64_t top = (uint64_t)base + len;
  uint32_t e_l = bitlen(len >> 9);
  uint32_t e_b = base == 0 ? 32u : (uint32_t)__builtin_ctz(base);
  uint32_t e = e_l < e_b ? e_l : e_b;
  e = e < 14 ? e : 14;
  uint32_t b = (base >> e) & kMantissaMask;
  uint32_t t = (e_l > e_b || e_l > 14) ? ((b - 1) & kMantissaMask)
                                       : ((uint32_t)(top >> e_l) & kMantissaMask);
  x->addr = base;
  x->e = e;
  x->b = b;
  x->t = t;
}

// CSetBounds, CSetBoundsImmediate, CSetBoundsExact, CSetBoundsRoundDown.
eg2_cap_t eg2_cap_set_bounds(eg2_cap_t c, uint32_t len, eg2_bounds_kind_t kind) {
  eg2_capx_t x = eg2_cap_decode(c);
  bool in = in_bounds_x(&x, x.addr, len);
  x.tag = x.tag && x.otype == 0;
  bool exact = true;
  if (kind == kEg2BoundsRoundDown) {
    set_bounds_round_down_x(&x, x.addr, len);
  } else {
    exact = set_bounds_x(&x, x.addr, len);
  }
  x.tag = x.tag && in && (kind != kEg2BoundsExact || exact);
  return eg2_cap_encode(&x);
}

// CAndPerm.
eg2_cap_t eg2_cap_and_perm(eg2_cap_t c, uint32_t mask) {
  eg2_capx_t x = eg2_cap_decode(c);
  mask &= kEg2PermAll;
  if (x.otype != 0 && (mask | kEg2PermGl) != kEg2PermAll) {
    x.tag = false;
  }
  x.perms &= mask;
  return eg2_cap_encode(&x);
}

// CSeal.
eg2_cap_t eg2_cap_seal(eg2_cap_t c, eg2_cap_t auth) {
  eg2_capx_t x = eg2_cap_decode(c);
  eg2_capx_t a = eg2_cap_decode(auth);
  uint32_t ot = a.addr;
  bool otype_ok = has(x.perms, kEg2PermEx) ? (ot >= 1 && ot <= 7) : (ot > 8 && ot <= 15);
  uint32_t base;
  uint64_t top;
  bounds(&a, &base, &top);
  bool permitted = a.tag && a.otype == 0 && has(a.perms, kEg2PermSe) && ot >= base &&
                   (uint64_t)ot < top && otype_ok;
  x.tag = x.tag && x.otype == 0 && permitted;
  x.otype = ot & 0xfu;
  return eg2_cap_encode(&x);
}

// CUnseal.
eg2_cap_t eg2_cap_unseal(eg2_cap_t c, eg2_cap_t auth) {
  eg2_capx_t x = eg2_cap_decode(c);
  eg2_capx_t a = eg2_cap_decode(auth);
  bool permitted = a.tag && x.otype != 0 && a.otype == 0 && in_bounds_x(&a, x.otype, 1) &&
                   has(a.perms, kEg2PermUs);
  bool global = has(x.perms, kEg2PermGl) && has(a.perms, kEg2PermGl);
  x.otype = 0;
  x.perms = (x.perms & ~(uint32_t)kEg2PermGl) | (global ? kEg2PermGl : 0u);
  x.tag = x.tag && permitted;
  return eg2_cap_encode(&x);
}

// CSetHigh: the bits replaced, the tag cleared, the result normalised.
eg2_cap_t eg2_cap_set_high(eg2_cap_t c, uint32_t hi) {
  eg2_cap_t n = {.hi = hi, .addr = c.addr, .tag = false};
  eg2_capx_t x = eg2_cap_decode(n);
  return eg2_cap_encode(&x);
}

eg2_cap_t eg2_cap_clear_tag(eg2_cap_t c) {
  c.tag = false;
  return c;
}

// CTestSubset cs1 = a, cs2 = b.
uint32_t eg2_cap_test_subset(eg2_cap_t a, eg2_cap_t b) {
  eg2_capx_t xa = eg2_cap_decode(a);
  eg2_capx_t xb = eg2_cap_decode(b);
  uint32_t ba, bb;
  uint64_t ta, tb;
  bounds(&xa, &ba, &ta);
  bounds(&xb, &bb, &tb);
  if (xa.tag != xb.tag || bb < ba || tb > ta || (xb.perms & xa.perms) != xb.perms) {
    return 0;
  }
  return 1;
}

// CSetEqualExact (CSEQX).
uint32_t eg2_cap_equal_exact(eg2_cap_t a, eg2_cap_t b) {
  return (a.tag == b.tag && a.hi == b.hi && a.addr == b.addr) ? 1u : 0u;
}

// getRepresentableAlignmentMask.
uint32_t eg2_cap_cram(uint32_t len) {
  eg2_capx_t x = eg2_cap_decode(eg2_cap_root_mem(0));
  set_bounds_x(&x, 0, len);
  return x.e >= 32 ? 0u : 0xffffffffu << x.e;
}

// getRepresentableLength.
uint32_t eg2_cap_crrl(uint32_t len) {
  uint32_t m = eg2_cap_cram(len);
  return (len + ~m) & m;
}

static uint32_t upper_imm(uint32_t imm20) {
  // sign_extend(imm20) << 11.
  uint32_t s = (imm20 & 0x80000u) ? (imm20 | 0xfff00000u) : (imm20 & 0xfffffu);
  return s << 11;
}

// AUIPCC.
eg2_cap_t eg2_cap_auipcc(eg2_cap_t pcc, uint32_t pc, uint32_t imm20) {
  eg2_capx_t x = eg2_cap_decode(pcc);
  uint32_t addr = pc + upper_imm(imm20);
  x.tag = x.tag && representable(&x, addr);
  x.addr = addr;
  return eg2_cap_encode(&x);
}

// AUICGP.
eg2_cap_t eg2_cap_auicgp(eg2_cap_t cgp, uint32_t imm20) {
  eg2_capx_t x = eg2_cap_decode(cgp);
  uint32_t addr = cgp.addr + upper_imm(imm20);
  x.tag = x.tag && x.otype == 0 && representable(&x, addr);
  x.addr = addr;
  return eg2_cap_encode(&x);
}

// The link capability of CJAL and CJALR.
eg2_cap_t eg2_cap_link(eg2_cap_t pcc, uint32_t next_pc, bool to_ra, bool mie) {
  eg2_capx_t x = eg2_cap_decode(pcc);
  x.addr = next_pc;
  if (to_ra) {
    x.otype = mie ? kEg2OtypeSentryBie : kEg2OtypeSentryBid;
  }
  return eg2_cap_encode(&x);
}

// CSpecialRW's legalisation of MTCC and MEPCC.
eg2_cap_t eg2_cap_scr_legalise(uint32_t scr, eg2_cap_t c) {
  eg2_capx_t x = eg2_cap_decode(c);
  uint32_t align = scr == 28 ? 3u : 1u;
  bool invalid = (x.addr & align) != 0 || x.otype != 0 || !has(x.perms, kEg2PermEx);
  x.addr &= ~align;
  x.tag = x.tag && !invalid;
  return eg2_cap_encode(&x);
}

// ext_data_get_addr (integer accesses), LoadCapImm and StoreCapImm.
int eg2_cap_check_access(eg2_cap_t auth, uint32_t addr, uint32_t width, eg2_access_t access,
                         bool stored_tag) {
  eg2_capx_t x = eg2_cap_decode(auth);
  if (!x.tag) {
    return kEg2CauseTag;
  }
  if (x.otype != 0) {
    return kEg2CauseSeal;
  }
  bool store = access == kEg2AccessStore || access == kEg2AccessStoreCap;
  if (!store && !has(x.perms, kEg2PermLd)) {
    return kEg2CauseLd;
  }
  if (store && !has(x.perms, kEg2PermSd)) {
    return kEg2CauseSd;
  }
  if (access == kEg2AccessStoreCap && !has(x.perms, kEg2PermMc) && stored_tag) {
    return kEg2CauseSc;
  }
  if (!in_bounds_x(&x, addr, width)) {
    return kEg2CauseBounds;
  }
  if ((access == kEg2AccessLoadCap || access == kEg2AccessStoreCap) && (addr & 7u) != 0) {
    return -1;
  }
  return 0;
}

// LoadCapImm: the loaded capability as written to cd (no revocation: the bitmap is clear).
eg2_cap_t eg2_cap_clc_result(eg2_cap_t auth, eg2_cap_t mem) {
  eg2_capx_t a = eg2_cap_decode(auth);
  eg2_capx_t r = eg2_cap_decode(mem);
  if (!has(a.perms, kEg2PermMc)) {
    r.tag = false;
  }
  if (r.tag && !has(a.perms, kEg2PermLg)) {
    r.perms &= ~(uint32_t)kEg2PermGl;
    if (r.otype == 0) {
      r.perms &= ~(uint32_t)kEg2PermLg;
    }
  }
  if (r.tag && !has(a.perms, kEg2PermLm) && r.otype == 0) {
    r.perms &= ~(uint32_t)(kEg2PermSd | kEg2PermLm);
  }
  return eg2_cap_encode(&r);
}

// StoreCapImm: what reaches memory.
eg2_cap_t eg2_cap_csc_stored(eg2_cap_t auth, eg2_cap_t value) {
  eg2_capx_t a = eg2_cap_decode(auth);
  eg2_capx_t v = eg2_cap_decode(value);
  if (!has(a.perms, kEg2PermSl) && !has(v.perms, kEg2PermGl)) {
    value.tag = false;
  }
  return value;
}

// CJALR's checks.
int eg2_cap_check_cjalr(eg2_cap_t target, uint32_t imm, bool cd_is_ra, bool cd_is_zero,
                        bool cs1_is_ra) {
  eg2_capx_t x = eg2_cap_decode(target);
  bool sealed = x.otype != 0;
  bool allowed;
  if (cd_is_zero && cs1_is_ra) {
    allowed = x.otype == kEg2OtypeSentryBid || x.otype == kEg2OtypeSentryBie;
  } else if (cd_is_ra) {
    allowed = !sealed || (x.otype >= kEg2OtypeSentry && x.otype <= kEg2OtypeSentryIe);
  } else {
    allowed = !sealed || x.otype == kEg2OtypeSentry;
  }
  if (!x.tag) {
    return kEg2CauseTag;
  }
  if ((sealed && imm != 0) || !allowed) {
    return kEg2CauseSeal;
  }
  if (!has(x.perms, kEg2PermEx)) {
    return kEg2CauseEx;
  }
  return 0;
}
