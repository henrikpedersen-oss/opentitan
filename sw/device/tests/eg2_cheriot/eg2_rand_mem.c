// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_mem (CHERIoT mode: `_cheriot`) and its RISC-V twin (EG2 verification spec,
 * "Memory, tags and devices" and "chip_sw_eg2_rand_mem, RISC-V mode"; REQ_CIB_06, REQ_CMS_09,
 * REQ_CMS_10, REQ_PER_02, REQ_SRAM_01, REQ_SEC_26).
 *
 * Every access kind (lb, lbu, lh, lhu, lw, sb, sh, sw, c.lw, c.sw and, in CHERIoT mode, clc, csc,
 * c.clc, c.csc) at the edges of what may be accessed, with the outcome computed from the
 * architecture, never read back: the trap (mcause, mtval: the CHERI cause and register index, or
 * the address), the loaded value from the test's shadow of memory, and the loaded capability and
 * its tag from the model (eg2_cap_model.h) and the region's tag rule (REQ_CMS_10). The
 * scoreboard checks every instruction too.
 *
 * CHERIoT mode:
 *  1. The capabilities the start-up code derived: CSP over the stack and CGP over the globals
 *     against the model of their derivation; loads at their last word and at their tops.
 *  2. Bounds edges: for every access kind and width w, the cursor at base - 1, base, base + 1,
 *     top - w - 1, top - w, top - w + 1, top - 1, top and top + w, over capabilities of exponent
 *     0 (lengths 1, 7, 8, 9, 64, 511 on aligned and odd bases), rounded (1001), exponent 6, zero
 *     length, sec SRAM's first and last bytes, retention SRAM (untagged), main SRAM, based at 0,
 *     ending at 2^32, and the root at 0 and 2^32 - 1 (bounds checked with 33-bit arithmetic).
 *  3. Permissions: every permission removed (LD, SD, MC, SL, GL, LG, LM), none, sealed, untagged,
 *     at every access kind; CSC of global, local, sealed and untagged capabilities; CLC's
 *     attenuation without LG and LM.
 *  4. Device windows: a capability with the window's exact bounds and one ending one register past
 *     INTR_STATE: the register, the register past the top, the window's top.
 *  5. `n_accesses` random accesses: region by weight (main SRAM 25, sec SRAM 15, RRAM 15,
 *     retention SRAM 10, ROM 5, device 20, unmapped 10), width by weight (byte 20, half 20, word
 *     30, capability 30, 30 % misaligned), the authority's shape, permissions and cursor by the
 *     suite's classes; `store_over_cap` % of integer stores over a stored capability,
 *     `local_store` % of capability stores of a local capability.
 *
 * RISC-V mode (the twin's bounds are ePMP regions):
 *  1. ePMP region edges: TOR, NA4 and NAPOT (8, 64, 4096 bytes) regions with no access, R and RW,
 *     locked (checked in M mode, and in U mode) and unlocked (U mode, and M mode where it does not
 *     apply), every access kind at lo - w, lo - 1, lo, hi - w, hi - 1 and hi.
 *  2. Random accesses by region and width as in CHERIoT mode, `u_mode_pct` % from U mode.
 *
 * Built with EG2_RAND_MEM_INJECT_FAIL, the first edge case that must trap is expected not to
 * (CHERIoT: lw one word past the top; RISC-V: a load the ePMP denies). The run must fail.
 *
 * `model_gaps` (eg2sim only; 0 on the chip): no capability based at or above 2^31 is made
 * (addr_hi 0), since eg2sim's MPACT-CHERIoT faults a CLC of one (status/dv_todo.md C2). Every
 * check counts.
 */

#include "hw/top/top_memory.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

// 0 draws in software from the seed (eg2_cheriot_rand.h); addr_hi as in eg2_rand_cap_ops.
#define RAND_MEM_PARAMS(X)        \
  X(n_accesses, 0, 0, 4096)       \
  X(boost, 0, 0, 255)             \
  X(store_over_cap, 101, 0, 101)  \
  X(local_store, 101, 0, 101)     \
  X(u_mode_pct, 101, 0, 101)      \
  X(addr_hi, 1, 0, 1)             \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(RAND_MEM_PARAMS);

// addr_hi, off under model_gaps (the header).
__attribute__((unused)) static bool addr_hi(void) {
  return EG2_PARAM(addr_hi) != 0 && EG2_PARAM(model_gaps) == 0;
}

// A percentage knob: 101 (the default) draws it from the seed in [0, hi].
static uint32_t pct_knob(uint32_t v, uint32_t hi) { return v <= 100 ? v : eg2_rand_range(0, hi); }

// --------------------------------------- Access kinds ----------------------------------------

typedef enum op {
  kOpLb,
  kOpLbu,
  kOpLh,
  kOpLhu,
  kOpLw,
  kOpSb,
  kOpSh,
  kOpSw,
  kOpCLw,
  kOpCSw,
#ifdef __CHERIOT__
  kOpClc,
  kOpCsc,
  kOpCClc,
  kOpCCsc,
#endif
  kOpCount,
} op_t;

static uint32_t op_width(op_t op) {
  switch (op) {
    case kOpLb:
    case kOpLbu:
    case kOpSb:
      return 1;
    case kOpLh:
    case kOpLhu:
    case kOpSh:
      return 2;
    case kOpLw:
    case kOpSw:
    case kOpCLw:
    case kOpCSw:
      return 4;
    default:
      return 8;
  }
}

static bool op_store(op_t op) {
  switch (op) {
    case kOpSb:
    case kOpSh:
    case kOpSw:
    case kOpCSw:
#ifdef __CHERIOT__
    case kOpCsc:
    case kOpCCsc:
#endif
      return true;
    default:
      return false;
  }
}

__attribute__((unused)) static bool op_cap(op_t op) { return op_width(op) == 8; }

static const char *op_name(op_t op) {
  switch (op) {
    case kOpLb:
      return "lb";
    case kOpLbu:
      return "lbu";
    case kOpLh:
      return "lh";
    case kOpLhu:
      return "lhu";
    case kOpLw:
      return "lw";
    case kOpSb:
      return "sb";
    case kOpSh:
      return "sh";
    case kOpSw:
      return "sw";
    case kOpCLw:
      return "c.lw";
    case kOpCSw:
      return "c.sw";
#ifdef __CHERIOT__
    case kOpClc:
      return "clc";
    case kOpCsc:
      return "csc";
    case kOpCClc:
      return "c.clc";
    case kOpCCsc:
      return "c.csc";
#endif
    default:
      return "?";
  }
}

static char *op_stub(op_t op) {
  switch (op) {
    case kOpLb:
      return eg2_stub_lb;
    case kOpLbu:
      return eg2_stub_lbu;
    case kOpLh:
      return eg2_stub_lh;
    case kOpLhu:
      return eg2_stub_lhu;
    case kOpLw:
      return eg2_stub_lw;
    case kOpSb:
      return eg2_stub_sb;
    case kOpSh:
      return eg2_stub_sh;
    case kOpSw:
      return eg2_stub_sw;
    case kOpCLw:
      return eg2_stub_c_lw;
    case kOpCSw:
      return eg2_stub_c_sw;
#ifdef __CHERIOT__
    case kOpClc:
      return eg2_stub_clc;
    case kOpCsc:
      return eg2_stub_csc;
    case kOpCClc:
      return eg2_stub_c_clc;
    default:
      return eg2_stub_c_csc;
#else
    default:
      return eg2_stub_c_sw;
#endif
  }
}

// ---------------------------------- Memory and its shadow ------------------------------------

// What the bus does at an address (the memory map; EG2 spec REQ_CMS_09, REQ_CMS_10).
typedef enum mem_class {
  kMemUnmapped,  // bus error
  kMemShadowed,  // RAM the test owns: data and tags known
  kMemReadOnly,  // ROM and RRAM: readable, contents not modelled
  kMemDevice,    // a device register: readable, value not modelled
  kMemUnknown,   // anything else: never accessed in bounds
} mem_class_t;

// The shadowed memories: [base, base + size), tagged or not, their shadow's offset in the work
// area. The work area holds the data shadows, then one byte per granule for the tags.
typedef struct arena {
  uint32_t base, size, shadow;
  bool tagged;
} arena_t;

enum { kArenaCount = 4, kTagShadow = 0x5000 };
static uint32_t main_arena_base;

static arena_t arena(uint32_t i) {
  arena_t a;
  switch (i) {
    case 0:  // sec SRAM's first 8 KiB: its first granule and the boundary with main SRAM
      a.base = kEg2ArenaA;
      a.size = kEg2ArenaSize;
      a.shadow = 0;
      a.tagged = true;
      break;
    case 1:  // sec SRAM's last 8 KiB: its last granule
      a.base = kEg2ArenaB;
      a.size = kEg2ArenaSize;
      a.shadow = kEg2ArenaSize;
      a.tagged = true;
      break;
    case 2:  // retention SRAM: untagged
      a.base = kEg2RetArena;
      a.size = kEg2RetArenaSize;
      a.shadow = 2 * kEg2ArenaSize;
      a.tagged = false;
      break;
    default:  // main SRAM below the stack
      a.base = main_arena_base;
      a.size = kEg2MainArenaSize;
      a.shadow = 2 * kEg2ArenaSize + kEg2RetArenaSize;
      a.tagged = true;
      break;
  }
  return a;
}

static volatile uint8_t *work;  // the work area: shadows

// The arena holding [addr, addr + w), or -1.
static int arena_of(uint32_t addr, uint32_t w) {
  for (uint32_t i = 0; i < kArenaCount; ++i) {
    arena_t a = arena(i);
    if (a.base != 0 && addr >= a.base && (uint64_t)addr + w <= (uint64_t)a.base + a.size) {
      return (int)i;
    }
  }
  return -1;
}

// The device windows and their INTR_STATE registers (pointer-free tables: .rodata, inside .text).
static const uint32_t kWinBase[] = {
#define WIN_BASE_(id, name, base, size) base,
    EG2_DEV_TABLE(WIN_BASE_)
#undef WIN_BASE_
};
static const uint32_t kWinSize[] __attribute__((unused)) = {
#define WIN_SIZE_(id, name, base, size) size,
    EG2_DEV_TABLE(WIN_SIZE_)
#undef WIN_SIZE_
};
static const uint32_t kIntrRows[][2] = {
#define INTR_ROW_(dev, state, test, mask) {dev, state},
    EG2_INTR_TABLE(INTR_ROW_)
#undef INTR_ROW_
};
enum { kIntrRowCount = sizeof(kIntrRows) / sizeof(kIntrRows[0]) };

// A device's INTR_STATE: reading it has no side effect.
static bool is_intr_state(uint32_t addr) {
  for (uint32_t r = 0; r < kIntrRowCount; ++r) {
    if (addr == kWinBase[kIntrRows[r][0]] + kIntrRows[r][1]) {
      return true;
    }
  }
  return false;
}

static bool in_range(uint32_t addr, uint32_t w, uint32_t base, uint32_t size) {
  return addr >= base && (uint64_t)addr + w <= (uint64_t)base + size;
}

static mem_class_t mem_class(uint32_t addr, uint32_t w) {
  if (arena_of(addr, w) >= 0) {
    return kMemShadowed;
  }
  if (w == 4 && is_intr_state(addr)) {
    return kMemDevice;
  }
  if (in_range(addr, w, kEg2RomBase, kEg2RomSize) || in_range(addr, w, kEg2RramBase, kEg2RramSize)) {
    return kMemReadOnly;
  }
  // Below rv_dm's memory and above the last window nothing answers.
  if ((uint64_t)addr + w <= 0x10000u || addr >= 0x50000000u) {
    return kMemUnmapped;
  }
  return kMemUnknown;
}

static uint32_t shadow_off(uint32_t addr) {
  arena_t a = arena((uint32_t)arena_of(addr, 1));
  return a.shadow + (addr - a.base);
}

static uint32_t tag_off(uint32_t addr) {
  arena_t a = arena((uint32_t)arena_of(addr, 1));
  return kTagShadow + a.shadow / 8 + (addr - a.base) / 8;
}

static uint32_t shadow_read(uint32_t addr, uint32_t w) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < w; ++i) {
    v |= (uint32_t)work[shadow_off(addr + i)] << (8 * i);
  }
  return v;
}

// An integer store: the bytes, and the tag of every granule it touches cleared.
static void shadow_write(uint32_t addr, uint32_t w, uint32_t v) {
  for (uint32_t i = 0; i < w; ++i) {
    work[shadow_off(addr + i)] = (uint8_t)(v >> (8 * i));
    work[tag_off(addr + i)] = 0;
  }
}

// Fills the arenas with a pattern from the seed, through plain integer stores.
static void arenas_fill(void) {
  for (uint32_t i = 0; i < kArenaCount; ++i) {
    arena_t a = arena(i);
    if (a.base == 0) {
      continue;
    }
    volatile uint32_t *m = eg2_mem(a.base, a.size);
    for (uint32_t k = 0; k < a.size / 4; ++k) {
      uint32_t v = eg2_rand();
      m[k] = v;
      shadow_write(a.base + 4 * k, 4, v);
    }
  }
}

// ------------------------------------- One access --------------------------------------------

typedef struct outcome {
  bool trapped;
  uint32_t mcause, mtval;
  bool value_known;
  uint32_t value;
#ifdef __CHERIOT__
  eg2_cap_t cap;  // CLC's result
#endif
} outcome_t;

static uint32_t sign_extend(uint32_t v, uint32_t w) {
  uint32_t shift = 32 - 8 * w;
  return (uint32_t)((int32_t)(v << shift) >> shift);
}

// The bus's outcome of an access that passed the core's checks; updates the shadow on a store.
static outcome_t bus_outcome(op_t op, uint32_t addr, uint32_t store_value, bool *ok_to_run) {
  outcome_t o = {0};
  uint32_t w = op_width(op);
  bool store = op_store(op);
  switch (mem_class(addr, w)) {
    case kMemShadowed:
      if (!store) {
        uint32_t v = shadow_read(addr, w < 4 ? w : 4);
        o.value_known = true;
        o.value = (op == kOpLb || op == kOpLh) ? sign_extend(v, w) : v;
      }
      (void)store_value;
      break;
    case kMemReadOnly:
    case kMemDevice:
      *ok_to_run = !store;  // never written
      break;
    case kMemUnmapped:
      o.trapped = true;
      o.mcause = store ? kEg2McauseStoreFault : kEg2McauseLoadFault;
      o.mtval = addr;
      break;
    default:
      *ok_to_run = false;
      break;
  }
  return o;
}

// Compares an outcome; prints the access on a mismatch.
static void compare(eg2_tally_t *t, const char *what, uint32_t idx, op_t op, uint32_t addr,
                    const outcome_t *exp, bool trapped) {
  uint32_t failed = t->failed;
  eg2_check(t, what, idx, trapped ? 1u : 0u, exp->trapped ? 1u : 0u);
  if (trapped && exp->trapped) {
    eg2_check(t, what, idx, eg2_trap_rec.mcause, exp->mcause);
    eg2_check(t, what, idx, eg2_trap_rec.mtval, exp->mtval);
  } else if (!trapped && !exp->trapped && exp->value_known) {
    eg2_check(t, what, idx, eg2_trap_rec.value, exp->value);
  }
  if (t->failed != failed && failed < kEg2MaxReports) {
    eg2_puts("  ");
    eg2_puts(op_name(op));
    eg2_puts(" at 0x");
    eg2_puthex(addr);
    eg2_puts(trapped ? " trapped mcause 0x" : " no trap (mcause 0x");
    eg2_puthex(eg2_trap_rec.mcause);
    eg2_puts(" mtval 0x");
    eg2_puthex(eg2_trap_rec.mtval);
    eg2_puts(trapped ? "\r\n" : ")\r\n");
  }
}

#ifdef __CHERIOT__

// ======================================= CHERIoT mode =======================================

enum { kAuthReg = 10 };  // the stubs' authority register, ca0

static eg2_cp_t data_cap(uint32_t base, uint32_t len) {
  return eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_mem(), base), len, kEg2BoundsInexact);
}

/**
 * Runs `op` through `auth` (its address is the access address), with `data` as CSC's capability
 * or `value` as an integer store's data, and checks it against the model. The shadow follows the
 * expected outcome.
 */
static void access(eg2_tally_t *t, const char *what, uint32_t idx, op_t op, eg2_cp_t auth,
                   eg2_cp_t data, uint32_t value, bool inject) {
  uint32_t addr = auth.m.addr;
  uint32_t w = op_width(op);
  bool store = op_store(op);
  eg2_access_t kind = op_cap(op) ? (store ? kEg2AccessStoreCap : kEg2AccessLoadCap)
                                 : (store ? kEg2AccessStore : kEg2AccessLoad);
  bool stored_tag = op == kOpCsc || op == kOpCCsc ? data.m.tag : false;
  int cause = eg2_cap_check_access(auth.m, addr, w, kind, stored_tag);
  outcome_t exp = {0};
  bool ok_to_run = true;
  if (cause > 0) {
    exp.trapped = true;
    exp.mcause = kEg2McauseCheri;
    exp.mtval = eg2_cap_mtval(kAuthReg, (uint32_t)cause);
  } else if (cause < 0) {
    exp.trapped = true;
    exp.mcause = store ? kEg2McauseStoreMisaligned : kEg2McauseLoadMisaligned;
    exp.mtval = addr;
  } else {
    exp = bus_outcome(op, addr, value, &ok_to_run);
  }
  if (!ok_to_run) {
    return;  // in bounds of something the test may not touch: not run
  }
  if (inject) {
    exp.trapped = !exp.trapped;
  }
  // The expected memory after the access, and CLC's expected result.
  eg2_cap_t clc_exp = eg2_cap_null(0);
  bool shadowed = !exp.trapped && mem_class(addr, w) == kMemShadowed;
  if (shadowed && op_cap(op)) {
    bool tagged = arena((uint32_t)arena_of(addr, w)).tagged;
    if (store) {
      eg2_cap_t s = eg2_cap_csc_stored(auth.m, data.m);
      shadow_write(addr, 4, s.addr);
      shadow_write(addr + 4, 4, s.hi);
      work[tag_off(addr)] = (s.tag && tagged) ? 1 : 0;
    } else {
      eg2_cap_t mem = {.addr = shadow_read(addr, 4),
                       .hi = shadow_read(addr + 4, 4),
                       .tag = work[tag_off(addr)] != 0 && tagged};
      clc_exp = eg2_cap_clc_result(auth.m, mem);
      exp.value_known = false;
    }
  } else if (shadowed && store) {
    shadow_write(addr, w, value);
  }
  void *op1 = (op == kOpCsc || op == kOpCCsc) ? data.hw : (void *)(uintptr_t)value;
  bool trapped = eg2_probe_call(auth.hw, op1, (void *)(uintptr_t)0xdeadbeefu, op_stub(op));
  compare(t, what, idx, op, addr, &exp, trapped);
  if (shadowed && op_cap(op) && !store && !trapped) {
    eg2_cp_t got = {.hw = eg2_trap_rec.cap.p, .m = clc_exp};
    eg2_cp_check(t, what, idx, got);
  }
  ++t->checks;
}

// ------------------------------ 1. The start-up's capabilities -------------------------------

static void phase_startup(eg2_tally_t *t) {
  uint32_t ss = eg2_sym_stack_start(), se = eg2_sym_stack_end();
  uint32_t ds = eg2_sym_data_start(), be = eg2_sym_bss_end();
  eg2_cp_t root = eg2_cp_root_mem();
  // CSP: [_stack_start, _stack_end) from the memory root; its address moves, so the address is
  // the model's starting point.
  eg2_cap_t sp_m = eg2_cap_set_bounds(eg2_cap_set_addr(root.m, ss), se - ss, kEg2BoundsInexact);
  void *sp;
  __asm__ volatile("cmove %0, csp" : "=C"(sp));
  eg2_cap_t sp_hw = eg2_hw_read(sp);
  eg2_cp_t csp = {.hw = sp, .m = eg2_cap_set_addr(sp_m, sp_hw.addr)};
  eg2_cp_check_full(t, "CSP", 0, csp);
  // CGP: [_data_start, _bss_end), its address in the middle.
  eg2_cap_t gp_m = eg2_cap_set_bounds(eg2_cap_set_addr(root.m, ds), be - ds, kEg2BoundsInexact);
  gp_m = eg2_cap_inc_addr(gp_m, (be - ds) / 2);
  void *gp;
  __asm__ volatile("cmove %0, cgp" : "=C"(gp));
  eg2_cp_t cgp = {.hw = gp, .m = gp_m};
  eg2_cp_check_full(t, "CGP", 0, cgp);
  ++t->done;
  // Loads only: the last word in bounds, the first word past, a misaligned word over the top.
  eg2_cp_t none = eg2_cp_root_mem();
  uint32_t idx = 0;
  for (uint32_t c = 0; c < 2; ++c) {
    eg2_cp_t cap = c == 0 ? csp : cgp;
    uint32_t top = (uint32_t)eg2_cap_top(cap.m);
    uint32_t addrs[4] = {top - 4, top, top - 2, top - 8};
    for (uint32_t k = 0; k < 4; ++k) {
      eg2_cp_t at = eg2_cp_set_addr(cap, addrs[k]);
      op_t op = k == 3 ? kOpClc : kOpLw;
      // The stack and the globals are not shadowed: only whether it traps is checked.
      uint32_t w = op_width(op);
      int cause = eg2_cap_check_access(at.m, addrs[k], w, op == kOpClc ? kEg2AccessLoadCap
                                                                         : kEg2AccessLoad, false);
      outcome_t exp = {0};
      if (cause > 0) {
        exp.trapped = true;
        exp.mcause = kEg2McauseCheri;
        exp.mtval = eg2_cap_mtval(kAuthReg, (uint32_t)cause);
      }
      bool trapped = eg2_probe_call(at.hw, none.hw, NULL, op_stub(op));
      compare(t, c == 0 ? "CSP edge" : "CGP edge", idx++, op, addrs[k], &exp, trapped);
    }
  }
  ++t->done;
}

// --------------------------------------- 2. Bounds edges -------------------------------------

typedef struct shape {
  uint32_t base, len;
  uint32_t addr;  // where the authority's address starts
} shape_t;

enum { kEdgeShapes = 16 };

static shape_t edge_shape(uint32_t s) {
  shape_t d;
  uint32_t a = kEg2ArenaA + 0x100 + 0x200 * s;
  switch (s) {
    case 0:
      d.base = a;
      d.len = 1;
      break;
    case 1:
      d.base = a + 1;  // odd base
      d.len = 7;
      break;
    case 2:
      d.base = a;
      d.len = 8;
      break;
    case 3:
      d.base = a + 4;
      d.len = 9;
      break;
    case 4:
      d.base = a + 2;
      d.len = 64;
      break;
    case 5:
      d.base = kEg2ArenaA + 0x1000;
      d.len = 511;  // the largest exact E 0 length
      break;
    case 6:
      d.base = kEg2ArenaB + 0x100;
      d.len = 1001;  // E 1, rounded up to 1002
      break;
    case 7:
      d.base = kEg2ArenaB + 0x800;
      d.len = 0x1000;  // E 4
      break;
    case 8:
      d.base = kEg2ArenaA + 0x40;
      d.len = 0;  // zero length
      break;
    case 9:
      d.base = kEg2ArenaA;  // sec SRAM's first byte
      d.len = 24;
      break;
    case 10:
      d.base = kEg2SecBase + kEg2SecSize - 24;  // ends at sec SRAM's last byte
      d.len = 24;
      break;
    case 11:
      d.base = kEg2RetArena + 0x100;  // retention SRAM: untagged
      d.len = 40;
      break;
    case 12:
      d.base = main_arena_base + 0x100;  // main SRAM
      d.len = 40;
      break;
    case 13:
      d.base = 0;  // based at 0: unmapped
      d.len = 16;
      break;
    case 14:
      d.base = 0xffffff00u;  // ends at 2^32: unmapped
      d.len = 0x100;
      break;
    default:
      d.base = 0;  // the memory root itself (bounds [0, 2^32))
      d.len = 0;
      break;
  }
  d.addr = d.base;
  return d;
}

static eg2_cp_t shape_cap(shape_t d, uint32_t s) {
  if (s == kEdgeShapes - 1) {
    return eg2_cp_root_mem();
  }
  return data_cap(d.base, d.len);
}

enum { kCursors = 9 };

static uint32_t edge_cursor(eg2_cap_t c, uint32_t k, uint32_t w, bool root) {
  uint32_t base = eg2_cap_base(c);
  uint32_t top = (uint32_t)eg2_cap_top(c);  // 2^32 is 0
  if (root) {
    // The address space's ends, and a word in arena A.
    static const uint32_t kRoot[kCursors] = {0, 1, 0xfffffffcu, 0xfffffffdu, 0xfffffffeu,
                                             0xffffffffu, kEg2ArenaA + 0x18, 0x0000fff8u,
                                             0x5000fffcu};
    return kRoot[k];
  }
  switch (k) {
    case 0:
      return base - 1;
    case 1:
      return base;
    case 2:
      return base + 1;
    case 3:
      return top - w - 1;
    case 4:
      return top - w;
    case 5:
      return top - w + 1;
    case 6:
      return top - 1;
    case 7:
      return top;
    default:
      return top + w;
  }
}

static void phase_edges(eg2_tally_t *t) {
  eg2_cp_t global = data_cap(kEg2ArenaB + 0x40, 32);  // what CSC stores
  uint32_t idx = 0;
  bool injected = false;
  for (uint32_t s = 0; s < kEdgeShapes; ++s) {
    if (s == 14 && !addr_hi()) {
      ++t->done;
      continue;
    }
    shape_t d = edge_shape(s);
    eg2_cp_t c = shape_cap(d, s);
    eg2_cp_check_full(t, "edge shape", s, c);
    for (uint32_t op = 0; op < kOpCount; ++op) {
      uint32_t w = op_width((op_t)op);
      for (uint32_t k = 0; k < kCursors; ++k) {
        uint32_t a = edge_cursor(c.m, k, w, s == kEdgeShapes - 1);
        eg2_cp_t at = eg2_cp_set_addr(c, a);
        bool inject = false;
#ifdef EG2_RAND_MEM_INJECT_FAIL
        inject = !injected && op == kOpLw && k == 7;  // lw at the top: must trap
#endif
        injected = injected || inject;
        access(t, "edge", idx++, (op_t)op, at, global, eg2_rand(), inject);
      }
    }
    ++t->done;
  }
}

// ---------------------------------------- 3. Permissions -------------------------------------

enum { kPermClasses = 11 };

static eg2_cp_t perm_auth(eg2_cp_t c, uint32_t k) {
  eg2_cp_t seal_root = eg2_cp_root_seal();
  switch (k) {
    case 0:
      return c;
    case 1:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermLd);
    case 2:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermSd);
    case 3:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermMc);
    case 4:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermSl);
    case 5:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermGl);
    case 6:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermLg);
    case 7:
      return eg2_cp_and_perm(c, ~(uint32_t)kEg2PermLm);
    case 8:
      return eg2_cp_and_perm(c, 0);
    case 9:
      return eg2_cp_seal(c, eg2_cp_set_addr(seal_root, 9));
    default:
      return eg2_cp_clear_tag(c);
  }
}

static void phase_perms(eg2_tally_t *t) {
  eg2_cp_t seal_root = eg2_cp_root_seal();
  // The capabilities CSC stores: global, local, sealed, untagged; CLC then reloads them.
  eg2_cp_t g = data_cap(kEg2ArenaB + 0x40, 32);
  eg2_cp_t vals[4] = {
      g,
      eg2_cp_and_perm(g, ~(uint32_t)kEg2PermGl),
      eg2_cp_seal(g, eg2_cp_set_addr(seal_root, 11)),
      eg2_cp_clear_tag(g),
  };
  uint32_t idx = 0;
  for (uint32_t k = 0; k < kPermClasses; ++k) {
    // Each class gets its own 64-byte slot of arena A.
    eg2_cp_t base = data_cap(kEg2ArenaA + 0x1800 + 0x40 * k, 0x40);
    eg2_cp_t auth = perm_auth(base, k);
    eg2_cp_check_full(t, "perm auth", k, auth);
    for (uint32_t v = 0; v < 4; ++v) {
      eg2_cp_t full = eg2_cp_set_addr(base, base.m.addr + 8 * v);
      // A capability stored through full authority, then every kind through the class.
      access(t, "perm prime", idx, kOpCsc, full, vals[v], 0, false);
      for (uint32_t op = 0; op < kOpCount; ++op) {
        eg2_cp_t at = eg2_cp_set_addr(auth, base.m.addr + 8 * v);
        access(t, "perm", idx++, (op_t)op, at, vals[v], eg2_rand(), false);
        // Re-store the capability, which an integer store clears.
        access(t, "perm prime", idx, kOpCsc, full, vals[v], 0, false);
      }
    }
    ++t->done;
  }
}

// --------------------------------------- 4. Device windows -----------------------------------

// A device access: the register is read, never written; past the top it must not reach the bus.
static void device_access(eg2_tally_t *t, uint32_t idx, op_t op, eg2_cp_t at, uint32_t reg) {
  uint32_t addr = at.m.addr;
  int cause = eg2_cap_check_access(at.m, addr, op_width(op), kEg2AccessLoad, false);
  outcome_t exp = {0};
  if (cause > 0) {
    exp.trapped = true;
    exp.mcause = kEg2McauseCheri;
    exp.mtval = eg2_cap_mtval(kAuthReg, (uint32_t)cause);
  } else if (addr != reg || op_store(op)) {
    return;  // in bounds but not the known register: not run
  }
  bool trapped = eg2_probe_call(at.hw, NULL, NULL, op_stub(op));
  compare(t, "device", idx, op, addr, &exp, trapped);
}

static void phase_devices(eg2_tally_t *t) {
  uint32_t idx = 0;
  for (uint32_t r = 0; r < kIntrRowCount; ++r) {
    uint32_t base = kWinBase[kIntrRows[r][0]];
    uint32_t size = kWinSize[kIntrRows[r][0]];
    uint32_t reg = base + kIntrRows[r][1];
    eg2_cp_t exact = eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_mem(), base), size,
                                   kEg2BoundsExact);
    eg2_cp_t tight = eg2_cp_bounds(eg2_cp_set_addr(eg2_cp_root_mem(), base), reg - base + 4,
                                   kEg2BoundsExact);
    eg2_cp_check(t, "device window", r, exact);
    uint32_t addrs[5] = {reg, reg + 4, reg + 1, base + size, base + size - 2};
    for (uint32_t k = 0; k < 5; ++k) {
      device_access(t, idx++, kOpLw, eg2_cp_set_addr(tight, addrs[k]), reg);
      device_access(t, idx++, kOpCLw, eg2_cp_set_addr(exact, addrs[k]), reg);
      device_access(t, idx++, kOpSw, eg2_cp_set_addr(exact, addrs[k]), reg);
    }
    ++t->done;
  }
}

// --------------------------------------- 5. Random accesses ----------------------------------

enum { kRegMain, kRegSec, kRegRram, kRegRet, kRegRom, kRegDevice, kRegUnmapped, kRegCount };
static const uint8_t kRegDefault[kRegCount] = {25, 15, 15, 10, 5, 20, 10};

enum {
  kShapeExact,   // plain: exact bounds over the target
  kShapeWide,    // plain: the whole region
  kShapeRoot,
  kShapeZero,
  kShapeOneByte,
  kShapeRounded,
  kShapeSealed,
  kShapeUntagged,
  kShapeReduced,
  kShapeCount,
};
enum { kShapePlain = 1u << kShapeExact | 1u << kShapeWide };

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

typedef struct rand_ctx {
  eg2_family_t region, width, shape, cursor;
  uint32_t store_over_cap, local_store;
} rand_ctx_t;

// A target address in the region, aligned for the width unless misaligned.
static uint32_t region_target(uint32_t region, uint32_t w, bool misaligned, uint32_t *base,
                              uint32_t *size) {
  switch (region) {
    case kRegMain:
      *base = main_arena_base;
      *size = kEg2MainArenaSize;
      break;
    case kRegSec:
      *base = eg2_rand_range(0, 1) ? kEg2ArenaA : kEg2ArenaB;
      *size = kEg2ArenaSize;
      break;
    case kRegRram:
      *base = kEg2RramBase;
      *size = kEg2RramSize;
      break;
    case kRegRet:
      *base = kEg2RetArena;
      *size = kEg2RetArenaSize;
      break;
    case kRegRom:
      *base = kEg2RomBase;
      *size = kEg2RomSize;
      break;
    case kRegUnmapped:
      *base = (addr_hi() && eg2_rand_range(0, 1)) ? 0xfffff000u : 0x1000u;
      *size = 0x1000;
      break;
    default: {  // a device register (INTR_STATE), read only
      uint32_t r = eg2_pick(kIntrRowCount);
      *base = kWinBase[kIntrRows[r][0]];
      *size = kWinSize[kIntrRows[r][0]];
      return *base + kIntrRows[r][1];
    }
  }
  uint32_t a = *base + (eg2_rand_range(0, *size - 16) & ~(w - 1));
  if (misaligned && w > 1) {
    a += eg2_rand_range(1, w - 1);
  }
  return a;
}

static op_t pick_op(uint32_t width_class, bool store) {
  switch (width_class) {
    case 0:
      return store ? kOpSb : (eg2_rand_range(0, 1) ? kOpLb : kOpLbu);
    case 1:
      return store ? kOpSh : (eg2_rand_range(0, 1) ? kOpLh : kOpLhu);
    case 2:
      return eg2_rand_range(0, 1) ? (store ? kOpSw : kOpLw) : (store ? kOpCSw : kOpCLw);
    default:
      return eg2_rand_range(0, 1) ? (store ? kOpCsc : kOpClc) : (store ? kOpCCsc : kOpCClc);
  }
}

static void rand_access(eg2_tally_t *t, rand_ctx_t *x, uint32_t n) {
  uint32_t region = eg2_family_pick(&x->region);
  op_t op = pick_op(eg2_family_pick(&x->width), eg2_rand_range(0, 1) != 0);
  if (region == kRegRram || region == kRegRom || region == kRegDevice) {
    // Read only.
    op = op == kOpSb ? kOpLbu
                     : (op == kOpSh ? kOpLhu
                                    : (op == kOpSw ? kOpLw
                                                   : (op == kOpCSw ? kOpCLw
                                                                   : (op == kOpCsc ? kOpClc
                                                                                   : (op == kOpCCsc
                                                                                          ? kOpCClc
                                                                                          : op)))));
  }
  if (region == kRegDevice) {
    op = kOpLw;
  }
  uint32_t w = op_width(op);
  bool misaligned = eg2_rand_range(0, 99) < 30;
  uint32_t rbase, rsize;
  uint32_t target = region_target(region, w, misaligned, &rbase, &rsize);
  // Integer stores over a stored capability: the target granule gets a capability first.
  if (op_store(op) && !op_cap(op) && arena_of(target & ~7u, 8) >= 0 &&
      eg2_rand_range(0, 99) < x->store_over_cap) {
    eg2_cp_t g = data_cap(kEg2ArenaB + 0x40, 32);
    access(t, "rand prime", n, kOpCsc, eg2_cp_set_addr(data_cap(target & ~7u, 8), target & ~7u),
           g, 0, false);
  }
  // The authority, by shape, then its cursor by class.
  eg2_cp_t c;
  uint32_t shape = eg2_family_pick(&x->shape);
  switch (shape) {
    case kShapeExact:
      c = data_cap(target & ~7u, 16);
      break;
    case kShapeWide:
      c = data_cap(rbase, rsize);
      break;
    case kShapeRoot:
      c = eg2_cp_root_mem();
      break;
    case kShapeZero:
      c = data_cap(target, 0);
      break;
    case kShapeOneByte:
      c = data_cap(target, 1);
      break;
    case kShapeRounded:
      c = data_cap(target & ~7u, 1001);
      break;
    case kShapeSealed:
      c = eg2_cp_seal(data_cap(target & ~7u, 16),
                      eg2_cp_set_addr(eg2_cp_root_seal(), eg2_rand_range(9, 15)));
      break;
    case kShapeUntagged:
      c = eg2_cp_clear_tag(data_cap(target & ~7u, 16));
      break;
    default:
      c = eg2_cp_and_perm(data_cap(target & ~7u, 16), eg2_rand() & kEg2PermAll);
      break;
  }
  uint32_t base = eg2_cap_base(c.m);
  uint32_t top = (uint32_t)eg2_cap_top(c.m);
  uint32_t a;
  switch (eg2_family_pick(&x->cursor)) {
    case kCurBelowBase:
      a = base - eg2_rand_range(1, w);
      break;
    case kCurAtBase:
      a = base;
      break;
    case kCurLastByte:
      a = top - 1;
      break;
    case kCurAtTop:
      a = top;
      break;
    case kCurPastTop:
      a = top - w + eg2_rand_range(1, w);
      break;
    case kCurAboveTop:
      a = top + eg2_rand_range(w, 0x100);
      break;
    case kCurZero:
      a = 0;
      break;
    case kCurAllOnes:
      a = 0xffffffffu;
      break;
    default:
      a = target;
      break;
  }
  // CSC's capability: global, or local in `local_store` % of them.
  eg2_cp_t data = data_cap(kEg2ArenaB + 0x40, 32);
  if (eg2_rand_range(0, 99) < x->local_store) {
    data = eg2_cp_and_perm(data, ~(uint32_t)kEg2PermGl);
  }
  access(t, "rand", n, op, eg2_cp_set_addr(c, a), data, eg2_rand(), false);
}

static void phase_random(eg2_tally_t *t) {
  rand_ctx_t x;
  uint32_t boost = EG2_PARAM(boost);
  uint32_t n = eg2_draw_count(EG2_PARAM(n_accesses), 256, 2048, 4096);
  x.region.n = kRegCount;
  x.region.plain_mask = 0;
  x.region.boosted = kRegCount;
  for (uint32_t r = 0; r < kRegCount; ++r) {
    x.region.w[r] = eg2_rand_range(kRegDefault[r] / 2u, kRegDefault[r] * 3u / 2u);
  }
  x.width.n = 4;
  x.width.plain_mask = 0;
  x.width.boosted = 4;
  x.width.w[0] = 20;
  x.width.w[1] = 20;
  x.width.w[2] = 30;
  x.width.w[3] = 30;
  eg2_family_draw(&x.shape, kShapeCount, kShapePlain, boost);
  eg2_family_draw(&x.cursor, kCurCount, kCurPlain, boost);
  x.store_over_cap = pct_knob(EG2_PARAM(store_over_cap), 30);
  x.local_store = pct_knob(EG2_PARAM(local_store), 20);
  eg2_family_print("region", &x.region);
  eg2_family_print("shape", &x.shape);
  eg2_family_print("cursor", &x.cursor);
  t->planned = n;
  for (uint32_t i = 0; i < n; ++i) {
    rand_access(t, &x, i);
    ++t->done;
  }
}

#else  // RISC-V mode

// ======================================== RISC-V mode =======================================

enum { kPmpEntry = 8 };  // the region's entry; entry 7 is its TOR base

static uint32_t read_mseccfg(void) {
  uint32_t v;
  __asm__ volatile("csrr %0, 0x747" : "=r"(v));
  return v;
}

// pmpcfg2 holds entries 8-11, pmpcfg1 entries 4-7.
static void pmp_set(uint32_t cfg8, uint32_t addr7, uint32_t addr8) {
  __asm__ volatile("csrw pmpaddr7, %0" : : "r"(addr7));
  __asm__ volatile("csrw pmpaddr8, %0" : : "r"(addr8));
  uint32_t c2;
  __asm__ volatile("csrr %0, pmpcfg2" : "=r"(c2));
  c2 = (c2 & ~0xffu) | cfg8;
  __asm__ volatile("csrw pmpcfg2, %0" : : "r"(c2));
  __asm__ volatile("fence" ::: "memory");
}

static void pmp_clear(void) { pmp_set(0, 0, 0); }

enum { kPmpR = 1, kPmpW = 2, kPmpTor = 1 << 3, kPmpNa4 = 2 << 3, kPmpNapot = 3 << 3, kPmpL = 0x80 };

// One access, from M mode or U mode, against the shadow and the region [lo, hi) with `perm`.
static void pmp_access(eg2_tally_t *t, const char *what, uint32_t idx, op_t op, uint32_t addr,
                       bool umode, bool enforced, uint32_t lo, uint32_t hi, uint32_t perm,
                       bool inject) {
  uint32_t w = op_width(op);
  bool store = op_store(op);
  bool ok_to_run = true;
  outcome_t exp = bus_outcome(op, addr, 0, &ok_to_run);
  if (!ok_to_run || exp.trapped) {
    return;
  }
  bool inside = addr >= lo && (uint64_t)addr + w <= hi;
  bool allowed = !enforced || !inside || (store ? (perm & kPmpW) != 0 : (perm & kPmpR) != 0);
  if (!allowed) {
    exp.trapped = true;
    exp.mcause = store ? kEg2McauseStoreFault : kEg2McauseLoadFault;
    exp.mtval = addr;
    exp.value_known = false;
  }
  if (inject) {
    exp.trapped = !exp.trapped;
  }
  uint32_t value = eg2_rand();
  if (store && !exp.trapped) {
    shadow_write(addr, w, value);
  }
  bool trapped;
  if (umode) {
    trapped = eg2_probe_call_umode((void *)(uintptr_t)addr, (void *)(uintptr_t)value,
                                   op_stub(op));
    // A U-mode access that did not fault ends in the trampoline's ecall from U.
    if (trapped && eg2_trap_rec.mcause == kEg2McauseEcallU) {
      trapped = false;
    }
  } else {
    trapped = eg2_probe_call((void *)(uintptr_t)addr, (void *)(uintptr_t)value, NULL, op_stub(op));
  }
  compare(t, what, idx, op, addr, &exp, trapped);
  ++t->checks;
}

typedef struct region_def {
  uint32_t mode, lo, size;
} region_def_t;

enum { kRegionDefs = 5 };

static region_def_t region_def(uint32_t r) {
  region_def_t d;
  uint32_t a = kEg2ArenaA + 0x1000;
  switch (r) {
    case 0:
      d.mode = kPmpTor;
      d.lo = a + 0x14;
      d.size = 0x2c;
      break;
    case 1:
      d.mode = kPmpNa4;
      d.lo = a + 0x104;
      d.size = 4;
      break;
    case 2:
      d.mode = kPmpNapot;
      d.lo = a + 0x208;
      d.size = 8;
      break;
    case 3:
      d.mode = kPmpNapot;
      d.lo = a + 0x340;
      d.size = 64;
      break;
    default:
      d.mode = kPmpNapot;
      d.lo = kEg2ArenaB;
      d.size = 0x1000;
      break;
  }
  return d;
}

static void phase_epmp(eg2_tally_t *t) {
  bool rlb = (read_mseccfg() & 4) != 0;  // mseccfg.RLB: locked entries can be undone
  eg2_put_kv("EG2 mseccfg", read_mseccfg());
  eg2_puts("\r\n");
  uint32_t idx = 0;
  bool injected = false;
  for (uint32_t r = 0; r < kRegionDefs; ++r) {
    region_def_t d = region_def(r);
    uint32_t lo = d.lo, hi = d.lo + d.size;
    uint32_t addr7 = lo >> 2, addr8;
    if (d.mode == kPmpTor) {
      addr8 = hi >> 2;
    } else if (d.mode == kPmpNa4) {
      addr8 = lo >> 2;
    } else {
      addr8 = (lo >> 2) | ((d.size >> 3) - 1);  // NAPOT: trailing ones encode the size
    }
    for (uint32_t p = 0; p < 3; ++p) {
      uint32_t perm = p == 0 ? 0 : (p == 1 ? kPmpR : (kPmpR | kPmpW));
      for (uint32_t l = 0; l < 2; ++l) {
        bool locked = l == 1;
        if (locked && !rlb) {
          continue;  // a locked entry could not be undone
        }
        pmp_set(d.mode | perm | (locked ? kPmpL : 0u), addr7, addr8);
        for (uint32_t u = 0; u < 2; ++u) {
          bool umode = u == 1;
          bool enforced = locked || umode;  // an unlocked entry does not apply to M mode
          for (uint32_t op = 0; op < kOpCount; ++op) {
            uint32_t w = op_width((op_t)op);
            uint32_t addrs[6] = {lo - w, lo - 1, lo, hi - w, hi - 1, hi};
            for (uint32_t k = 0; k < 6; ++k) {
              if (w > 1 && (k == 1 || k == 4)) {
                continue;  // only bytes straddle nothing there
              }
              bool inject = false;
#ifdef EG2_RAND_MEM_INJECT_FAIL
              inject = !injected && enforced && perm == 0 && k == 2 && !op_store((op_t)op);
#endif
              injected = injected || inject;
              pmp_access(t, "epmp", idx++, (op_t)op, addrs[k], umode, enforced, lo, hi, perm,
                         inject);
            }
          }
        }
        pmp_clear();
      }
    }
    ++t->done;
  }
}

static void phase_random(eg2_tally_t *t) {
  uint32_t n = eg2_draw_count(EG2_PARAM(n_accesses), 256, 2048, 4096);
  uint32_t u_pct = pct_knob(EG2_PARAM(u_mode_pct), 50);
  eg2_family_t region = {.n = 3, .plain_mask = 0, .boosted = 3};
  region.w[0] = 25;  // main SRAM
  region.w[1] = 15;  // sec SRAM
  region.w[2] = 10;  // retention SRAM
  t->planned = n;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t r = eg2_family_pick(&region);
    uint32_t base = r == 0 ? main_arena_base : (r == 1 ? kEg2ArenaA : kEg2RetArena);
    uint32_t size = r == 0 ? kEg2MainArenaSize : (r == 1 ? kEg2ArenaSize : kEg2RetArenaSize);
    op_t op = (op_t)eg2_pick(kOpCount);
    uint32_t w = op_width(op);
    uint32_t a = base + (eg2_rand_range(0, size - 8) & ~(w - 1));
    if (eg2_rand_range(0, 99) < 30 && w > 1) {
      a += eg2_rand_range(1, w - 1);
    }
    bool umode = eg2_rand_range(0, 99) < u_pct;
    pmp_access(t, "rand", i, op, a, umode, false, 0, 0, 0, false);
    ++t->done;
  }
  // Unmapped addresses: bus errors in both privilege modes.
  for (uint32_t i = 0; i < 16; ++i) {
    op_t op = (op_t)eg2_pick(kOpCount);
    uint32_t a = (i & 1) ? 0x1000u + 4 * i : 0xfffff000u + 4 * i;
    pmp_access(t, "unmapped", i, op, a, (i & 2) != 0, false, 0, 0, 0, false);
  }
}

#endif  // __CHERIOT__

// ---------------------------------------------------------------------------------------------

static void merge(eg2_tally_t *total, const char *name, const eg2_tally_t *t) {
  eg2_tally_print(name, t);
  total->checks += t->checks;
  total->failed += t->failed;
  total->planned += t->planned;
  total->done += t->done;
}

bool eg2_test_main(void) {
#ifdef __CHERIOT__
  eg2_cp_save_sealing_root();
#endif
  main_arena_base = eg2_main_arena();
  if (!eg2_sec_sram_init()) {
    eg2_puts("FAIL: no main SRAM arena\r\n");
    return false;
  }
  work = eg2_mem(kEg2WorkBase, kEg2WorkSize);
  arenas_fill();
  eg2_probe_begin();
  eg2_tally_t total = {0};
#ifdef __CHERIOT__
  eg2_tally_t t1 = {.planned = 2};
  phase_startup(&t1);
  merge(&total, "startup", &t1);
  eg2_tally_t t2 = {.planned = kEdgeShapes};
  phase_edges(&t2);
  merge(&total, "edges", &t2);
  eg2_tally_t t3 = {.planned = kPermClasses};
  phase_perms(&t3);
  merge(&total, "perms", &t3);
  eg2_tally_t t4 = {.planned = kIntrRowCount};
  phase_devices(&t4);
  merge(&total, "devices", &t4);
#else
  eg2_tally_t t1 = {.planned = kRegionDefs};
  phase_epmp(&t1);
  merge(&total, "epmp", &t1);
#endif
  eg2_tally_t t5 = {0};
  phase_random(&t5);
  merge(&total, "random", &t5);
  eg2_put_kv("EG2 traps taken", eg2_trap_rec.count);
  eg2_puts("\r\n");
  eg2_probe_end();
  eg2_tally_print("total", &total);
  return eg2_tally_ok(&total);
}
