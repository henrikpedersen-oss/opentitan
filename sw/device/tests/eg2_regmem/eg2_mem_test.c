// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_mem_test (EG2 verification spec, "Registers and memories" and "Registers and
 * memories in CHERIoT mode"; REQ_CIB_06, REQ_SRAM_01, REQ_RRAM_13, REQ_PBF_19): memory-test
 * algorithms run by the core over every memory it reaches, in either execution mode.
 *
 * The memories are the generated table's (eg2_regmem_tables.h): main SRAM (the part that holds
 * neither the image's globals nor the stack), secondary SRAM, retention SRAM, the CHERIoT
 * revocation bitmap at the base of the meta SRAM (in RISC-V mode only checked to fault, as its
 * window answers an error in ePMP mode; in CHERIoT mode only the part whose granules
 * hold no capability the test uses), OTBN's IMEM and DMEM, the usbdev packet buffer, and, read
 * only, the RRAM host window and the ROM.
 *
 * Draws: `n_mems` memories by the `w_mem_*` weights (each at most once), then per memory the
 * algorithm (`w_alg_*`: March C-, MATS+, checkerboard and its inverse, walking address bit,
 * address-in-address), the access width (`w_width_*`: word, half, byte, and in CHERIoT mode
 * capability; memories without byte writes take words), the range (`range_class`: the whole
 * testable part, its first 1 KiB, its last 1 KiB, a random window of `window_bytes`), the
 * direction of the ascending elements (`direction`) and the data background (from the seed).
 *
 * Checks per memory: every read against what the algorithm wrote; the first and last word and
 * byte answer; the address after the last faults when no other window holds it (in CHERIoT
 * mode also through the window's own capability, with a bounds fault); a store to a read-only
 * memory faults; reads of the read-only memories agree across widths; the ROM's digest equals
 * the expected digest (rom_ctrl). In CHERIoT mode with `tag_march`, on the tagged memories
 * (main and secondary SRAM): capabilities stored in the window load back tagged and equal, an
 * integer store into `int_over_cap` per cent of them clears their tag, the others keep it, and
 * the march's integer writes clear every tag.
 *
 * `model_gaps` skips what the EG2 SystemC model does not model (set by eg2sim runs only).
 *
 * Built with `EG2_REGMEM_INJECT_FAIL`, the first value compared is expected with one bit
 * flipped: the run must fail with `MEM FAIL`.
 */

#include "hw/top/rom_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem_tables.h"

EG2_TEST_CONFIG();

#define MEM_TEST_PARAMS(X)                    \
  X(n_mems, EG2_RM_MEM_COUNT, 1, EG2_RM_MEM_COUNT) \
  X(w_mem_main, 25, 0, 100)                   \
  X(w_mem_sec, 15, 0, 100)                    \
  X(w_mem_ret, 10, 0, 100)                    \
  X(w_mem_rram, 15, 0, 100)                   \
  X(w_mem_otbn, 10, 0, 100)                   \
  X(w_mem_buf, 10, 0, 100)                    \
  X(w_mem_meta, 5, 0, 100)                    \
  X(w_mem_rom, 10, 0, 100)                    \
  X(w_alg_march_c, 30, 0, 100)                \
  X(w_alg_mats, 20, 0, 100)                   \
  X(w_alg_checker, 20, 0, 100)                \
  X(w_alg_walk_addr, 30, 0, 100)              \
  X(w_alg_addr, 20, 0, 100)                   \
  X(w_width_word, 60, 0, 100)                 \
  X(w_width_half, 20, 0, 100)                 \
  X(w_width_byte, 20, 0, 100)                 \
  X(w_width_cap, 20, 0, 100)                  \
  X(range_class, 3, 0, 3)                     \
  X(window_bytes, 1024, 64, 65536)            \
  X(direction, 0, 0, 1)                       \
  X(tag_march, 1, 0, 1)                       \
  X(int_over_cap, 10, 0, 30)                  \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(MEM_TEST_PARAMS)

enum { kMemMain, kMemSec, kMemRet, kMemRram, kMemOtbn, kMemBuf, kMemMeta, kMemRom, kMemKinds };
enum { kAlgMarchC, kAlgMats, kAlgChecker, kAlgWalkAddr, kAlgAddr, kAlgCount };
enum { kW8 = 1, kW16 = 2, kW32 = 4, kW64 = 8 };
enum {
  kKiB = 1024,
  kGuard = 256,  // bytes kept clear above the globals and below the stack in main SRAM
  kMaxFailLines = 60,
  kMaxTagGranules = 128,
};

typedef struct ctx {
  bool gaps;
  bool injected;
  uint32_t checks;
  uint32_t fails;
  uint32_t lines;  // failure lines printed for the current memory
  uint32_t gap_skips;
  const char *mem;  // name of the memory under test
} ctx_t;

static bool check(ctx_t *c, const char *what, uint32_t addr, uint32_t got, uint32_t exp) {
#ifdef EG2_REGMEM_INJECT_FAIL
  if (!c->injected) {
    c->injected = true;
    exp ^= 1;
  }
#endif
  ++c->checks;
  if (got == exp) {
    return true;
  }
  if (c->fails++ < kMaxFailLines && c->lines++ < 8) {
    eg2_puts("MEM FAIL ");
    eg2_puts(c->mem);
    eg2_puts(" ");
    eg2_puts(what);
    eg2_rm_hex(" at ", addr);
    eg2_rm_hex(": got ", got);
    eg2_rm_hex(" expected ", exp);
    eg2_puts("\r\n");
  }
  return false;
}

static uint32_t kind_of(const eg2_rm_mem_t *m) {
  switch (m->base) {
    case TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR:
      return kMemMain;
    case TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR:
      return kMemSec;
    case TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR:
      return kMemRet;
    case TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR:
      return kMemRram;
    case TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR:
      return kMemRom;
    case TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR:
      return kMemMeta;
    default:
      break;
  }
  if (m->base - TOP_EARLGREY_OTBN_BASE_ADDR < TOP_EARLGREY_OTBN_SIZE_BYTES) {
    return kMemOtbn;
  }
  return kMemBuf;
}

// An unbounded pointer to a bus address: the memory root's in CHERIoT mode (so an access past a
// window reaches the bus and its error, not a bounds fault).
static volatile void *root_ptr(uint32_t addr) {
#ifdef __CHERIOT__
  void *root;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(root));
  return (volatile void *)__builtin_cheri_address_set(root, addr);
#else
  return (volatile void *)(uintptr_t)addr;
#endif
}

static uint32_t addr_of(const volatile void *p) {
#ifdef __CHERIOT__
  return (uint32_t)__builtin_cheri_address_get(p);
#else
  return (uint32_t)(uintptr_t)p;
#endif
}

// ---------------------------------------------------------------------------------------------
// Accesses of one width over a window. `base` is a pointer valid for the window.

static uint32_t mask_of(uint32_t width) {
  return width >= kW32 ? 0xffffffffu : (1u << (8 * width)) - 1;
}

static void mw(volatile uint8_t *base, uint32_t off, uint32_t width, uint32_t v, uint32_t hi) {
  switch (width) {
    case kW8:
      *(volatile uint8_t *)(base + off) = (uint8_t)v;
      break;
    case kW16:
      *(volatile uint16_t *)(base + off) = (uint16_t)v;
      break;
    case kW32:
      *(volatile uint32_t *)(base + off) = v;
      break;
    default: {
#ifdef __CHERIOT__
      // A capability store of the 64 bits {hi, v}: the bits go through a register as an untagged
      // capability (CLC from the stack, CSC to the window).
      union {
        void *cap;
        uint32_t w[2];
      } u;
      u.w[0] = v;
      u.w[1] = hi;
      *(void *volatile *)(base + off) = *(void *volatile *)&u.cap;
#else
      *(volatile uint32_t *)(base + off) = v;
      *(volatile uint32_t *)(base + off + 4) = hi;
#endif
      break;
    }
  }
}

static uint32_t mr(volatile uint8_t *base, uint32_t off, uint32_t width, uint32_t *hi) {
  switch (width) {
    case kW8:
      return *(volatile uint8_t *)(base + off);
    case kW16:
      return *(volatile uint16_t *)(base + off);
    case kW32:
      return *(volatile uint32_t *)(base + off);
    default: {
#ifdef __CHERIOT__
      union {
        void *cap;
        uint32_t w[2];
      } u;
      *(void *volatile *)&u.cap = *(void *volatile *)(base + off);
      *hi = ((volatile uint32_t *)u.w)[1];
      return ((volatile uint32_t *)u.w)[0];
#else
      *hi = *(volatile uint32_t *)(base + off + 4);
      return *(volatile uint32_t *)(base + off);
#endif
    }
  }
}

// The value an element writes at offset `off`: the data background or its inverse.
typedef struct bg {
  uint32_t lo, hi;
} bg_t;

static void put(volatile uint8_t *b, uint32_t off, uint32_t width, bg_t d, bool inv) {
  mw(b, off, width, inv ? ~d.lo : d.lo, inv ? ~d.hi : d.hi);
}

static void get(ctx_t *c, volatile uint8_t *b, uint32_t win_base, uint32_t off, uint32_t width,
                bg_t d, bool inv, const char *what) {
  uint32_t hi = 0;
  uint32_t got = mr(b, off, width, &hi);
  uint32_t m = mask_of(width);
  check(c, what, win_base + off, got & m, (inv ? ~d.lo : d.lo) & m);
  if (width == kW64) {
    check(c, what, win_base + off + 4, hi, inv ? ~d.hi : d.hi);
  }
}

// One march element over [0, len) in steps of width: optional read of r, then write of w.
// ascending, read_inv, write_inv; a negative value skips the read or write.
static void element(ctx_t *c, volatile uint8_t *b, uint32_t wbase, uint32_t len, uint32_t width,
                    bg_t d, bool up, int rd_inv, int wr_inv, const char *what) {
  uint32_t n = len / width;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t off = (up ? i : n - 1 - i) * width;
    if (rd_inv >= 0) {
      get(c, b, wbase, off, width, d, rd_inv != 0, what);
    }
    if (wr_inv >= 0) {
      put(b, off, width, d, wr_inv != 0);
    }
  }
}

static void alg_march_c(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len, uint32_t w,
                        bg_t d, bool up) {
  element(c, b, wb, len, w, d, up, -1, 0, "");
  element(c, b, wb, len, w, d, up, 0, 1, "March C- r0");
  element(c, b, wb, len, w, d, up, 1, 0, "March C- r1");
  element(c, b, wb, len, w, d, !up, 0, 1, "March C- r0 (down)");
  element(c, b, wb, len, w, d, !up, 1, 0, "March C- r1 (down)");
  element(c, b, wb, len, w, d, up, 0, -1, "March C- final r0");
}

static void alg_mats(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len, uint32_t w,
                     bg_t d, bool up) {
  element(c, b, wb, len, w, d, up, -1, 0, "");
  element(c, b, wb, len, w, d, up, 0, 1, "MATS+ r0");
  element(c, b, wb, len, w, d, !up, 1, 0, "MATS+ r1");
}

static void alg_checker(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len, uint32_t w,
                        bg_t d, bool up) {
  uint32_t n = len / w;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t k = up ? i : n - 1 - i;
      put(b, k * w, w, d, ((k ^ (uint32_t)pass) & 1) != 0);
    }
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t k = up ? i : n - 1 - i;
      get(c, b, wb, k * w, w, d, ((k ^ (uint32_t)pass) & 1) != 0,
          pass ? "checkerboard (inverse)" : "checkerboard");
    }
  }
}

// Address decoder: the window's first unit gets the background, the unit at each power-of-two
// offset its inverse; every one is then read. A stuck or shorted address bit makes two of them
// the same cell.
static void alg_walk_addr(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len, uint32_t w,
                          bg_t d, bool up) {
  (void)up;
  put(b, 0, w, d, false);
  for (uint32_t off = w; off + w <= len; off <<= 1) {
    put(b, off, w, d, true);
  }
  get(c, b, wb, 0, w, d, false, "walking address bit (base)");
  for (uint32_t off = w; off + w <= len; off <<= 1) {
    get(c, b, wb, off, w, d, true, "walking address bit");
  }
}

// Address in address: each unit holds its own address (mixed with the background), written in
// one direction and read in the other, then the inverse.
static void alg_addr(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len, uint32_t w,
                     bg_t d, bool up) {
  uint32_t n = len / w;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t k = up ? i : n - 1 - i;
      bg_t a = {(wb + k * w) ^ d.lo, (wb + k * w + 4) ^ d.hi};
      put(b, k * w, w, a, pass != 0);
    }
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t k = up ? n - 1 - i : i;
      bg_t a = {(wb + k * w) ^ d.lo, (wb + k * w + 4) ^ d.hi};
      get(c, b, wb, k * w, w, a, pass != 0, pass ? "address in address (inverse)"
                                                 : "address in address");
    }
  }
}

// ---------------------------------------------------------------------------------------------

// A probe that must or must not fault.
static void expect_access(ctx_t *c, const char *what, volatile void *p, uint32_t op,
                          uint32_t want_cause) {
  eg2_rm_out_t out;
  uint32_t cause = eg2_rm_probe(p, op, 0x5a5aa5a5u, &out);
  check(c, what, addr_of(p), cause, want_cause);
}

#ifdef __CHERIOT__
// Tags in the window: capabilities stored at sampled granules load back tagged and equal; an
// integer store clears the tag of `int_over_cap` per cent of them; the others keep it.
static void tag_check(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len) {
  uint32_t ngran = len / 8;
  uint32_t n = ngran < kMaxTagGranules ? ngran : kMaxTagGranules;
  uint32_t stride = ngran / n;
  void *root;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(root));
  uint8_t cleared[kMaxTagGranules];
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t off = i * stride * 8;
    // A capability to the granule itself: exact bounds of 8 bytes.
    void *cap = __builtin_cheri_bounds_set_exact(__builtin_cheri_address_set(root, wb + off), 8);
    *(void *volatile *)(b + off) = cap;
    void *got = *(void *volatile *)(b + off);
    check(c, "capability stored: tag", wb + off, __builtin_cheri_tag_get(got), 1);
    check(c, "capability stored: equal", wb + off, __builtin_cheri_equal_exact(got, cap), 1);
    cleared[i] = eg2_rand_range(0, 99) < EG2_PARAM(int_over_cap);
    if (cleared[i]) {
      // An integer store of any width anywhere in the granule clears its tag (REQ_CMS_10).
      uint32_t w = 1u << eg2_rand_range(0, 2);
      uint32_t at = eg2_rand_range(0, 8 / w - 1) * w;
      mw(b, off + at, w, eg2_rand(), 0);
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t off = i * stride * 8;
    void *got = *(void *volatile *)(b + off);
    check(c, cleared[i] ? "tag after an integer store" : "tag kept", wb + off,
          __builtin_cheri_tag_get(got), cleared[i] ? 0 : 1);
  }
}

static void tags_cleared(ctx_t *c, volatile uint8_t *b, uint32_t wb, uint32_t len) {
  uint32_t ngran = len / 8;
  uint32_t n = ngran < kMaxTagGranules ? ngran : kMaxTagGranules;
  uint32_t stride = ngran / n;
  for (uint32_t i = 0; i < n; ++i) {
    void *got = *(void *volatile *)(b + i * stride * 8);
    check(c, "tag after the march's integer writes", wb + i * stride * 8,
          __builtin_cheri_tag_get(got), 0);
  }
}
#endif

// The testable part of a memory: [lo, hi) bus addresses.
static void testable(const eg2_rm_mem_t *m, uint32_t kind, uint32_t *lo, uint32_t *hi) {
  *lo = m->base;
  *hi = m->base + m->size;
  if (kind == kMemMain) {
    // Neither the image's globals (at the base) nor the stack (at the top).
    uint32_t globals_end, stack_lo;
#ifdef __CHERIOT__
    void *gp;
    __asm__ volatile("cmove %0, cgp" : "=C"(gp));
    globals_end = (uint32_t)__builtin_cheri_top_get(gp);
    stack_lo = (uint32_t)__builtin_cheri_base_get(__builtin_frame_address(0));
#else
    extern char _bss_end[], _stack_start[];
    globals_end = (uint32_t)(uintptr_t)_bss_end;
    stack_lo = (uint32_t)(uintptr_t)_stack_start;
#endif
    *lo = (globals_end + (uint32_t)kGuard + (uint32_t)kKiB - 1u) & ~((uint32_t)kKiB - 1u);
    *hi = (stack_lo - (uint32_t)kGuard) & ~((uint32_t)kKiB - 1u);
  }
  if (kind == kMemRom) {
    // Not the top eight words: the expected digest, which reads with an integrity error (see the
    // boundaries in one_memory). 2026-10-26.
    *hi -= 32;
  }
#ifdef __CHERIOT__
  if (kind == kMemMeta) {
    // Revocation bits of main SRAM's free part only: one bitmap byte covers 64 bytes (eight
    // 8-byte granules). The globals, CGP's and the stack's own bases stay unrevoked.
    uint32_t mlo = 0, mhi = 0;
    for (uint32_t i = 0; i < EG2_RM_MEM_COUNT; ++i) {
      if (kind_of(&kEg2RmMems[i]) == kMemMain) {
        testable(&kEg2RmMems[i], kMemMain, &mlo, &mhi);
      }
    }
    *lo = m->base + (mlo - TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR) / 64;
    *hi = m->base + (mhi - TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR) / 64;
  }
#endif
}

static const char *alg_name(uint32_t a) {
  switch (a) {
    case kAlgMarchC:
      return "March C-";
    case kAlgMats:
      return "MATS+";
    case kAlgChecker:
      return "checkerboard";
    case kAlgWalkAddr:
      return "walking address bit";
    default:
      return "address in address";
  }
}

static void one_memory(ctx_t *c, const eg2_rm_mem_t *m) {
  uint32_t kind = kind_of(m);
  c->mem = &kEg2RmNames[m->name];
  c->lines = 0;
  uint32_t fails0 = c->fails, checks0 = c->checks;
  bool writable = (m->flags & kEg2RmMemWritable) != 0;
  uint32_t lo, hi;
  testable(m, kind, &lo, &hi);

  // The range.
  uint32_t len = hi - lo;
  uint32_t cls = EG2_PARAM(range_class);
  uint32_t wlo = lo, whi = hi;
  if (cls == 1 && len > kKiB) {
    whi = lo + kKiB;
  } else if (cls == 2 && len > kKiB) {
    wlo = hi - kKiB;
  } else if (cls == 3 && len > EG2_PARAM(window_bytes)) {
    uint32_t wl = EG2_PARAM(window_bytes) & ~7u;
    wlo = lo + (eg2_rand_range(0, (len - wl) / 8) * 8);
    whi = wlo + wl;
  }
  volatile uint8_t *win = (volatile uint8_t *)eg2_rm_ptr(m->base);
  volatile uint8_t *b = win + (wlo - m->base);

#ifndef __CHERIOT__
  if (kind == kMemMeta && !c->gaps) {
    // RISC-V (ePMP) mode: cheriot_mem_sys's theory of operation, the revocation bitmap window
    // "returns a TL-UL error while the system is in ePMP mode" (and "any meta SRAM access while in
    // ePMP mode": TL-UL error). So a load and a store fault, and no algorithm runs (2026-10-26:
    // the walking-address test took that fault as unexpected and ended the run).
    eg2_puts("MEM ");
    eg2_puts(c->mem);
    eg2_puts(": ePMP mode, every access answers an error\r\n");
    expect_access(c, "bitmap load in ePMP mode", b, kEg2RmLw, kEg2RmCauseLoadFault);
    expect_access(c, "bitmap store in ePMP mode", b, kEg2RmSw, kEg2RmCauseStoreFault);
    eg2_puts(c->fails == fails0 ? "MEM PASS " : "MEM FAIL ");
    eg2_puts(c->mem);
    eg2_puts(": ");
    eg2_putdec(c->checks - checks0);
    eg2_puts(" checks, ");
    eg2_putdec(c->fails - fails0);
    eg2_puts(" failures\r\n");
    return;
  }
#endif

  // The algorithm, width and data background.
  uint32_t wa[kAlgCount] = {EG2_PARAM(w_alg_march_c), EG2_PARAM(w_alg_mats),
                            EG2_PARAM(w_alg_checker), EG2_PARAM(w_alg_walk_addr),
                            EG2_PARAM(w_alg_addr)};
  uint32_t alg = eg2_rm_pick(wa, kAlgCount);
  if (alg >= kAlgCount) {
    alg = kAlgMarchC;
  }
  uint32_t ww[4] = {EG2_PARAM(w_width_word), EG2_PARAM(w_width_half),
                    EG2_PARAM(w_width_byte), 0};
#ifdef __CHERIOT__
  if (m->flags & kEg2RmMemTagged) {
    ww[3] = EG2_PARAM(w_width_cap);
  }
#endif
  static const uint8_t kWidths[4] = {kW32, kW16, kW8, kW64};
  uint32_t wi = eg2_rm_pick(ww, 4);
  uint32_t width = wi < 4 ? kWidths[wi] : kW32;
  if (!(m->flags & kEg2RmMemByteWrite) && width < kW32) {
    width = kW32;  // no sub-word writes into this memory
  }
  bg_t d = {eg2_rand(), eg2_rand()};
  bool up = EG2_PARAM(direction) == 0;

  eg2_puts("MEM ");
  eg2_puts(c->mem);
  eg2_rm_hex(": [", wlo);
  eg2_rm_hex(", ", whi);
  eg2_puts(writable ? ") " : ") read only, ");
  eg2_puts(writable ? alg_name(alg) : "width consistency");
  eg2_puts(", ");
  eg2_putdec(width * 8);
  eg2_puts("-bit\r\n");

  if (writable && whi > wlo) {
#ifdef __CHERIOT__
    bool tags = EG2_PARAM(tag_march) && (m->flags & kEg2RmMemTagged);
    if (tags) {
      tag_check(c, b, wlo, whi - wlo);
    }
#endif
    switch (alg) {
      case kAlgMarchC:
        alg_march_c(c, b, wlo, whi - wlo, width, d, up);
        break;
      case kAlgMats:
        alg_mats(c, b, wlo, whi - wlo, width, d, up);
        break;
      case kAlgChecker:
        alg_checker(c, b, wlo, whi - wlo, width, d, up);
        break;
      case kAlgWalkAddr:
        alg_walk_addr(c, b, wlo, whi - wlo, width, d, up);
        break;
      default:
        alg_addr(c, b, wlo, whi - wlo, width, d, up);
        break;
    }
#ifdef __CHERIOT__
    // Every algorithm but the walking address bit writes every unit of the window.
    if (tags && width != kW64 && alg != kAlgWalkAddr) {
      tags_cleared(c, b, wlo, whi - wlo);
    }
#endif
    if (kind == kMemMeta) {
      // The revocation bitmap back to nothing revoked.
      for (uint32_t off = 0; off < whi - wlo; off += 4) {
        mw(b, off, kW32, 0, 0);
      }
    }
  } else if (!writable) {
    // Read only: every byte and half-word read agrees with the word read.
    uint32_t rlo = wlo, rhi = whi;
    if (kind == kMemRram) {
      // Inside the test's own image (its register table), never unprogrammed RRAM.
      rlo = addr_of(&kEg2RmRegs[0]);
      rhi = rlo + sizeof(kEg2RmRegs);
      if (rhi - rlo > EG2_PARAM(window_bytes)) {
        rlo += (eg2_rand_range(0, (rhi - rlo - EG2_PARAM(window_bytes)) / 4)) * 4;
        rhi = rlo + EG2_PARAM(window_bytes);
      }
    }
    volatile uint8_t *rb = win + (rlo - m->base);
    for (uint32_t off = 0; off + 4 <= rhi - rlo; off += 4) {
      uint32_t word = *(volatile uint32_t *)(rb + off);
      uint32_t half = *(volatile uint16_t *)(rb + off) |
                      ((uint32_t) * (volatile uint16_t *)(rb + off + 2) << 16);
      uint32_t bytes = 0;
      for (uint32_t k = 0; k < 4; ++k) {
        bytes |= (uint32_t) * (volatile uint8_t *)(rb + off + k) << (8 * k);
      }
      check(c, "half-word reads against the word", rlo + off, half, word);
      check(c, "byte reads against the word", rlo + off, bytes, word);
    }
    // A store faults (read-only memory). The model takes stores into the ROM and the RRAM host
    // window as into RAM.
    uint32_t spot = kind == kMemRram ? m->base + m->size - 0x10000 : m->base + m->size - 0x1000;
    if (c->gaps) {
      ++c->gap_skips;
    } else {
      expect_access(c, "store into read-only memory", win + (spot - m->base), kEg2RmSw,
                    kEg2RmCauseStoreFault);
    }
  }

  // Boundaries: first and last word and byte answer; the address after the last faults.
  // The ROM's last word is below its top eight words, the expected digest, which are stored so
  // that they unscramble with invalid ECC (rom_ctrl theory_of_operation: the checker reads them
  // "ignoring ECC bits"; scramble_image.py add_hash): a bus read of them is an integrity error.
  // 2026-10-26 (regr-vlt: "rom_ctrl.rom last word at 0x0006fffc: got 0xffffffe0").
  uint32_t last = kind == kMemRom ? m->size - 32 - 4 : m->size - 4;
  if (writable && kind != kMemMain && kind != kMemMeta) {
    // A word nothing wrote since reset is not known to hold valid integrity: the packet buffer
    // has no initialisation (a word the march did not cover reads as X in simulation, u_usbdev
    // rspfifo DataKnown_A, regr-xlm 2026-10-25), and neither have sec and retention SRAM (INIT is
    // software's, sram_ctrl programmers_guide "Initialization"; the test ROM runs none for them)
    // nor OTBN's IMEM and DMEM (its reset wipe is of internal state: otbn theory_of_operation).
    // A read of one is a load integrity error (Ibex internal interrupt, mcause 0xFFFFFFE0;
    // regr-vlt 2026-10-26, CHERIoT run: first word of otbn.DMEM/IMEM, sec and ret SRAM). Write
    // the boundary words first; only the access is checked here. Not main SRAM (globals at its
    // base, the stack at its top; the test ROM initialises it) nor the bitmap window (revokes).
    // 2026-10-26.
    expect_access(c, "first word written", win, kEg2RmSw, 0);
    expect_access(c, "last word written", win + last, kEg2RmSw, 0);
  }
  expect_access(c, "first word", win, kEg2RmLw, 0);
  expect_access(c, "first byte", win, kEg2RmLbu, 0);
  if (kind != kMemRram) {
    // (RRAM's last words are the OTP pages, which the host window does not reach.)
    expect_access(c, "last word", win + last, kEg2RmLw, 0);
    expect_access(c, "last byte", win + last + 3, kEg2RmLbu, 0);
  }
  if (!(m->flags & kEg2RmMemNextMapped)) {
    if (c->gaps && kind == kMemMeta) {
      ++c->gap_skips;  // the model maps the whole meta SRAM behind the bitmap window
    } else {
      expect_access(c, "load after the last address", root_ptr(m->base + m->size), kEg2RmLw,
                    kEg2RmCauseLoadFault);
    }
#ifdef __CHERIOT__
    // Through the window's own capability, the bounds stop it first.
    expect_access(c, "load past the window's capability", win + m->size, kEg2RmLw,
                  kEg2RmCauseCheri);
#endif
  }

  eg2_puts(c->fails == fails0 ? "MEM PASS " : "MEM FAIL ");
  eg2_puts(c->mem);
  eg2_puts(": ");
  eg2_putdec(c->checks - checks0);
  eg2_puts(" checks, ");
  eg2_putdec(c->fails - fails0);
  eg2_puts(" failures\r\n");
}

// rom_ctrl: the digest it computed over the ROM equals the expected digest.
static void rom_digest(ctx_t *c) {
  c->mem = "rom_ctrl.regs";
  c->lines = 0;
  for (uint32_t i = 0; i < ROM_CTRL_DIGEST_MULTIREG_COUNT; ++i) {
    uint32_t got = eg2_rm_rd32(TOP_EARLGREY_ROM_CTRL_REGS_BASE_ADDR +
                               ROM_CTRL_DIGEST_0_REG_OFFSET + 4 * i);
    uint32_t exp = eg2_rm_rd32(TOP_EARLGREY_ROM_CTRL_REGS_BASE_ADDR +
                               ROM_CTRL_EXP_DIGEST_0_REG_OFFSET + 4 * i);
    check(c, "ROM digest against the expected digest", ROM_CTRL_DIGEST_0_REG_OFFSET + 4 * i,
          got, exp);
  }
}

bool eg2_test_main(void) {
  eg2_rm_irq_off();
  if (!eg2_rm_params_begin()) {
    return false;
  }
  ctx_t c = {0};
  c.gaps = EG2_PARAM(model_gaps) != 0;

  uint32_t wk[kMemKinds] = {EG2_PARAM(w_mem_main), EG2_PARAM(w_mem_sec), EG2_PARAM(w_mem_ret),
                            EG2_PARAM(w_mem_rram), EG2_PARAM(w_mem_otbn), EG2_PARAM(w_mem_buf),
                            EG2_PARAM(w_mem_meta), EG2_PARAM(w_mem_rom)};
  // The memories: drawn by weight, each at most once.
  uint32_t w[EG2_RM_MEM_COUNT];
  for (uint32_t i = 0; i < EG2_RM_MEM_COUNT; ++i) {
    w[i] = wk[kind_of(&kEg2RmMems[i])];
  }
  uint32_t done = 0;
  for (uint32_t k = 0; k < EG2_PARAM(n_mems); ++k) {
    uint32_t i = eg2_rm_pick(w, EG2_RM_MEM_COUNT);
    if (i >= EG2_RM_MEM_COUNT) {
      break;
    }
    w[i] = 0;
    if (kind_of(&kEg2RmMems[i]) == kMemRom) {
      rom_digest(&c);
    }
    one_memory(&c, &kEg2RmMems[i]);
    ++done;
  }

  eg2_puts("MEM summary: ");
  eg2_putdec(done);
  eg2_puts(" memories, ");
  eg2_putdec(c.checks);
  eg2_puts(" checks, ");
  eg2_putdec(c.fails);
  eg2_puts(" failures; model-gap skips ");
  eg2_putdec(c.gap_skips);
  eg2_puts("\r\n");
  return c.fails == 0 && c.checks > 0;
}
