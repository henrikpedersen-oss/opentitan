// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_illegal_sweep (EG2 verification spec REQ_CIB_07; "CHERIoT Ibex in the Chip"): every
 * encoding the mode's ISA does not define, or defines and rejects, traps as an illegal instruction
 * (mcause 2), with mtval the encoding in RISC-V mode and 0 in CHERIoT mode (CHERIoT-Ibex spec
 * REQ_ISA_11, REQ_ISA_12; Ibex doc exception_interrupts.rst), retires nothing and changes no
 * register; every encoding the ISA defines does not raise that trap.
 *
 * The index space (eg2_isa_decode.h): all 65,536 16-bit parcels (a parcel with bits [1:0] = 11 is
 * the low half of a 32-bit word whose high half is 0), then every 32-bit major opcode x funct3 x
 * funct7 with fixed operand fields rd, rs1, rs2. A run sweeps [first, first + count); the dvsim
 * list splits the space into short runs (chip_eg2_cib_tests.hjson). The expected legal/illegal
 * split of each encoding comes from the decode table eg2_isa_decode.c, built from the ISA and IP
 * documents; the bench's instruction check (Spike in RISC-V mode, CHERIoT-Sail in CHERIoT mode)
 * checks every instruction and trap against the reference model on top of it.
 *
 * Each encoding runs in a 32-byte code slot (eg2_isa_decode.h: a CSR or SCR probe, the encoding,
 * a CSR restore, `ebreak`), in batches of up to 64 slots surrounded by 4 KiB of zeros (c.unimp)
 * on each side, so every PC-relative jump or branch of up to 4 KiB lands on an illegal parcel.
 * The batch's placement is drawn per batch:
 *   0 main SRAM (below the stack), 1 sec SRAM: the software stores the slots (EXEC on);
 *   2 RRAM: the controller writes them to scratch pages 3584-3603 (a region RD | WR | ECC);
 *   3 ROM: the bench writes them by backdoor into ROM offset 0x21000 (pads 0x20000-0x22800) when
 *     the software prints `EG2 ISA batch ...`, and gives the same words to Spike (RISC-V mode);
 *     the software reads back every word and checks it against this file's slots first.
 * The harness (eg2_isa_harness.S) enters each slot by `mret` with every register holding the
 * data address D (an untagged integer in CHERIoT mode, so every capability access and jump
 * through a register faults there); in RISC-V mode D is the middle of an 8 KiB data buffer whose
 * words all hold L, the address of 256 bytes of zeros: loads, stores (re-filled after each),
 * register jumps and the address a cm.popret loads all stay in memory the test owns, where a
 * fetch meets c.unimp.
 *
 * Not executed (counted as skipped; the decode table still classifies them): MRET (it returns
 * through mepc, which the harness enters by), WFI (no interrupt is enabled), a PC-relative jump
 * or branch whose target leaves the padded area or lands in the probes or bytes of its own slot
 * or in another slot's code, a register jump through x0 (its target is unmapped, and the
 * reference model has no fetch errors), a write that changes a PMP or mseccfg CSR (RISC-V mode).
 *
 * Checks, per executed encoding:
 *   - illegal by the table: a trap with mcause 2 at the encoding, mtval as above, and every
 *     register as the slot set it (D; the probe's register and the register it filled as the
 *     probe left them); in CHERIoT mode no register tagged;
 *   - legal by the table: no illegal-instruction trap at the encoding;
 * at the end: the D buffer unchanged (every word L), the pads and L still zero, the number of
 * illegal traps equal to the table's, and no mismatch. Lines `EG2 ISA batch`, `EG2 ISA result` and
 * `EG2 ISA cov` give the bench the placement, the counts and the coverage (class x placement x
 * outcome).
 *
 * Knobs:
 *   first, count          the index range
 *   rd_f, rs1_f, rs2_f    the 32-bit part's fixed operand fields (15, 14, 13: a5, a4, a3, which
 *                         CHERIoT mode has)
 *   w_place_main, w_place_sec, w_place_rram, w_place_rom
 *                         weights of the batch's placement
 *   batch                 slots per batch (at most 64)
 *   max_report            mismatches printed in full on the console
 *   model_gaps            skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the ROM placement (the batch goes to sec     | no bench writes the ROM in eg2sim            |
 * |   SRAM)                                      |                                              |
 * | CHERIoT mode: every placement but one sec    | MPACT-CHERIoT runs stale code at an address  |
 * |   SRAM area of 896 slots, written once (the  |   written again, fence.i notwithstanding     |
 * |   run's range cut to 896 encodings)          |   (model finding; chip_sw_rram_write_then_   |
 * |                                              |   fetch found it for controller writes)      |
 * | CHERIoT mode: the legal/illegal split, mtval | MPACT-CHERIoT differs from the CHERIoT ISA:  |
 * |   and D-buffer writes are logged as model    |   mtval = the encoding, no RV32E check, no   |
 * |   findings (`EG2 ISA model-disagree`), not   |   Zcb, the A extension, CSRs Ibex lacks,     |
 * |   failures; a jump naming x16-x31 whose      |   c.mv HINTs trapped (model findings); the   |
 * |   target is unsafe is not run                |   bench's CHERIoT-Sail is the reference      |
 * | RISC-V mode: mtval of an illegal encoding    | Spike reads bits [4:0] = 11111 as a 48-bit   |
 * |   with bits [4:0] = 11111                    |   or longer instruction and reports mtval 0  |
 * |                                              |   (model finding: Ibex doc cs_registers.rst  |
 * |                                              |   "mtval holds the actual faulting           |
 * |                                              |   instruction", a 32-bit word in Ibex)       |
 * | RISC-V mode: cm.mvsa01 with r1s' = r2s' (8   | Spike executes them; Zcmp reserves them      |
 * |   encodings) not run                         |   (model finding)                            |
 *
 * Built with EG2_ILLEGAL_SWEEP_INJECT_FAIL the first value check (the harness's self-test:
 * c.unimp traps with mcause 2) expects bit 0 flipped: the run must fail with `ISA FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_cib/eg2_isa_decode.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"
#include "sw/device/tests/eg2_rram/eg2_rram.h"

// CHERIoT mode: the boot stage enables execution from main and sec SRAM before it locks sram_ctrl
// (eg2_cheriot_start.S step 0b, REQ_SOC_08); this test runs code there.
const volatile uint8_t eg2_cheriot_boot_sram_exec __attribute__((used)) = 1;

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define ILLEGAL_SWEEP_PARAMS(X) \
  X(first, 0, 0, 98303)         \
  X(count, 2048, 1, 98304)      \
  X(rd_f, 15, 0, 31)            \
  X(rs1_f, 14, 0, 31)           \
  X(rs2_f, 13, 0, 31)           \
  X(w_place_main, 1, 0, 100)    \
  X(w_place_sec, 1, 0, 100)     \
  X(w_place_rram, 1, 0, 100)    \
  X(w_place_rom, 1, 0, 100)     \
  X(batch, 64, 1, 64)           \
  X(max_report, 16, 0, 100000)  \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(ILLEGAL_SWEEP_PARAMS)

enum {
  // Sec SRAM (free in every EG2 test image).
  kLanding = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,  // L: 256 bytes of zeros
  kLandingBytes = 256,
  kCtx = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x400,  // eg2_isa_harness.S EG2_ISA_CTX
  kCov = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x800,  // [class][place][outcome] counters
  kDLo = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x1000,
  kD = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x2000,  // the data address in every register
  kDHi = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x3000,
  kSelfSlots = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x3800,  // the self-test's two slots
  kSecSlots = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x5000,
  // The model's CHERIoT run: one area of 896 slots no batch rewrites (see model_gaps).
  kSecBigSlots = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x8000,
  kSecBigCap = 896,
  kSecEnd = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES,
  // Every area: 4 KiB of zeros, 64 slots (2 KiB), 4 KiB of zeros.
  kPad = 0x1000,
  kSlotsBytes = 64 * kEg2IsaSlotBytes,
  kMaxSlots = 64,
  kGuard = 64,
  // ROM: offset 0x21000 (the test ROM is far below; the digest is at the top).
  kRomSlots = TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR + 0x21000,
  // RRAM: scratch pages 3584.. (eg2_rram.h), 20 pages.
  kRramPage = kEg2RramScratchFirstPage,
  kRramPages = (2 * kPad + kSlotsBytes) / kEg2RramPageBytes,
  kRramLo = kRramPage * kEg2RramPageBytes,  // byte offset in the data partition
  kRramSlots = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR + kRramLo + kPad,
  kPolls = 200000,
  // ctx offsets (eg2_isa_harness.S)
  kCtxMcause = 0x80,
  kCtxMepc = 0x84,
  kCtxMtval = 0x88,
  kCtxResumed = 0x90,
};

enum {
  kPlaceMain = 0,
  kPlaceSec = 1,
  kPlaceRram = 2,
  kPlaceRom = 3,
  kPlaceCount = 4,
};

#ifdef __CHERIOT__
enum { kRegs = 16, kRegStride = 2 };  // c0-c15, 8 bytes each
static const bool kCheriot = true;
#else
enum { kRegs = 32, kRegStride = 1 };
static const bool kCheriot = false;
#endif

void eg2_isa_run(uint32_t slot, uint32_t d, volatile void *ctx);

typedef struct area {
  uint32_t slots;  // the first slot's address
  uint32_t cap;    // slots it holds (64; more in the model's CHERIoT run)
  bool ready;      // the pads are in place
  bool usable;
} area_t;

typedef struct sweep {
  area_t area[kPlaceCount];
  uint32_t ops;
  uint32_t rram_region;
  // Totals.
  uint32_t executed, skipped, exp_illegal, got_illegal, mismatches, reported, resumed;
  uint32_t bad_words;
  // model_gaps in CHERIoT mode: MPACT-CHERIoT's legal/illegal split and mtval are logged as
  // model findings (`EG2 ISA model-disagree`), not failures.
  bool model_cheriot;
  uint32_t model_findings;
} sweep_t;

// The context and the counters, as pointers made once (a capability lookup each in CHERIoT mode).
static volatile uint32_t *ctx_p_;
static volatile uint32_t *cov_p_;

static volatile uint32_t *ctx_(void) { return ctx_p_; }
static uint32_t ctx_word(uint32_t off) { return ctx_()[off / 4]; }

/** Register `r`'s value (its address in CHERIoT mode) in the handler's dump. */
static uint32_t dump_reg(uint32_t r) { return ctx_()[r * kRegStride]; }

static bool dump_tag(uint32_t r) {
#ifdef __CHERIOT__
  void *volatile *cp = (void *volatile *)(uintptr_t)&ctx_()[r * kRegStride];
  return __builtin_cheri_tag_get(*cp);
#else
  (void)r;
  return false;
#endif
}

static void cov_add(uint32_t cls, uint32_t place, uint32_t outcome) {
  cov_p_[(cls * kPlaceCount + place) * 2 + outcome] += 1;
}

static void fill(uint32_t lo, uint32_t hi, uint32_t v) {
  volatile uint32_t *p = eg2_chk_ptr(lo);
  for (uint32_t i = 0; i < (hi - lo) / 4; ++i) {
    p[i] = v;
  }
}

/** Counts the words of [lo, hi) that differ from `v`. */
static uint32_t count_diff(uint32_t lo, uint32_t hi, uint32_t v) {
  volatile uint32_t *p = eg2_chk_ptr(lo);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < (hi - lo) / 4; ++i) {
    if (p[i] != v) {
      if (bad == 0) {
        LOG_INFO("EG2 ISA word changed 0x%08x 0x%08x exp 0x%08x", lo + 4 * i, p[i], v);
      }
      ++bad;
    }
  }
  return bad;
}

/** Every RRAM write of the test: whole RRAM words, at most 1024 bus words per operation. */
static bool rram_write(uint32_t off, const uint32_t *w, uint32_t words) {
  eg2_rram_result_t r;
  return eg2_rram_write(kEg2RramPartData, off, w, words, &r) && r.op_status == 1;
}

/** The pads of an area, once: zeros around its slots. */
static bool area_prepare(sweep_t *s, uint32_t place) {
  area_t *a = &s->area[place];
  if (a->ready) {
    return true;
  }
  uint32_t lo = a->slots - kPad;
  uint32_t hi = a->slots + a->cap * kEg2IsaSlotBytes + kPad;
  bool ok = true;
  if (place == kPlaceMain || place == kPlaceSec) {
    fill(lo, hi, 0);
  } else if (place == kPlaceRram) {
    static const uint32_t kZeros[64] = {0};
    for (uint32_t off = kRramLo; off < kRramLo + (hi - lo) && ok; off += sizeof(kZeros)) {
      ok = rram_write(off, kZeros, 64);
    }
    ok = eg2_chk_true("RRAM area written", ok);
  }
  // ROM: the bench writes the pads with the first batch; checked after it.
  a->ready = place != kPlaceRom;
  return ok;
}

/** One slot's words: encoding `first + k` for k < n, zeros after. */
static void slot_words(const sweep_t *s, uint32_t first, uint32_t n, uint32_t k, uint32_t *w) {
  eg2_isa_slot_t sl;
  if (k < n) {
    eg2_isa_slot(eg2_isa_encoding(first + k, s->ops), kCheriot, &sl);
  }
  for (uint32_t i = 0; i < kEg2IsaSlotWords; ++i) {
    w[i] = k < n ? sl.w[i] : 0;
  }
}

/** Places a batch; false if its words are not in place (then it is not run). */
static bool place_batch(sweep_t *s, uint32_t place, uint32_t first, uint32_t n) {
  enum { kWords = kMaxSlots * kEg2IsaSlotWords };
  uint32_t w[kWords];  // RRAM and ROM batches: 64 slots
  area_t *a = &s->area[place];
  if (!area_prepare(s, place)) {
    return false;
  }
  if (place == kPlaceMain || place == kPlaceSec) {
    // Straight into the SRAM, every slot of the area (unused ones 0).
    volatile uint32_t *p = eg2_chk_ptr(a->slots);
    for (uint32_t k = 0; k < a->cap; ++k) {
      uint32_t sw[kEg2IsaSlotWords];
      slot_words(s, first, n, k, sw);
      for (uint32_t i = 0; i < kEg2IsaSlotWords; ++i) {
        p[k * kEg2IsaSlotWords + i] = sw[i];
      }
    }
  } else {
    for (uint32_t k = 0; k < kMaxSlots; ++k) {
      slot_words(s, first, n, k, &w[k * kEg2IsaSlotWords]);
    }
    if (place == kPlaceRram &&
        !eg2_chk_true("RRAM slots written", rram_write(kRramLo + kPad, w, kWords))) {
      return false;
    }
  }
  // The bench: writes the ROM batch (and the ROM pads the first time), and in RISC-V mode gives
  // ROM and RRAM words to Spike, whose fetches do not see device or backdoor writes.
  LOG_INFO("EG2 ISA batch 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x", place, a->slots, first, n,
           s->ops);
  if (place >= kPlaceRram) {
    eg2_chk_delay(200);
  }
  eg2_chk_fence_i();
  bool ok = true;
  if (place >= kPlaceRram) {
    // Every word as this file builds it: the RRAM write, and the bench's ROM writes.
    volatile uint32_t *p = eg2_chk_ptr(a->slots);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < kWords; ++i) {
      if (p[i] != w[i]) {
        if (bad == 0) {
          eg2_chk_step("slot word differs at", a->slots + 4 * i);
          eg2_chk_step("  read", p[i]);
          eg2_chk_step("  built", w[i]);
        }
        ++bad;
      }
    }
    if (!a->ready) {  // the ROM pads, after the bench's first batch
      bad += count_diff(a->slots - kPad, a->slots, 0);
      bad += count_diff(a->slots + kSlotsBytes, a->slots + kSlotsBytes + kPad, 0);
      a->ready = true;
    }
    ok = eg2_chk_eq(place == kPlaceRom ? "ROM batch words as built" : "RRAM batch words as built",
                    0, bad);
  }
  return ok;
}

/** True if `tgt` is in code of the batch a jump must not land in. */
static bool in_slot_code(const sweep_t *s, const area_t *ar, uint32_t first, uint32_t n,
                         uint32_t k, uint32_t tgt) {
  uint32_t base = ar->slots;
  if (tgt < base || tgt >= base + ar->cap * kEg2IsaSlotBytes) {
    return false;
  }
  uint32_t j = (tgt - base) / kEg2IsaSlotBytes;
  if (j >= n) {
    return false;
  }
  uint32_t sj = base + j * kEg2IsaSlotBytes;
  if (j == k) {
    // Its own probes and its own bytes loop or re-run the slot; its restore and ebreak are fine.
    uint32_t insn = eg2_isa_encoding(first + k, s->ops);
    return tgt < sj + kEg2IsaTestOff + (eg2_isa_is16(insn) ? 2u : 4u);
  }
  eg2_isa_slot_t sl;
  eg2_isa_slot(eg2_isa_encoding(first + j, s->ops), kCheriot, &sl);
  return tgt < sj + sl.end_off + 4;
}

/** True if the harness may run the legal encoding (the header's "Not executed" list). */
static bool runnable(const sweep_t *s, const eg2_isa_info_t *in, uint32_t insn, const area_t *ar,
                     uint32_t first, uint32_t n, uint32_t k) {
  uint32_t base = ar->slots;
  uint32_t a = base + k * kEg2IsaSlotBytes + kEg2IsaTestOff;
  switch (in->kind) {
    case kEg2IsaKindMret:
    case kEg2IsaKindWfi:
      return false;
    case kEg2IsaKindXferPc: {
      // A guard of 64 bytes inside each end: the prefetcher reads a few words past a fetch.
      uint32_t tgt = a + (uint32_t)in->off;
      if (tgt < base - kPad + kGuard ||
          tgt >= base + ar->cap * kEg2IsaSlotBytes + kPad - kGuard) {
        return false;
      }
      return !in_slot_code(s, ar, first, n, k, tgt);
    }
    case kEg2IsaKindXferReg:
      return in->base != 0;  // RISC-V mode: D + off is in the D buffer
    case kEg2IsaKindCsr: {
      if (kCheriot) {
        return true;
      }
      uint32_t c = in->csr;
      bool pmp = (c >= 0x3a0 && c <= 0x3a3) || (c >= 0x3b0 && c <= 0x3bf) || c == 0x747 ||
                 c == 0x757;
      uint32_t f3 = (insn >> 12) & 7;
      uint32_t rs1 = (insn >> 15) & 0x1f;
      bool changes = (f3 == 1 && rs1 == 0) || f3 == 5 || ((f3 == 6 || f3 == 7) && rs1 != 0);
      return !(pmp && changes);
    }
    default:
      return true;
  }
}

/** After a legal store in RISC-V mode: the words it may have written back to L. */
static void refill(const eg2_isa_info_t *in) {
  if (kCheriot) {
    return;
  }
  if (in->kind == kEg2IsaKindPush) {
    fill(kD - 64, kD, kLanding);
  } else if (in->kind == kEg2IsaKindStore && in->base != 0) {
    uint32_t addr = kD + (uint32_t)in->off;
    uint32_t lo = addr & ~3u;
    uint32_t hi = (addr + in->size + 3) & ~3u;
    if (lo >= kDLo && hi <= kDHi) {
      fill(lo, hi, kLanding);
    }
  }
}

static void report(sweep_t *s, uint32_t idx, uint32_t insn, const eg2_isa_info_t *in,
                   const char *what) {
  ++s->mismatches;
  LOG_INFO("EG2 ISA mismatch 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x", idx, insn,
           (uint32_t)in->legal, ctx_word(kCtxMcause), ctx_word(kCtxMepc), ctx_word(kCtxMtval));
  if (s->reported < EG2_PARAM(max_report)) {
    ++s->reported;
    eg2_chk_true(what, false);
    eg2_chk_step("  encoding", insn);
    eg2_chk_step("  index", idx);
    eg2_chk_step("  mcause", ctx_word(kCtxMcause));
    eg2_chk_step("  mepc", ctx_word(kCtxMepc));
    eg2_chk_step("  mtval", ctx_word(kCtxMtval));
  }
}

/** A legality or mtval disagreement: a failure, or under model_gaps in CHERIoT mode a model finding. */
static void report_split(sweep_t *s, uint32_t idx, uint32_t insn, const eg2_isa_info_t *in,
                         const char *what) {
  if (!s->model_cheriot) {
    report(s, idx, insn, in, what);
    return;
  }
  ++s->model_findings;
  LOG_INFO("EG2 ISA model-disagree 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x", idx, insn,
           (uint32_t)in->legal, ctx_word(kCtxMcause), ctx_word(kCtxMepc), ctx_word(kCtxMtval));
}

/** Runs one encoding from its placed slot and checks it. */
static void run_one(sweep_t *s, uint32_t place, uint32_t first, uint32_t n, uint32_t k,
                    uint32_t *b_exp, uint32_t *b_got) {
  uint32_t idx = first + k;
  uint32_t insn = eg2_isa_encoding(idx, s->ops);
  eg2_isa_info_t in;
  eg2_isa_decode(insn, kCheriot, &in);
  uint32_t base = s->area[place].slots;
  // eg2sim's Spike executes cm.mvsa01 with r1s' = r2s', which Zcmp reserves ("r1s' and r2s'
  // must be different"): not run in the model (model finding).
  bool mvsa01_same = !kCheriot && (insn & 0xfc63) == 0xac22 && ((insn >> 7) & 7) == ((insn >> 2) & 7);
  // MPACT-CHERIoT executes some encodings the table holds illegal for naming x16-x31 (cjal x16, 0
  // loops on itself): in the model, such a jump runs only where its target is safe.
  bool x16_jump_unsafe = s->model_cheriot && in.x16 && in.kind == kEg2IsaKindXferPc &&
                         !runnable(s, &in, insn, &s->area[place], first, n, k);
  if ((in.legal && !runnable(s, &in, insn, &s->area[place], first, n, k)) || x16_jump_unsafe ||
      (mvsa01_same && EG2_PARAM(model_gaps) != 0)) {
    ++s->skipped;
    return;
  }
  uint32_t slot = base + k * kEg2IsaSlotBytes;
  uint32_t a = slot + kEg2IsaTestOff;
  eg2_isa_run(slot, kD, ctx_());
  ++s->executed;
  s->resumed += ctx_word(kCtxResumed);
  uint32_t cause = ctx_word(kCtxMcause);
  bool ill_at_a = cause == kEg2ChkCauseIllegal && ctx_word(kCtxMepc) == a;
  cov_add(in.cls, place, ill_at_a ? 0 : 1);
  if (!in.legal) {
    ++*b_exp;
  }
  if (ill_at_a) {
    ++*b_got;
  }
  if (in.legal) {
    refill(&in);
    if (ill_at_a) {
      report_split(s, idx, insn, &in, "a legal encoding trapped as illegal");
    }
    return;
  }
  if (!ill_at_a) {
    report_split(s, idx, insn, &in, "an illegal encoding did not trap as illegal at itself");
    refill(&in);  // what it may have done as a store
    return;
  }
  uint32_t tval = kCheriot ? 0 : insn;
  // eg2sim's Spike reads bits [4:0] = 11111 as the prefix of a 48-bit or longer instruction,
  // fetches that many bytes and reports mtval 0; Ibex decodes a 32-bit word and, by
  // cs_registers.rst, holds the faulting instruction in mtval (model finding).
  bool long_prefix = !kCheriot && (insn & 0x1f) == 0x1f;
  if (long_prefix && EG2_PARAM(model_gaps) != 0) {
    tval = ctx_word(kCtxMtval);
  }
  if (ctx_word(kCtxMtval) != tval) {
    report_split(s, idx, insn, &in, "mtval of an illegal-instruction trap");
    return;
  }
  // No register changed: each holds D, except the probe's register and the one it filled.
  eg2_isa_slot_t sl;
  eg2_isa_slot(insn, kCheriot, &sl);
  for (uint32_t r = 1; r < kRegs; ++r) {
    if (r == sl.probe_reg) {
      continue;
    }
    uint32_t exp = kD;
    if (r == sl.probe1_reg) {
      exp = sl.probe1_copy ? dump_reg(sl.probe_reg) : 0;
    } else if (dump_tag(r)) {
      LOG_INFO("EG2 ISA reg 0x%08x tagged 0x%08x", r, dump_reg(r));
      report(s, idx, insn, &in, "a register tagged after an illegal-instruction trap");
      return;
    }
    if (dump_reg(r) != exp) {
      LOG_INFO("EG2 ISA reg 0x%08x 0x%08x exp 0x%08x", r, dump_reg(r), exp);
      report(s, idx, insn, &in, "a register changed by an illegal-instruction trap");
      return;
    }
  }
}

static uint32_t draw_place(const sweep_t *s) {
  uint32_t w[kPlaceCount] = {EG2_PARAM(w_place_main), EG2_PARAM(w_place_sec),
                             EG2_PARAM(w_place_rram), EG2_PARAM(w_place_rom)};
  for (uint32_t p = 0; p < kPlaceCount; ++p) {
    if (!s->area[p].usable) {
      w[p] = 0;
    }
  }
  uint32_t p = eg2_chk_pick(w, kPlaceCount);
  return p == kPlaceCount ? kPlaceSec : p;
}

// The linker's symbols as integers (CHERIoT: not capabilities).
#define SYM_ADDR_(fn, sym)                                                     \
  static uint32_t fn(void) {                                                   \
    uint32_t a;                                                                \
    __asm__("lui %0, %%hi(" #sym ")\n addi %0, %0, %%lo(" #sym ")" : "=r"(a)); \
    return a;                                                                  \
  }
SYM_ADDR_(sym_stack_start, _stack_start)
SYM_ADDR_(sym_bss_end, _bss_end)

/** The harness's self-test: c.unimp traps at the slot, c.nop falls through to its ebreak. */
static bool self_test(sweep_t *s) {
  bool ok = true;
  static const uint32_t kInsns[2] = {0x0000, 0x0001};
  for (uint32_t i = 0; i < 2; ++i) {
    eg2_isa_slot_t sl;
    eg2_isa_slot(kInsns[i], kCheriot, &sl);
    // Two slots: MPACT-CHERIoT runs stale code at an address written again.
    uint32_t slot = kSelfSlots + i * kEg2IsaSlotBytes;
    volatile uint32_t *p = eg2_chk_ptr(slot);
    for (uint32_t j = 0; j < kEg2IsaSlotWords; ++j) {
      p[j] = sl.w[j];
    }
    eg2_chk_fence_i();
    eg2_isa_run(slot, kD, ctx_());
    if (i == 0) {
      ok = eg2_chk_eq("self-test: c.unimp mcause", kEg2ChkCauseIllegal, ctx_word(kCtxMcause)) &&
           ok;
      ok = eg2_chk_eq("self-test: c.unimp mepc", slot + kEg2IsaTestOff,
                      ctx_word(kCtxMepc)) &&
           ok;
    } else {
      ok = eg2_chk_eq("self-test: c.nop ends at the ebreak (mcause 3)", 3,
                      ctx_word(kCtxMcause)) &&
           ok;
      ok = eg2_chk_eq("self-test: c.nop ends at the ebreak (mepc)", slot + sl.end_off,
                      ctx_word(kCtxMepc)) &&
           ok;
    }
  }
  (void)s;
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("ISA");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  sweep_t s = {0};
  s.model_cheriot = gaps && kCheriot;
  ctx_p_ = eg2_chk_ptr(kCtx);
  cov_p_ = eg2_chk_ptr(kCov);
  s.ops = EG2_PARAM(rd_f) | EG2_PARAM(rs1_f) << 5 | EG2_PARAM(rs2_f) << 10;
  eg2_chk_step("mode (1 CHERIoT)", kCheriot);

  // Sec SRAM: whole words first (integrity: the prefetcher reads ahead of every fetch, and a
  // word no write has initialised fails its integrity check), L zeros, the counters, the D buffer
  // of L, the sec area's pads; execution on in both SRAMs (OTP HW_CFG1.EN_SRAM_IFETCH is
  // MuBi8True in the test images).
  fill(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR, s.model_cheriot ? kSecEnd : kSecSlots + kSlotsBytes + kPad, 0);
  fill(kDLo, kDHi, kLanding);
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMultiBitBool4True);
  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMultiBitBool4True);

  uint32_t main_slots = (sym_stack_start() - 0x2000) & ~0xffu;
  s.area[kPlaceMain] = (area_t){.slots = main_slots, .cap = kMaxSlots,
                                .usable = main_slots - kPad >= sym_bss_end()};
  s.area[kPlaceSec] = (area_t){.slots = kSecSlots, .cap = kMaxSlots, .usable = true, .ready = true};
  s.area[kPlaceRram] = (area_t){.slots = kRramSlots, .cap = kMaxSlots, .usable = false};
  // The ROM placement needs the chip bench's backdoor (sim_dv): on the Verilator top the ROM keeps
  // its built words (2026-10-27: "ROM batch words as built exp 0x00000000 got 0x00000100").
  bool rom_bench = kDeviceType == kDeviceSimDV;
  s.area[kPlaceRom] = (area_t){.slots = kRomSlots, .cap = kMaxSlots, .usable = !gaps && rom_bench};
  uint32_t nb = EG2_PARAM(batch);
  if (s.model_cheriot) {
    // MPACT-CHERIoT runs stale code at an address written again (model finding): one batch in
    // one area of sec SRAM that nothing rewrites, at most kSecBigCap encodings.
    eg2_chk_skip("CHERIoT: every placement but one sec SRAM area of 896 slots written once");
    s.area[kPlaceMain].usable = false;
    s.area[kPlaceSec] = (area_t){.slots = kSecBigSlots, .cap = kSecBigCap, .usable = true, .ready = true};
    nb = kSecBigCap;
  }
  if (!s.area[kPlaceMain].usable) {
    eg2_chk_step("main SRAM placement off: globals reach", sym_bss_end());
  }
  if (gaps) {
    eg2_chk_skip("the ROM placement (no bench)");
  } else if (!rom_bench) {
    eg2_chk_skip("the ROM placement (no bench: not sim_dv)");
  }
  if (!(gaps && kCheriot) && EG2_PARAM(w_place_rram) != 0) {
    if (eg2_chk_true("RRAM controller initialised", eg2_rram_wait_init(kPolls))) {
      s.rram_region = eg2_rram_free_region();
      if (eg2_chk_true("a free RRAM data region", s.rram_region < kEg2RramRegions)) {
        eg2_rram_region_set(s.rram_region, kRramPage, kRramPages,
                            kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
        s.area[kPlaceRram].usable = true;
      }
    }
  } else if (gaps && kCheriot) {
    eg2_chk_skip("CHERIoT: the RRAM placement (stale ISS code after a rewrite)");
  }

  bool ok = self_test(&s);

  uint32_t first = EG2_PARAM(first);
  uint32_t end = first + EG2_PARAM(count);
  if (end > kEg2IsaCount) {
    end = kEg2IsaCount;
  }
  if (s.model_cheriot && end - first > kSecBigCap) {
    eg2_chk_step("model run: the range cut to (encodings)", kSecBigCap);
    end = first + kSecBigCap;
  }
  for (uint32_t b0 = first; b0 < end; b0 += nb) {
    uint32_t n = end - b0 < nb ? end - b0 : nb;
    uint32_t place = draw_place(&s);
    if (!place_batch(&s, place, b0, n)) {
      ok = false;
      continue;
    }
    uint32_t b_exp = 0, b_got = 0;
    for (uint32_t k = 0; k < n; ++k) {
      run_one(&s, place, b0, n, k, &b_exp, &b_got);
    }
    LOG_INFO("EG2 ISA result 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x", place, b0, n, b_exp, b_got);
    s.exp_illegal += b_exp;
    s.got_illegal += b_got;
  }

  // Coverage (class x placement x outcome) for the bench.
  volatile uint32_t *c = cov_p_;
  for (uint32_t cls = 0; cls < kEg2IsaClassCount; ++cls) {
    for (uint32_t p = 0; p < kPlaceCount; ++p) {
      uint32_t i = (cls * kPlaceCount + p) * 2;
      if (c[i] != 0 || c[i + 1] != 0) {
        LOG_INFO("EG2 ISA cov 0x%08x 0x%08x 0x%08x 0x%08x", cls, p, c[i], c[i + 1]);
      }
    }
  }
  // The padded areas still zero: no stray store reached code.
  for (uint32_t p = 0; p < kPlaceCount; ++p) {
    area_t *a = &s.area[p];
    if (a->ready && (p == kPlaceMain || p == kPlaceSec)) {
      s.bad_words += count_diff(a->slots - kPad, a->slots, 0);
      uint32_t end_slots = a->slots + a->cap * kEg2IsaSlotBytes;
      s.bad_words += count_diff(end_slots, end_slots + kPad, 0);
    }
  }
  s.bad_words += count_diff(kLanding, kLanding + kLandingBytes, 0);
  // Nothing wrote the D buffer that the harness did not put back. (MPACT-CHERIoT writes through
  // untagged capabilities with the A extension the configuration lacks: a model finding.)
  uint32_t dbad = count_diff(kDLo, kDHi, kLanding);
  if (s.model_cheriot) {
    s.model_findings += dbad;
  } else {
    s.bad_words += dbad;
  }

  eg2_chk_step("first", first);
  eg2_chk_step("end", end);
  eg2_chk_step("executed", s.executed);
  eg2_chk_step("skipped (not run)", s.skipped);
  eg2_chk_step("probe traps resumed", s.resumed);
  if (s.model_cheriot) {
    eg2_chk_step("model findings (MPACT-CHERIoT against the table)", s.model_findings);
    eg2_chk_step("  illegal traps, table", s.exp_illegal);
    eg2_chk_step("  illegal traps, model", s.got_illegal);
    eg2_chk_skip("CHERIoT: the legal/illegal split and mtval against MPACT-CHERIoT");
  } else {
    ok = eg2_chk_eq("illegal-instruction traps equal the decode table's", s.exp_illegal,
                    s.got_illegal) &&
         ok;
  }
  ok = eg2_chk_eq("encodings that disagree with the decode table", 0, s.mismatches) && ok;
  ok = eg2_chk_eq("words changed in the D buffer, the pads and L", 0, s.bad_words) && ok;
  ok = eg2_chk_true("every encoding of the range executed or skipped",
                    s.executed + s.skipped == end - first) &&
       ok;
  return eg2_chk_summary(6) && ok;
}
