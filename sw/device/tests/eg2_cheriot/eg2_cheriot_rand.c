// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"

#include "sw/device/lib/testing/eg2/eg2_params.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"

// ----------------------------------------- Weights -------------------------------------------

uint32_t eg2_pick(uint32_t n) { return n <= 1 ? 0 : eg2_rand_range(0, n - 1); }

void eg2_family_draw(eg2_family_t *f, uint32_t n, uint32_t plain_mask, uint32_t boost) {
  if (n > kEg2FamilyMax) {
    n = kEg2FamilyMax;
  }
  f->n = n;
  f->plain_mask = plain_mask;
  f->boosted = n;
  bool do_boost;
  uint32_t b = 0;
  if (boost == 0) {
    do_boost = eg2_rand_range(0, 4) == 0;  // one run in five
    b = eg2_pick(n);
  } else if (boost == kEg2BoostNone) {
    do_boost = false;
  } else {
    do_boost = true;
    b = (boost - 1) % n;
  }
  for (uint32_t i = 0; i < n; ++i) {
    f->w[i] = eg2_rand_range(1, 100);
  }
  if (do_boost && n > 1) {
    uint32_t others = 0;
    for (uint32_t i = 0; i < n; ++i) {
      others += i == b ? 0 : f->w[i];
    }
    f->w[b] = others;  // twice the boosted class is the total
    f->boosted = b;
    return;
  }
  uint32_t all = n >= 32 ? 0xffffffffu : (1u << n) - 1;
  plain_mask &= all;
  if (plain_mask == 0 || plain_mask == all) {
    return;
  }
  // Boundary classes scaled up so the plain share can be set to a whole percentage.
  uint32_t sum_b = 0, sum_v = 0, n_plain = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if ((plain_mask >> i) & 1) {
      sum_v += f->w[i];
      ++n_plain;
    } else {
      f->w[i] *= 100;
      sum_b += f->w[i];
    }
  }
  uint32_t pct = eg2_rand_range(5, 30);
  uint32_t target = sum_b * pct / (100 - pct);
  if (target < n_plain) {
    target = n_plain;
  }
  for (uint32_t i = 0; i < n; ++i) {
    if ((plain_mask >> i) & 1) {
      uint32_t w = target * f->w[i] / sum_v;  // < 2^24: no 64-bit division
      f->w[i] = w == 0 ? 1 : w;
    }
  }
}

uint32_t eg2_family_pick(const eg2_family_t *f) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < f->n; ++i) {
    total += f->w[i];
  }
  if (total == 0) {
    return 0;
  }
  uint32_t r = eg2_rand_range(0, total - 1);
  for (uint32_t i = 0; i < f->n; ++i) {
    if (r < f->w[i]) {
      return i;
    }
    r -= f->w[i];
  }
  return f->n - 1;
}

void eg2_family_print(const char *name, const eg2_family_t *f) {
  eg2_puts("EG2 weights ");
  eg2_puts(name);
  eg2_puts(":");
  for (uint32_t i = 0; i < f->n; ++i) {
    eg2_puts(" ");
    eg2_putdec(f->w[i]);
  }
  if (f->boosted < f->n) {
    eg2_puts(" boost ");
    eg2_putdec(f->boosted);
  }
  eg2_puts("\r\n");
}

uint32_t eg2_draw_count(uint32_t param, uint32_t lo, uint32_t mid, uint32_t hi) {
  if (param != 0) {
    return param;
  }
  uint32_t r = eg2_rand_range(0, 99);
  if (r < 20) {
    return lo;
  }
  if (r < 80) {
    return eg2_rand_range(lo + 1, mid);
  }
  return eg2_rand_range(mid + 1, hi);
}

// ----------------------------------------- Tally ---------------------------------------------

void eg2_put_kv(const char *key, uint32_t value) {
  eg2_puts(key);
  eg2_puts("=0x");
  eg2_puthex(value);
}

bool eg2_check(eg2_tally_t *t, const char *what, uint32_t idx, uint32_t got, uint32_t exp) {
  ++t->checks;
  if (got == exp) {
    return true;
  }
  ++t->failed;
  if (t->failed <= kEg2MaxReports) {
    eg2_puts("MISMATCH ");
    eg2_puts(what);
    eg2_puts(" #");
    eg2_putdec(idx);
    eg2_puts(": got 0x");
    eg2_puthex(got);
    eg2_puts(" expected 0x");
    eg2_puthex(exp);
    eg2_puts("\r\n");
  }
  return false;
}

void eg2_tally_print(const char *phase, const eg2_tally_t *t) {
  eg2_puts("EG2 ");
  eg2_puts(phase);
  eg2_puts(": ");
  eg2_putdec(t->done);
  eg2_puts("/");
  eg2_putdec(t->planned);
  eg2_puts(" done, ");
  eg2_putdec(t->checks);
  eg2_puts(" checks, ");
  eg2_putdec(t->failed);
  eg2_puts(" failed\r\n");
}

bool eg2_tally_ok(const eg2_tally_t *t) { return t->failed == 0 && t->done == t->planned; }

// ----------------------------------------- Memory --------------------------------------------

void *eg2_mem(uint32_t addr, uint32_t len) {
#ifdef __CHERIOT__
  void *c;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(c));
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(addr));
  __asm__ volatile("csetbounds %0, %0, %1" : "+C"(c) : "r"(len));
  return c;
#else
  (void)len;
  return (void *)(uintptr_t)addr;
#endif
}

#define EG2_SYM_ADDR_(fn, sym)                                                         \
  uint32_t fn(void) {                                                                  \
    uint32_t a;                                                                        \
    __asm__("lui %0, %%hi(" #sym ")\n addi %0, %0, %%lo(" #sym ")" : "=r"(a));         \
    return a;                                                                          \
  }
EG2_SYM_ADDR_(eg2_sym_stack_start, _stack_start)
EG2_SYM_ADDR_(eg2_sym_stack_end, _stack_end)
EG2_SYM_ADDR_(eg2_sym_data_start, _data_start)
EG2_SYM_ADDR_(eg2_sym_bss_end, _bss_end)

uint32_t eg2_main_arena(void) {
  uint32_t base = (eg2_sym_stack_start() - 0x1000u) & ~63u;
  if (base < eg2_sym_bss_end()) {
    return 0;
  }
  return base;
}

bool eg2_sec_sram_init(void) {
  // Full-word stores first: a sub-word store or a read of SRAM no write has initialised would
  // meet its integrity bits uninitialised. The arenas and the work area are the whole of sec SRAM.
  volatile uint32_t *p = eg2_mem(kEg2SecBase, kEg2SecSize);
  for (uint32_t i = 0; i < kEg2SecSize / 4; ++i) {
    p[i] = 0;
  }
  volatile uint32_t *r = eg2_mem(kEg2RetArena, kEg2RetArenaSize);
  for (uint32_t i = 0; i < kEg2RetArenaSize / 4; ++i) {
    r[i] = 0;
  }
  uint32_t main = eg2_main_arena();
  if (main == 0) {
    return false;
  }
  volatile uint32_t *m = eg2_mem(main, kEg2MainArenaSize);
  for (uint32_t i = 0; i < kEg2MainArenaSize / 4; ++i) {
    m[i] = 0;
  }
  return true;
}

// --------------------------------- CHERIoT capability pairs -----------------------------------

#ifdef __CHERIOT__

static void *sealing_root;

eg2_cap_t eg2_hw_read(void *c) {
  uint32_t hi, addr, tag;
  __asm__ volatile("cgethigh %0, %1" : "=r"(hi) : "C"(c));
  // CGetAddr (the assembler turns `cgetaddr` into `mv`).
  __asm__ volatile(".insn r 0x5b, 0, 0x7f, %0, %1, x15" : "=r"(addr) : "C"(c));
  __asm__ volatile("cgettag %0, %1" : "=r"(tag) : "C"(c));
  eg2_cap_t r = {.hi = hi, .addr = addr, .tag = tag != 0};
  return r;
}

eg2_cp_t eg2_cp_adopt(void *hw) {
  eg2_cp_t r = {.hw = hw, .m = eg2_hw_read(hw)};
  return r;
}

eg2_cp_t eg2_cp_root_mem(void) {
  eg2_cp_t r;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(r.hw));
  r.m = eg2_cap_root_mem(eg2_hw_read(r.hw).addr);
  return r;
}

eg2_cp_t eg2_cp_root_exe(void) {
  eg2_cp_t r;
  __asm__ volatile("auipcc %0, 0" : "=C"(r.hw));
  r.m = eg2_cap_root_exe(eg2_hw_read(r.hw).addr);
  return r;
}

void eg2_cp_save_sealing_root(void) {
  void *c;
  __asm__ volatile("cspecialr %0, mscratchc" : "=C"(c));
  sealing_root = c;
}

eg2_cp_t eg2_cp_root_seal(void) {
  eg2_cp_t r = {.hw = sealing_root};
  r.m = eg2_cap_root_seal(eg2_hw_read(r.hw).addr);
  return r;
}

eg2_cp_t eg2_cp_set_addr(eg2_cp_t c, uint32_t addr) {
  eg2_cp_t r;
  __asm__ volatile("csetaddr %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(addr));
  r.m = eg2_cap_set_addr(c.m, addr);
  return r;
}

eg2_cp_t eg2_cp_inc(eg2_cp_t c, uint32_t delta) {
  eg2_cp_t r;
  __asm__ volatile("cincoffset %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(delta));
  r.m = eg2_cap_inc_addr(c.m, delta);
  return r;
}

uint32_t eg2_inc_imm_value(uint32_t which) {
  switch (which) {
    case 0:
      return 0;
    case 1:
      return 1;
    case 2:
      return 0xffffffffu;
    case 3:
      return 2047;
    case 4:
      return 0xfffff800u;  // -2048
    case 5:
      return 8;
    default:
      return 0xfffffff8u;  // -8
  }
}

eg2_cp_t eg2_cp_inc_imm(eg2_cp_t c, uint32_t which) {
  eg2_cp_t r;
  switch (which) {
    case 0:
      __asm__ volatile("cincoffsetimm %0, %1, 0" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 1:
      __asm__ volatile("cincoffsetimm %0, %1, 1" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 2:
      __asm__ volatile("cincoffsetimm %0, %1, -1" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 3:
      __asm__ volatile("cincoffsetimm %0, %1, 2047" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 4:
      __asm__ volatile("cincoffsetimm %0, %1, -2048" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 5:
      __asm__ volatile("cincoffsetimm %0, %1, 8" : "=C"(r.hw) : "C"(c.hw));
      break;
    default:
      __asm__ volatile("cincoffsetimm %0, %1, -8" : "=C"(r.hw) : "C"(c.hw));
      break;
  }
  r.m = eg2_cap_inc_addr(c.m, eg2_inc_imm_value(which));
  return r;
}

eg2_cp_t eg2_cp_bounds(eg2_cp_t c, uint32_t len, eg2_bounds_kind_t kind) {
  eg2_cp_t r;
  switch (kind) {
    case kEg2BoundsExact:
      __asm__ volatile("csetboundsexact %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(len));
      break;
    case kEg2BoundsRoundDown:
      __asm__ volatile("csetboundsrounddown %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(len));
      break;
    default:
      __asm__ volatile("csetbounds %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(len));
      break;
  }
  r.m = eg2_cap_set_bounds(c.m, len, kind);
  return r;
}

uint32_t eg2_bounds_imm_value(uint32_t which) {
  switch (which) {
    case 0:
      return 0;
    case 1:
      return 1;
    case 2:
      return 8;
    case 3:
      return 511;
    case 4:
      return 512;
    case 5:
      return 1001;
    default:
      return 4095;
  }
}

eg2_cp_t eg2_cp_bounds_imm(eg2_cp_t c, uint32_t which) {
  eg2_cp_t r;
  switch (which) {
    case 0:
      __asm__ volatile("csetboundsimm %0, %1, 0" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 1:
      __asm__ volatile("csetboundsimm %0, %1, 1" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 2:
      __asm__ volatile("csetboundsimm %0, %1, 8" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 3:
      __asm__ volatile("csetboundsimm %0, %1, 511" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 4:
      __asm__ volatile("csetboundsimm %0, %1, 512" : "=C"(r.hw) : "C"(c.hw));
      break;
    case 5:
      __asm__ volatile("csetboundsimm %0, %1, 1001" : "=C"(r.hw) : "C"(c.hw));
      break;
    default:
      __asm__ volatile("csetboundsimm %0, %1, 4095" : "=C"(r.hw) : "C"(c.hw));
      break;
  }
  r.m = eg2_cap_set_bounds(c.m, eg2_bounds_imm_value(which), kEg2BoundsInexact);
  return r;
}

eg2_cp_t eg2_cp_and_perm(eg2_cp_t c, uint32_t mask) {
  eg2_cp_t r;
  __asm__ volatile("candperm %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(mask));
  r.m = eg2_cap_and_perm(c.m, mask);
  return r;
}

eg2_cp_t eg2_cp_seal(eg2_cp_t c, eg2_cp_t auth) {
  eg2_cp_t r;
  __asm__ volatile("cseal %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "C"(auth.hw));
  r.m = eg2_cap_seal(c.m, auth.m);
  return r;
}

eg2_cp_t eg2_cp_unseal(eg2_cp_t c, eg2_cp_t auth) {
  eg2_cp_t r;
  __asm__ volatile("cunseal %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "C"(auth.hw));
  r.m = eg2_cap_unseal(c.m, auth.m);
  return r;
}

eg2_cp_t eg2_cp_set_high(eg2_cp_t c, uint32_t hi) {
  eg2_cp_t r;
  __asm__ volatile("csethigh %0, %1, %2" : "=C"(r.hw) : "C"(c.hw), "r"(hi));
  r.m = eg2_cap_set_high(c.m, hi);
  return r;
}

eg2_cp_t eg2_cp_clear_tag(eg2_cp_t c) {
  eg2_cp_t r;
  __asm__ volatile("ccleartag %0, %1" : "=C"(r.hw) : "C"(c.hw));
  r.m = eg2_cap_clear_tag(c.m);
  return r;
}

eg2_cp_t eg2_cp_move(eg2_cp_t c) {
  eg2_cp_t r;
  __asm__ volatile("cmove %0, %1" : "=C"(r.hw) : "C"(c.hw));
  r.m = c.m;
  return r;
}

void eg2_cap_print(const char *what, eg2_cap_t c) {
  eg2_puts("  ");
  eg2_puts(what);
  eg2_puts(c.tag ? " tag 1 " : " tag 0 ");
  eg2_puthex(c.hi);
  eg2_puts(" ");
  eg2_puthex(c.addr);
  eg2_puts(" [");
  eg2_puthex(eg2_cap_base(c));
  eg2_puts(", ");
  uint64_t top = eg2_cap_top(c);
  eg2_puts(top > 0xffffffffull ? "1" : "0");
  eg2_puthex((uint32_t)top);
  eg2_puts(") p ");
  eg2_puthex(eg2_cap_perms(c));
  eg2_puts(" t ");
  eg2_putdec(eg2_cap_otype(c));
  eg2_puts("\r\n");
}

static bool cp_check(eg2_tally_t *t, const char *what, uint32_t idx, eg2_cp_t c, bool full) {
  eg2_cap_t h = eg2_hw_read(c.hw);
  uint32_t failed = t->failed;
  eg2_check(t, what, idx, h.tag ? 1u : 0u, c.m.tag ? 1u : 0u);
  eg2_check(t, what, idx, h.hi, c.m.hi);
  eg2_check(t, what, idx, h.addr, c.m.addr);
  if (full) {
    uint32_t base, top, len, perm, type;
    __asm__ volatile("cgetbase %0, %1" : "=r"(base) : "C"(c.hw));
    __asm__ volatile("cgettop %0, %1" : "=r"(top) : "C"(c.hw));
    __asm__ volatile("cgetlen %0, %1" : "=r"(len) : "C"(c.hw));
    __asm__ volatile("cgetperm %0, %1" : "=r"(perm) : "C"(c.hw));
    __asm__ volatile("cgettype %0, %1" : "=r"(type) : "C"(c.hw));
    eg2_check(t, what, idx, base, eg2_cap_base(c.m));
    eg2_check(t, what, idx, top, eg2_cap_top32(c.m));
    eg2_check(t, what, idx, len, eg2_cap_len32(c.m));
    eg2_check(t, what, idx, perm, eg2_cap_perms(c.m));
    eg2_check(t, what, idx, type, eg2_cap_otype(c.m));
  }
  if (t->failed != failed && failed < kEg2MaxReports) {
    eg2_cap_print("hw   ", h);
    eg2_cap_print("model", c.m);
  }
  return t->failed == failed;
}

bool eg2_cp_check(eg2_tally_t *t, const char *what, uint32_t idx, eg2_cp_t c) {
  return cp_check(t, what, idx, c, false);
}

bool eg2_cp_check_full(eg2_tally_t *t, const char *what, uint32_t idx, eg2_cp_t c) {
  return cp_check(t, what, idx, c, true);
}

#endif  // __CHERIOT__
