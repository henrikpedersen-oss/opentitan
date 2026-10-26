// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_tl_errors (EG2 verification spec REQ_TL_03; "TileLink in RISC-V mode" and, for
 * `_cheriot`, "Twins of the RISC-V tests"): every explicit TL-UL error case, made by the core,
 * is answered with d_error, which the core takes as a load or store access fault (mcause 5 / 7,
 * mtval the address), and has no side effect. The cases and where each expectation comes from:
 *
 * | Case | Access | Expected (document) |
 * |---|---|---|
 * | 0 gap | a load or store at the first address after a | fault: "accesses to non-existent |
 * | |   device window that no other window holds |   addresses" (tlul README |
 * | |   (the generated window table) |   "Explicit Error Cases" 5) |
 * | 1 in_window | a word access at an offset of a register | fault: reggen README "Error |
 * | |   window past its last register |   responses" (unmapped address) |
 * | 2 subword_write | a byte or half store to a 32-bit register | fault, register unchanged: |
 * | | |   reggen "writes of size smaller |
 * | | |   than register size" |
 * | 3 misaligned | a half store at byte 1 of a 32-bit register | fault, unchanged: reggen |
 * | |   (inside one word: one bus access, Ibex LSU) |   "not word-aligned" writes |
 * | 4 subword_read | a byte or half load at any offset of a | no fault, the bytes of the word: |
 * | |   register (positive case) |   reggen "Reads of size smaller |
 * | | |   than full word ... no error" |
 * | 5 rom_write | a store to the ROM | fault, word unchanged (spec |
 * | | |   REQ_TL_03 table "a write to |
 * | | |   read-only memory") |
 * | 6 rram_write | a store to the RRAM host window | fault, unchanged (RFC RRAM §3.1 |
 * | | |   "host_tl is read-only") |
 * | 7 byte_reg | a byte store of its own value to a register | no fault, unchanged: errors only |
 * | |   whose fields fit in one byte |   where the IP says so (positive)|
 *
 * In RISC-V mode the test first checks that the ePMP lets every access through (the test ROM's
 * entry 15, locked RWX over the whole address space), so a fault is the bus's. In CHERIoT mode
 * every access goes through a capability derived from the memory root with the drawn `authority`
 * (0 exact bounds on the access, 1 a 4 KiB capability around it, 2 the device window's
 * capability from the device table where there is one), so the access reaches the bus, and each
 * case 0, 1, 5 and 6 also makes a capability access: CLC for a load case, CSC of a tagged
 * capability for a store case. It must fault the same way, and a CLC from the ROM or RRAM word
 * afterwards returns tag 0 and the old data (no tag written; REQ_CMS_17: a CSC to RRAM whose
 * capability does not match the stored bits is a bus error).
 *
 * Knobs:
 *   n_cases          error cases to make
 *   w_case_<c>       case weights (the eight cases above)
 *   w_width_<w>      access width weights: byte, half, word (where the case allows a choice)
 *   order            0: the cases in turn, 1: each drawn by weight
 *   authority        CHERIoT mode: 0 exact, 1 4 KiB, 2 device window, 3 drawn per case
 *   model_gaps       skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1           | Why                                                  |
 * |---------------------------------------|------------------------------------------------------|
 * | cases 2 and 3 (sub-word and           | eg2sim's register blocks apply sub-word writes to    |
 * |   misaligned register writes)         |   the bytes written (eg2_regblock.h); model finding  |
 * | cases 5 and 6 (ROM and RRAM writes)   | eg2sim's ROM and RRAM host windows are plain DMI     |
 * |                                       |   memories: a store lands (model finding)            |
 *
 * Built with EG2_TL_ERRORS_INJECT_FAIL the first value check expects bit 0 flipped: the run must
 * fail with `TL FAIL`.
 */

#include "hw/top/rv_timer_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define TL_ERRORS_PARAMS(X)        \
  X(n_cases, 48, 1, 512)           \
  X(w_case_gap, 20, 0, 100)        \
  X(w_case_in_window, 15, 0, 100)  \
  X(w_case_subword_write, 15, 0, 100) \
  X(w_case_misaligned, 10, 0, 100) \
  X(w_case_subword_read, 10, 0, 100) \
  X(w_case_rom_write, 10, 0, 100)  \
  X(w_case_rram_write, 10, 0, 100) \
  X(w_case_byte_reg, 10, 0, 100)   \
  X(w_width_byte, 30, 0, 100)      \
  X(w_width_half, 30, 0, 100)      \
  X(w_width_word, 40, 0, 100)      \
  X(order, 1, 0, 1)                \
  X(authority, 3, 0, 3)            \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(TL_ERRORS_PARAMS)

enum {
  kCases = 8,
  kCaseGap = 0,
  kCaseInWindow = 1,
  kCaseSubwordWrite = 2,
  kCaseMisaligned = 3,
  kCaseSubwordRead = 4,
  kCaseRomWrite = 5,
  kCaseRramWrite = 6,
  kCaseByteReg = 7,
  // The RRAM scratch pages (eg2_rram.h: pages 3584..4089 of 512 bytes).
  kRramScratchBase = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR + 3584 * 512,
  kRramScratchBytes = (4090 - 3584) * 512,
};

// The device windows, from the generated table: integers only (no pointers in .data).
#define WIN_BASE_(name, base, size) (base),
#define WIN_SIZE_(name, base, size) (size),
static const uint32_t kWinBase[] = {EG2_WINDOW_TABLE(WIN_BASE_)};
static const uint32_t kWinSize[] = {EG2_WINDOW_TABLE(WIN_SIZE_)};
enum { kWins = sizeof(kWinBase) / sizeof(kWinBase[0]) };

// 32-bit registers with no side effect and fields above bit 15 (cases 2-4).
static const uint32_t kWideReg[] = {
    TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR + 0x2a4,  // rram_ctrl SCRATCH
    TOP_EARLGREY_SPI_HOST0_BASE_ADDR + 0x18,        // spi_host0 CONFIGOPTS
    TOP_EARLGREY_I2C0_BASE_ADDR + 0x3c,             // i2c0 TIMING0
    TOP_EARLGREY_RV_TIMER_BASE_ADDR + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET,
};
enum { kWideRegs = sizeof(kWideReg) / sizeof(kWideReg[0]) };

// Register windows and the first offset past their last register (generated headers; case 1).
static const uint32_t kHoleBase[] = {
    TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR, TOP_EARLGREY_UART1_BASE_ADDR,
    TOP_EARLGREY_SPI_HOST1_BASE_ADDR,      TOP_EARLGREY_AON_TIMER_BASE_ADDR,
    TOP_EARLGREY_RV_TIMER_BASE_ADDR,       TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR,
    TOP_EARLGREY_PWRMGR_BASE_ADDR,         TOP_EARLGREY_CLKMGR_BASE_ADDR,
    TOP_EARLGREY_SYSRST_CTRL_BASE_ADDR,
};
static const uint32_t kHoleFirst[] = {0x2bc, 0x34, 0x38, 0x38, 0x120, 0x24, 0x44, 0x50, 0xac};
static const uint32_t kHoleSize[] = {
    TOP_EARLGREY_RRAM_CTRL_CORE_SIZE_BYTES,      TOP_EARLGREY_UART1_SIZE_BYTES,
    TOP_EARLGREY_SPI_HOST1_SIZE_BYTES,           TOP_EARLGREY_AON_TIMER_SIZE_BYTES,
    TOP_EARLGREY_RV_TIMER_SIZE_BYTES,            TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_SIZE_BYTES,
    TOP_EARLGREY_PWRMGR_SIZE_BYTES,              TOP_EARLGREY_CLKMGR_SIZE_BYTES,
    TOP_EARLGREY_SYSRST_CTRL_SIZE_BYTES,
};
enum { kHoles = sizeof(kHoleBase) / sizeof(kHoleBase[0]) };

static uint32_t counts_[kCases];

static bool in_any_window(uint32_t addr) {
  for (uint32_t i = 0; i < kWins; ++i) {
    if (addr >= kWinBase[i] && addr - kWinBase[i] < kWinSize[i]) {
      return true;
    }
  }
  return false;
}

/** The pointer for an access of `len` bytes at `addr` (the authority in CHERIoT mode). */
static volatile uint8_t *tl_ptr(uint32_t addr, uint32_t len) {
#ifdef __CHERIOT__
  uint32_t a = EG2_PARAM(authority);
  if (a == 3) {
    a = eg2_rand_range(0, 2);
  }
  // The ROM window (0x30000 bytes) is not a power of two: its exact capability may not be
  // representable, so the ROM always gets a derived one.
  bool rom = addr - TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR < TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES;
  if (a == 2 && !rom && in_any_window(addr)) {
    return (volatile uint8_t *)eg2_chk_ptr(addr);
  }
  void *c;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(c));
  if (a == 0) {
    c = __builtin_cheri_address_set(c, addr);
    c = __builtin_cheri_bounds_set_exact(c, len);
  } else {
    c = __builtin_cheri_address_set(c, addr & ~0xfffu);
    c = __builtin_cheri_bounds_set_exact(c, 0x1000);
    c = __builtin_cheri_address_set(c, addr);
  }
  return (volatile uint8_t *)c;
#else
  (void)len;
  return (volatile uint8_t *)(uintptr_t)addr;
#endif
}

#ifdef __CHERIOT__
typedef struct eg2_tl_cap_out {
  void *cap;
  uint32_t mcause;
  uint32_t mtval;
} eg2_tl_cap_out_t;
uint32_t eg2_tl_cap_access(volatile void *auth, uint32_t op, void *wcap, eg2_tl_cap_out_t *out);

/** The capability access of an error case: CLC or CSC at the 8-byte granule of `addr`. */
static bool cap_case(uint32_t addr, bool store, uint32_t exp_cause) {
  uint32_t granule = addr & ~7u;
  void *wcap;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(wcap));
  wcap = __builtin_cheri_bounds_set(__builtin_cheri_address_set(wcap, 0x10000000), 64);
  eg2_tl_cap_out_t o;
  uint32_t cause = eg2_tl_cap_access(tl_ptr(granule, 8), store ? 1 : 0, wcap, &o);
  bool ok = eg2_chk_eq(store ? "csc mcause" : "clc mcause", exp_cause, cause);
  ok = eg2_chk_eq("cap access mtval", granule, o.mtval) && ok;
  return ok;
}

/** After a refused CSC: a CLC of the granule returns tag 0 and the data it held before. */
static bool cap_untagged(uint32_t addr, uint32_t lo_before) {
  uint32_t granule = addr & ~7u;
  eg2_tl_cap_out_t o;
  uint32_t cause = eg2_tl_cap_access(tl_ptr(granule, 8), 0, NULL, &o);
  bool ok = eg2_chk_eq("clc after the refused csc: mcause", 0, cause);
  ok = eg2_chk_eq("clc after the refused csc: tag", 0, __builtin_cheri_tag_get(o.cap)) && ok;
  ok = eg2_chk_eq("clc after the refused csc: low word",
                  lo_before, (uint32_t)__builtin_cheri_address_get(o.cap)) && ok;
  return ok;
}
#endif

static uint32_t draw_width(void) {
  const uint32_t w[3] = {EG2_PARAM(w_width_byte), EG2_PARAM(w_width_half),
                         EG2_PARAM(w_width_word)};
  uint32_t k = eg2_chk_pick(w, 3);
  return k >= 3 ? 2 : k;  // 0 byte, 1 half, 2 word
}

static uint32_t load_op(uint32_t width) {
  return width == 0 ? kEg2ChkLbu : width == 1 ? kEg2ChkLhu : kEg2ChkLw;
}

static uint32_t store_op(uint32_t width) {
  return width == 0 ? kEg2ChkSb : width == 1 ? kEg2ChkSh : kEg2ChkSw;
}

/** One access that must fault with `exp_cause` at `addr`. */
static bool expect_fault(const char *what, uint32_t addr, uint32_t op, uint32_t exp_cause) {
  uint32_t width = (op == kEg2ChkSb || op == kEg2ChkLb || op == kEg2ChkLbu) ? 1
                   : (op == kEg2ChkSh || op == kEg2ChkLh || op == kEg2ChkLhu) ? 2
                                                                             : 4;
  eg2_chk_trap_t t;
  uint32_t cause = eg2_chk_access(tl_ptr(addr, width), op, eg2_rand(), &t);
  eg2_chk_step(what, addr);
  bool ok = eg2_chk_eq("mcause", exp_cause, cause);
  ok = eg2_chk_eq("mtval", addr, t.mtval) && ok;
  return ok;
}

static bool case_gap(void) {
  // A window whose first address after it is in no window.
  for (uint32_t tries = 0; tries < 4 * kWins; ++tries) {
    uint32_t i = eg2_rand_range(0, kWins - 1);
    uint32_t addr = kWinBase[i] + kWinSize[i];
    if (addr < kWinBase[i] || in_any_window(addr)) {
      continue;
    }
    uint32_t width = draw_width();
    bool store = eg2_rand_range(0, 1) == 1;
    bool ok = expect_fault("gap addr", addr, store ? store_op(width) : load_op(width),
                           store ? kEg2ChkCauseStoreFault : kEg2ChkCauseLoadFault);
#ifdef __CHERIOT__
    ok = cap_case(addr, store, store ? kEg2ChkCauseStoreFault : kEg2ChkCauseLoadFault) && ok;
#endif
    return ok;
  }
  return eg2_chk_true("a window with an unmapped address after it", false);
}

static bool case_in_window(void) {
  uint32_t i = eg2_rand_range(0, kHoles - 1);
  uint32_t off = kHoleFirst[i] + 4 * eg2_rand_range(0, (kHoleSize[i] - kHoleFirst[i]) / 4 - 1);
  uint32_t addr = kHoleBase[i] + off;
  bool store = eg2_rand_range(0, 1) == 1;
  bool ok = expect_fault("in-window hole addr", addr, store ? kEg2ChkSw : kEg2ChkLw,
                         store ? kEg2ChkCauseStoreFault : kEg2ChkCauseLoadFault);
#ifdef __CHERIOT__
  // The capability access needs a whole 8-byte granule inside the hole.
  uint32_t granule = (kHoleBase[i] + kHoleFirst[i] + 7) & ~7u;
  if (granule + 8 <= kHoleBase[i] + kHoleSize[i]) {
    ok = cap_case(granule, store, store ? kEg2ChkCauseStoreFault : kEg2ChkCauseLoadFault) && ok;
  }
#endif
  return ok;
}

/** Cases 2 and 3: a refused write to a 32-bit register, which keeps its value. */
static bool case_reg_write(bool misaligned) {
  uint32_t reg = kWideReg[eg2_rand_range(0, kWideRegs - 1)];
  uint32_t before = eg2_chk_rd32(reg);
  uint32_t addr;
  uint32_t op;
  if (misaligned) {
    addr = reg + 1;
    op = kEg2ChkSh;
  } else if (eg2_rand_range(0, 1) == 0) {
    addr = reg + eg2_rand_range(0, 3);
    op = kEg2ChkSb;
  } else {
    addr = reg + 2 * eg2_rand_range(0, 1);
    op = kEg2ChkSh;
  }
  bool ok = expect_fault(misaligned ? "misaligned half store addr" : "sub-word store addr", addr,
                         op, kEg2ChkCauseStoreFault);
  uint32_t after = eg2_chk_rd32(reg);
  ok = eg2_chk_eq("register after the refused write", before, after) && ok;
  if (after != before) {
    eg2_chk_wr32(reg, before);
  }
  return ok;
}

static bool case_subword_read(void) {
  uint32_t reg = kWideReg[eg2_rand_range(0, kWideRegs - 1)];
  uint32_t word = eg2_chk_rd32(reg);
  bool half = eg2_rand_range(0, 1) == 1;
  // Any byte offset, including a half at byte 1 (inside the word) or byte 3 is excluded: it
  // would cross into the next register.
  uint32_t off = half ? eg2_rand_range(0, 2) : eg2_rand_range(0, 3);
  eg2_chk_trap_t t;
  uint32_t cause = eg2_chk_access(tl_ptr(reg + off, half ? 2 : 1), half ? kEg2ChkLhu : kEg2ChkLbu,
                                  0, &t);
  eg2_chk_step("sub-word load addr", reg + off);
  bool ok = eg2_chk_eq("sub-word load mcause (none)", 0, cause);
  uint32_t mask = half ? 0xffff : 0xff;
  ok = eg2_chk_eq("sub-word load value", (word >> (8 * off)) & mask, t.value) && ok;
  return ok;
}

/** Cases 5 and 6: a store to read-only memory. */
static bool case_ro_write(bool rram) {
  uint32_t base = rram ? kRramScratchBase : TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR;
  uint32_t bytes = rram ? kRramScratchBytes : TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES;
  uint32_t width = draw_width();
  uint32_t addr = base + (eg2_rand_range(0, bytes / 4 - 1) * 4) + (width == 0 ? eg2_rand_range(0, 3)
                                                                   : width == 1
                                                                       ? 2 * eg2_rand_range(0, 1)
                                                                       : 0);
  uint32_t word_addr = addr & ~3u;
  uint32_t before = eg2_chk_rd32(word_addr);
  bool ok = expect_fault(rram ? "rram host store addr" : "rom store addr", addr, store_op(width),
                         kEg2ChkCauseStoreFault);
  ok = eg2_chk_eq("word after the refused store", before, eg2_chk_rd32(word_addr)) && ok;
#ifdef __CHERIOT__
  uint32_t lo = eg2_chk_rd32(addr & ~7u);
  ok = cap_case(addr, true, kEg2ChkCauseStoreFault) && ok;
  ok = cap_untagged(addr, lo) && ok;
#endif
  return ok;
}

static bool case_byte_reg(void) {
  // rv_timer INTR_ENABLE0: one field, bit 0, so a byte write is a whole-register write.
  uint32_t reg = TOP_EARLGREY_RV_TIMER_BASE_ADDR + RV_TIMER_INTR_ENABLE0_REG_OFFSET;
  uint32_t before = eg2_chk_rd32(reg);
  eg2_chk_trap_t t;
  uint32_t cause = eg2_chk_access(tl_ptr(reg, 1), kEg2ChkSb, before & 0xff, &t);
  eg2_chk_step("byte store to a one-byte register", reg);
  bool ok = eg2_chk_eq("byte register store mcause (none)", 0, cause);
  ok = eg2_chk_eq("byte register after the store", before, eg2_chk_rd32(reg)) && ok;
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("TL");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool ok = true;
  const bool gaps = EG2_PARAM(model_gaps) != 0;
#ifndef __CHERIOT__
  // Every fault below must be the bus's: the test ROM leaves ePMP entry 15 locked RWX over the
  // whole address space and the others off (test_rom_start.S).
  uint32_t cfg3, addr15;
  __asm__ volatile("csrr %0, pmpcfg3" : "=r"(cfg3));
  __asm__ volatile("csrr %0, pmpaddr15" : "=r"(addr15));
  ok = eg2_chk_true("ePMP entry 15 locked NAPOT RWX", (cfg3 >> 24) == 0x9f) && ok;
  ok = eg2_chk_true("ePMP entry 15 covers everything", addr15 == 0x7fffffff) && ok;
#endif
  const uint32_t w[kCases] = {
      EG2_PARAM(w_case_gap),         EG2_PARAM(w_case_in_window),
      EG2_PARAM(w_case_subword_write), EG2_PARAM(w_case_misaligned),
      EG2_PARAM(w_case_subword_read), EG2_PARAM(w_case_rom_write),
      EG2_PARAM(w_case_rram_write),  EG2_PARAM(w_case_byte_reg),
  };
  if (gaps) {
    eg2_chk_skip("sub-word and misaligned register writes (cases 2, 3)");
    eg2_chk_skip("ROM and RRAM host writes (cases 5, 6)");
  }
  uint32_t next = 0;
  for (uint32_t n = 0; n < EG2_PARAM(n_cases); ++n) {
    uint32_t c;
    if (EG2_PARAM(order) == 0) {
      // In turn, skipping weight-0 cases.
      c = kCases;
      for (uint32_t k = 0; k < kCases; ++k) {
        uint32_t cand = (next + k) % kCases;
        if (w[cand] != 0) {
          c = cand;
          break;
        }
      }
      next = c + 1;
    } else {
      c = eg2_chk_pick(w, kCases);
    }
    if (c >= kCases) {
      return eg2_chk_true("at least one case with a weight", false);
    }
    if (gaps && (c == kCaseSubwordWrite || c == kCaseMisaligned || c == kCaseRomWrite ||
                 c == kCaseRramWrite)) {
      continue;
    }
    ++counts_[c];
    switch (c) {
      case kCaseGap:
        ok = case_gap() && ok;
        break;
      case kCaseInWindow:
        ok = case_in_window() && ok;
        break;
      case kCaseSubwordWrite:
        ok = case_reg_write(false) && ok;
        break;
      case kCaseMisaligned:
        ok = case_reg_write(true) && ok;
        break;
      case kCaseSubwordRead:
        ok = case_subword_read() && ok;
        break;
      case kCaseRomWrite:
        ok = case_ro_write(false) && ok;
        break;
      case kCaseRramWrite:
        ok = case_ro_write(true) && ok;
        break;
      default:
        ok = case_byte_reg() && ok;
        break;
    }
  }
  for (uint32_t c = 0; c < kCases; ++c) {
    eg2_chk_step("cases made of class", c << 16 | counts_[c]);
  }
  return eg2_chk_summary(EG2_PARAM(n_cases) > 4 ? 4 : 1) && ok;
}
