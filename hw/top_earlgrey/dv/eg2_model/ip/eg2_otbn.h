// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The OTBN processor of the EG2 SystemC model: it runs the program software starts.
// Eg2Otbn (eg2_otbn_ctrl.h) is the block around it: registers,
// commands, the memories over the bus, timing, interrupt and alerts. This file needs no SystemC,
// so a program can also be run on its own. Link eg2_otbn_execute.cc and
// eg2_otbn_accelerators.cc; arithmetic and sponge helpers have their own headers.
//
// From hw/ip/otbn/README.md ("Processor State": GPRs, call stack, CSRs, WDRs, WSRs, flags, loop
// stack), hw/ip/otbn/data/{base,bignum}-insns.yml with enc-schemes.yml (the 66 instructions, their
// encodings and errors) and hw/ip/otbn/doc/theory_of_operation.md (errors, random numbers, the
// KMAC and masking accelerator interfaces, secure wipe).
//
//   Execution  From address 0 until ECALL or an error; WFI pauses when CTRL.wfi_enabled is set.
//              An instruction that raises an error changes nothing and does not count in INSN_CNT
//              (registers.md INSN_CNT); its error bits are those of the instruction's "errs" list.
//              Cycles: one per instruction; two for LW, BN.LID, BN.SID and BN.MOVR, and for
//              branches and jumps (the fetch stall of "Instruction Prefetch"); four for BN.MULV and
//              BN.MULVL, twelve for BN.MULVM and BN.MULVML (bignum-insns.yml).
//   Random     RND: 256 bits from edn_rnd (eight 32-bit words, least significant first), held in
//              the single-entry cache that RND_PREFETCH fills; a read takes the cache, and waits
//              for EDN when it is empty. URND: a local generator seeded from edn_urnd (six 32-bit
//              words) at the start of each execution, advancing every instruction. Its state is
//              128 bits, not Bivium's 177: URND_STATE reads it zero-extended and a restore takes
//              the six 32-bit words URND_STATUS announces; its values are not the chip's.
//   Key        KEY_S0_L/H and KEY_S1_L/H from keymgr's sideload interface; a read without a key is
//              a KEY_INVALID error.
//   KMAC       The interface's session ("KMAC interface") with SHA3, SHAKE, cSHAKE (the KMAC
//              block's PREFIX) and KMAC (the KMAC block's software key, 384 bits out). Every step
//              completes at once: READY stays 1, the first digest beat is valid right after
//              PROCESS, the next one once both shares were read, the final response right after
//              DONE. The digest's shares are masked with URND.
//   MAI        secAdd, secAddMod, A2B and B2A: the results are fresh sharings of the values
//              "Masking accelerator interface" specifies. MAI_STATUS.BUSY is set from the cycle
//              after START for the operation's documented cycles for the full vector (14, 22, 22
//              and 23, B2A without rejections) plus two cycles to start and write back, as
//              otbnsim counts them; the results land in MAI_RES_S0/S1 when BUSY clears.
//              INPUT_READY is clear for the eight cycles the inputs are dispatched.
//
// Not modelled: the memories' and registers' integrity code and scrambling (no integrity errors),
// RND's repetition and FIPS checks, the random overwrites of the secure wipe (the state is zeroed;
// software must not rely on it, theory of operation "Internal State Secure Wipe").
//
// Where the documentation is unclear, otbnsim (hw/ip/otbn/dv/otbnsim/sim) was read: the flags of
// BN.MULQACC.SO (the yml's text says "set iff ... is zero"; otbnsim and the RTL set M and L from
// the bits written), BN.ADDM subtracting MOD when the sum equals it, the error bits an instruction
// collects when one of its GPRs is x1 on an empty call stack, the loop stack seeing only its top
// entry at the end of a body, and the MAI's results for inputs out of range.

#ifndef EG2_MODEL_IP_EG2_OTBN_H_
#define EG2_MODEL_IP_EG2_OTBN_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"

#include "eg2_otbn_wide.h"
#include "eg2_otbn_keccak.h"

namespace eg2_model {

class Eg2OtbnCore {
 public:
  static constexpr uint32_t kImemBytes = 16 * 1024;  // theory of operation "Memories"
  static constexpr uint32_t kDmemBytes = 32 * 1024;
  static constexpr unsigned kStackDepth = 8;  // call stack and loop stack (README.md)
  // A URND reseed: edn_urnd answers one 32-bit word per request (otbn.sv), and the generator's
  // 177-bit state takes 32-bit parts (URND_STATUS: state width 177, restore width 32): six words.
  static constexpr unsigned kUrndSeedWords = 6;

  // ERR_BITS (registers.md).
  enum Err : uint32_t {
    kBadDataAddr = 1u << 0, kBadInsnAddr = 1u << 1, kCallStack = 1u << 2, kIllegalInsn = 1u << 3,
    kLoop = 1u << 4, kKeyInvalid = 1u << 5, kRndRepChkFail = 1u << 6, kRndFipsChkFail = 1u << 7,
    kMaiError = 1u << 8, kImemIntg = 1u << 16, kDmemIntg = 1u << 17, kRegIntg = 1u << 18,
    kBusIntg = 1u << 19, kBadInternalState = 1u << 20, kIllegalBusAccess = 1u << 21,
    kLifecycleEscalation = 1u << 22, kFatalSoftware = 1u << 23, kFatalMask = 0xff0000u
  };
  // Why run() returned: the program ended (ECALL or an error: err_bits()), paused on WFI, waits for
  // EDN, or used up its instruction budget.
  enum class Stop { kEnd, kWfi, kWaitEdn, kBudget };

  // CSR and WSR numbers (csr.yml, wsr.yml).
  enum Csr : uint32_t {
    kFg0 = 0x7c0, kFg1 = 0x7c1, kFlags = 0x7c8, kMod0 = 0x7d0, kMod7 = 0x7d7, kRndPrefetch = 0x7d8,
    kUrndCtrl = 0x7d9, kKmacStatus = 0x7db, kKmacCtrl = 0x7dc, kKmacCfg = 0x7dd,
    kKmacStrb = 0x7de, kMaiCtrl = 0x7e0, kRnd = 0xfc0, kUrnd = 0xfc1, kUrndStatus = 0xfc2,
    kInsnCnt = 0xfc3, kMaiStatus = 0xfca
  };
  enum Wsr : uint32_t {
    kWMod = 0, kWRnd = 1, kWUrnd = 2, kWAcc = 3, kWKeyS0L = 4, kWKeyS0H = 5, kWKeyS1L = 6,
    kWKeyS1H = 7, kWKmacS0 = 8, kWKmacS1 = 9, kWMaiResS0 = 10, kWMaiResS1 = 11, kWMaiIn0S0 = 12,
    kWMaiIn0S1 = 13, kWMaiIn1S0 = 14, kWMaiIn1S1 = 15, kWUrndState = 16, kNumWsrs = 17
  };

  // What the processor takes from the rest of the chip.
  struct Hooks {
    std::function<bool(uint32_t *)> edn_rnd;   // a 32-bit word from edn_rnd; false: none yet
    std::function<bool(uint32_t *)> edn_urnd;  // a 32-bit word from edn_urnd
    std::function<std::vector<uint8_t>()> kmac_prefix;  // the KMAC block's PREFIX, as bytes
    std::function<std::vector<uint8_t>()> kmac_key;     // the KMAC block's software key
  } hooks;

  std::vector<uint8_t> imem = std::vector<uint8_t>(kImemBytes, 0);
  std::vector<uint8_t> dmem = std::vector<uint8_t>(kDmemBytes, 0);
  // CTRL, as the block had it when the operation started.
  bool wfi_enabled = false, urnd_ctrl_enabled = false;

  // The sideload key: shares 0 and 1, 512 bits each ({_H, _L}).
  void set_key(const OtbnWide &s0l, const OtbnWide &s0h, const OtbnWide &s1l, const OtbnWide &s1h,
               bool valid) {
    key_[0] = s0l;
    key_[1] = s0h;
    key_[2] = s1l;
    key_[3] = s1h;
    key_valid_ = valid;
    if (!valid) {
      for (auto &k : key_) k = OtbnWide();
    }
  }

  // Everything to zero, memories included (reset).
  void reset() {
    std::fill(imem.begin(), imem.end(), 0);
    std::fill(dmem.begin(), dmem.end(), 0);
    wipe_internal();
    rnd_valid_ = false;
    rnd_words_ = 0;
    insn_cnt_ = 0;
    err_ = 0;
  }

  // EXECUTE ("Software Execution"): the RND cache is discarded, the RW CSRs, MOD and ACC are zero,
  // URND reseeds from EDN, execution starts at address 0.
  void start() {
    wipe_internal();
    pc_ = 0;
    err_ = 0;
    insn_cnt_ = 0;
    cycles_ = 0;
    rnd_valid_ = false;
    rnd_words_ = 0;
    urnd_words_ = 0;
    urnd_seeded_ = false;
    urnd_stopped_ = urnd_used_while_stopped_ = urnd_restoring_ = false;
    paused_ = ended_ = false;
  }

  // Run up to max_insns instructions.
  Stop run(uint64_t max_insns) {
    if (ended_) return Stop::kEnd;
    if (!urnd_seeded_ && !seed_urnd()) return Stop::kWaitEdn;
    for (uint64_t n = 0; n < max_insns; ++n) {
      Stop s = step();
      if (s != Stop::kBudget) return s;
    }
    return Stop::kBudget;
  }
  // RESUME after a WFI pause.
  void resume() { paused_ = false; }
  // A fatal error from outside the processor (life cycle escalation, a bus access while busy):
  // the program ends with it.
  void fatal(uint32_t bits) {
    err_ |= bits;
    ended_ = true;
  }

  // Internal state secure wipe ("Internal State Secure Wipe"): registers, flags, MOD, ACC, the
  // KMAC and MAI registers; loop and call stacks emptied.
  void wipe_internal() {
    gpr_.fill(0);
    for (auto &w : wdr_) w = OtbnWide();
    cstack_.clear();
    lstack_.clear();
    flags_[0] = flags_[1] = 0;
    mod_ = acc_ = OtbnWide();
    kmac_s0_ = kmac_s1_ = OtbnWide();
    for (auto &w : mai_) w = OtbnWide();
    mai_op_ = kMaiA2b;
    mai_busy_until_ = mai_ready_at_ = 0;
    mai_pending_ = false;
    kmac_reset();
  }

  uint32_t err_bits() const { return err_; }
  uint32_t insn_cnt() const { return insn_cnt_; }
  uint64_t cycles() const { return cycles_; }
  void add_cycles(uint64_t n) { cycles_ += n; }
  uint32_t pc() const { return pc_; }
  bool paused() const { return paused_; }
  // State, for a program run on its own (comparisons with otbnsim): after start(), a test case's
  // entry point and register inputs; at the end, the registers.
  void set_pc(uint32_t pc) { pc_ = pc; }
  void set_gpr(unsigned i, uint32_t v) {
    if (i == 1) cstack_.push_back(v);
    else if (i) gpr_[i] = v;
  }
  void set_wdr(unsigned i, const OtbnWide &v) { wdr_[i] = v; }
  uint32_t gpr(unsigned i) const {
    return i == 1 ? (cstack_.empty() ? 0 : cstack_.back()) : gpr_[i];
  }
  const OtbnWide &wdr(unsigned i) const { return wdr_[i]; }
  uint8_t flags(unsigned fg) const { return flags_[fg]; }
  const OtbnWide &acc() const { return acc_; }
  const OtbnWide &mod() const { return mod_; }

 private:
  // Flag bits in a group (the FG0 / FG1 CSR layout).
  enum : uint8_t { kC = 1, kM = 2, kL = 4, kZ = 8 };
  enum MaiOp : uint32_t { kMaiA2b = 11, kMaiB2a = 16, kMaiSecAdd = 23, kMaiSecAddMod = 12 };
  struct Loop {
    uint32_t start, last, restarts;
  };

  // ---- Memories ----
  uint32_t imem32(uint32_t a) const {
    uint32_t v;
    std::memcpy(&v, &imem[a], 4);
    return v;
  }
  uint32_t dmem32(uint32_t a) const {
    uint32_t v;
    std::memcpy(&v, &dmem[a], 4);
    return v;
  }
  OtbnWide dmem256(uint32_t a) const {
    OtbnWide w;
    std::memcpy(w.q, &dmem[a], 32);
    return w;
  }
  static bool valid_pc(uint32_t pc) { return !(pc & 3) && pc < kImemBytes; }
  static bool valid32(uint32_t a) { return !(a & 3) && a < kDmemBytes; }
  static bool valid256(uint32_t a) { return !(a & 31) && a < kDmemBytes; }

  // ---- Random numbers ----
  bool seed_urnd() {
    uint32_t w;
    while (urnd_words_ < kUrndSeedWords && hooks.edn_urnd && hooks.edn_urnd(&w)) {
      urnd_seed_[urnd_words_++] = w;
    }
    if (urnd_words_ < kUrndSeedWords) return false;
    uint64_t s = 0;
    for (unsigned i = 0; i < kUrndSeedWords; ++i) s = splitmix(s ^ urnd_seed_[i]);
    urnd_s_[0] = splitmix(s);
    urnd_s_[1] = splitmix(urnd_s_[0]) | 1;
    urnd_seeded_ = true;
    urnd_advance();
    return true;
  }
  static uint64_t splitmix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  // xoroshiro128** step.
  uint64_t urnd_next() {
    uint64_t s0 = urnd_s_[0], s1 = urnd_s_[1];
    uint64_t r = ((s0 * 5) << 7 | (s0 * 5) >> 57) * 9;
    s1 ^= s0;
    urnd_s_[0] = (s0 << 24 | s0 >> 40) ^ s1 ^ (s1 << 16);
    urnd_s_[1] = s1 << 37 | s1 >> 27;
    return r;
  }
  void urnd_advance() {
    for (auto &q : urnd_.q) q = urnd_next();
  }
  // The RND cache, filled from edn_rnd; false while EDN has not delivered all eight words.
  bool rnd_fill() {
    uint32_t w;
    while (!rnd_valid_ && rnd_words_ < 8 && hooks.edn_rnd && hooks.edn_rnd(&w)) {
      rnd_.set_word(rnd_words_++, w);
    }
    if (rnd_words_ == 8) {
      rnd_valid_ = true;
      rnd_words_ = 0;
    }
    return rnd_valid_;
  }

  // ---- CSRs ----
  static bool valid_csr(uint32_t c) {
    switch (c) {
      case kFg0: case kFg1: case kFlags: case kRndPrefetch: case kUrndCtrl: case kKmacStatus:
      case kKmacCtrl: case kKmacCfg: case kKmacStrb: case kMaiCtrl: case kRnd: case kUrnd:
      case kUrndStatus: case kInsnCnt: case kMaiStatus:
        return true;
      default:
        return c >= kMod0 && c <= kMod7;
    }
  }
  // A CSR's value without the read's side effects.
  uint32_t csr_peek(uint32_t c) const {
    if (c >= kMod0 && c <= kMod7) return mod_.word(c - kMod0);
    switch (c) {
      case kFg0: return flags_[0];
      case kFg1: return flags_[1];
      case kFlags: return uint32_t(flags_[0]) | uint32_t(flags_[1]) << 4;
      case kKmacStatus: return kmac_status();
      case kKmacCfg: return kmac_cfg_ & 0x3f003fu;
      case kKmacStrb: return kmac_strb_;
      case kMaiCtrl: return mai_op_ << 1;
      case kRnd: return rnd_.word(0);
      case kUrnd: return urnd_.word(0);
      case kUrndStatus:
        // URND_CTRL_ENABLED, STOPPED, RESTORING, USED_WHILE_STOPPED; state width 177, restore
        // word width 32.
        return uint32_t(urnd_ctrl_enabled) | uint32_t(urnd_stopped_) << 1 |
               uint32_t(urnd_restoring_) << 2 | uint32_t(urnd_used_while_stopped_) << 3 |
               177u << 16 | 32u << 26;
      case kInsnCnt: return insn_cnt_;
      case kMaiStatus: return uint32_t(mai_busy()) | uint32_t(mai_input_ready()) << 1;
      default: return 0;
    }
  }
  uint32_t csr_read(uint32_t c) {
    uint32_t v = csr_peek(c);
    if (c == kRnd) rnd_valid_ = false;  // the read takes the cache
    return v;
  }
  void csr_write(uint32_t c, uint32_t v) {
    if (c >= kMod0 && c <= kMod7) {
      mod_.set_word(c - kMod0, v);
      return;
    }
    switch (c) {
      case kFg0: flags_[0] = v & 0xf; break;
      case kFg1: flags_[1] = v & 0xf; break;
      case kFlags:
        flags_[0] = v & 0xf;
        flags_[1] = (v >> 4) & 0xf;
        break;
      case kRndPrefetch: rnd_fill(); break;
      case kUrndCtrl: urnd_ctrl(v); break;
      case kKmacStatus:  // RSP_ERROR, CTRL_ERROR, MSG_WRITE_ERROR: W1C
        kmac_err_ &= ~(v & 0x1cu);
        break;
      case kKmacCtrl: kmac_cmd(v & 0x1f); break;
      case kKmacCfg:
        if (!kmac_ready()) kmac_err_ |= kKmacMsgWriteError;
        kmac_cfg_ = v & 0x3f003fu;
        break;
      case kKmacStrb:
        if (!kmac_ready()) kmac_err_ |= kKmacMsgWriteError;
        kmac_strb_ = v;
        break;
      case kMaiCtrl: mai_ctrl(v); break;
      default: break;  // RND, URND, URND_STATUS, INSN_CNT, MAI_STATUS: read-only
    }
  }

  // URND_CTRL: STOP, START (wins over STOP), RESTORE; ignored unless CTRL.urnd_ctrl_enabled.
  void urnd_ctrl(uint32_t v) {
    if (!urnd_ctrl_enabled) return;
    if (v & 2) {
      urnd_stopped_ = false;
    } else if (v & 1) {
      urnd_stopped_ = true;
      urnd_used_while_stopped_ = false;
    }
    if ((v & 4) && !urnd_restoring_) {
      urnd_restoring_ = true;
      urnd_restore_words_ = 0;
      urnd_restore_ = OtbnWide();
    }
  }

  // ---- WSRs ----
  OtbnWide wsr_read(uint32_t w) {
    switch (w) {
      case kWMod: return mod_;
      case kWRnd: rnd_valid_ = false; return rnd_;
      case kWUrnd: return urnd_;
      case kWAcc: return acc_;
      case kWKeyS0L: case kWKeyS0H: case kWKeyS1L: case kWKeyS1H: return key_[w - kWKeyS0L];
      case kWKmacS0: case kWKmacS1: return kmac_read(w == kWKmacS1);
      case kWMaiResS0: case kWMaiResS1: case kWMaiIn0S0: case kWMaiIn0S1: case kWMaiIn1S0:
      case kWMaiIn1S1:
        return mai_[w - kWMaiResS0];
      case kWUrndState: {
        OtbnWide s;
        if (urnd_ctrl_enabled) {
          s.q[0] = urnd_s_[0];
          s.q[1] = urnd_s_[1];
        }
        return s;
      }
      default: return OtbnWide();
    }
  }
  void wsr_write(uint32_t w, const OtbnWide &v) {
    switch (w) {
      case kWMod: mod_ = v; break;
      case kWAcc: acc_ = v; break;
      case kWKmacS0: case kWKmacS1:
        if (!kmac_ready()) kmac_err_ |= kKmacMsgWriteError;
        (w == kWKmacS0 ? kmac_s0_ : kmac_s1_) = v;
        break;
      case kWMaiResS0: case kWMaiResS1: case kWMaiIn0S0: case kWMaiIn0S1: case kWMaiIn1S0:
      case kWMaiIn1S1:
        mai_[w - kWMaiResS0] = v;
        break;
      case kWUrndState:
        // A restore word: the low 32 bits (fewer for the last), least significant word first.
        if (urnd_ctrl_enabled && urnd_restoring_) {
          urnd_restore_.set_word(urnd_restore_words_, urnd_restore_words_ == 5 ? v.word(0) & 0x1ffff
                                                                                 : v.word(0));
          if (++urnd_restore_words_ == 6) {
            urnd_s_[0] = urnd_restore_.q[0];
            urnd_s_[1] = urnd_restore_.q[1];
            urnd_restoring_ = false;
            if (!(urnd_s_[0] | urnd_s_[1])) fatal(kBadInternalState);  // the PRNG's fixed point
          }
        }
        break;
      default: break;  // RND, URND, KEY_*: read-only
    }
  }

  // ---- KMAC interface (theory of operation "KMAC interface") ----
  enum KmacState { kKIdle, kKWaitForMsg, kKReceiving, kKWaitForClose };
  enum : uint32_t { kKmacRspError = 4, kKmacCtrlError = 8, kKmacMsgWriteError = 16 };
  void kmac_reset();
  bool kmac_ready() const;
  uint32_t kmac_status() const;
  // KMAC_CFG: EN_XOF (0), STRENGTH (3:1), MODE (5:4), and their inverses (21:16). MODE: SHA3 0,
  // SHAKE 1, cSHAKE 2, KMAC 3; STRENGTH: L128 0, L224 1, L256 2, L384 3, L512 4.
  bool kmac_decode();
  void kmac_cmd(uint32_t cmd);
  void kmac_digest_start();
  // The next 64-bit digest beat into the low 64 bits of KMAC_DATA_S0/S1.
  void kmac_next_beat();
  OtbnWide kmac_read(bool s1);

  // ---- Masking accelerator interface (theory of operation "Masking accelerator interface") ----
  static bool mai_op_valid(uint32_t op);
  bool mai_busy() const;
  bool mai_input_ready() const;
  // MAI_SOFTWARE_ERROR conditions of a write to MAI_CTRL: reserved bits, START or a change of
  // OPERATION while busy, START with an invalid OPERATION.
  bool mai_ctrl_ok(uint32_t v) const;
  // The results land when BUSY clears.
  void mai_land();
  void mai_ctrl(uint32_t v);

  // ---- Execution ----
  static uint32_t sext(uint32_t v, unsigned bits) {
    return uint32_t(int32_t(v << (32 - bits)) >> (32 - bits));
  }
  static uint8_t mlz(bool c, const OtbnWide &r) {
    return uint8_t((c ? kC : 0) | (r.bit(255) ? kM : 0) | (r.bit(0) ? kL : 0) |
                   (r.zero() ? kZ : 0));
  }
  // The second operand of the shifting big-number instructions: shift_type (30) and shift_bits
  // (29:25, in bytes).
  static OtbnWide shifted(const OtbnWide &b, uint32_t w) {
    unsigned n = ((w >> 25) & 31) * 8;
    return (w >> 30) & 1 ? OtbnWide::shr(b, n) : OtbnWide::shl(b, n);
  }
  // 32-bit lanes of a 512-bit value as 16 words, word 0 least significant.
  static uint32_t bits_of(const uint32_t *w, unsigned lo, unsigned n) {
    uint64_t v = w[lo / 32];
    if (lo / 32 + 1 < 16) v |= uint64_t(w[lo / 32 + 1]) << 32;
    v >>= lo % 32;
    return uint32_t(v) & (n == 32 ? 0xffffffffu : (1u << n) - 1);
  }
  static void put_bits(uint32_t *w, unsigned lo, unsigned n, uint32_t v) {
    for (unsigned i = 0; i < n; ++i) {
      unsigned b = lo + i;
      w[b / 32] = (w[b / 32] & ~(1u << (b % 32))) | (((v >> i) & 1u) << (b % 32));
    }
  }

  Stop end(uint32_t e) {
    err_ |= e;
    ended_ = true;
    return Stop::kEnd;
  }

  // One instruction. Its effects are applied only when it raises no error.
  Stop step();

  // ---- State ----
  std::array<uint32_t, 32> gpr_{};
  std::array<OtbnWide, 32> wdr_{};
  std::vector<uint32_t> cstack_;
  std::vector<Loop> lstack_;
  uint8_t flags_[2] = {0, 0};
  OtbnWide mod_, acc_, rnd_, urnd_, key_[4], kmac_s0_, kmac_s1_;
  OtbnWide mai_[6];  // MAI_RES_S0/S1, MAI_IN0_S0/S1, MAI_IN1_S0/S1
  uint32_t mai_op_ = kMaiA2b;
  // The operation in progress: its results, the cycle BUSY clears, the cycle INPUT_READY sets.
  OtbnWide mai_res_[2];
  uint64_t mai_busy_until_ = 0, mai_ready_at_ = 0;
  bool mai_pending_ = false;
  bool key_valid_ = false;
  uint32_t pc_ = 0, err_ = 0, insn_cnt_ = 0;
  uint64_t cycles_ = 0;
  bool paused_ = false, ended_ = false;
  // RND cache and the words of the request in progress.
  bool rnd_valid_ = false;
  unsigned rnd_words_ = 0;
  // URND: the seed from EDN, the generator's state, URND_CTRL.
  uint32_t urnd_seed_[kUrndSeedWords] = {};
  unsigned urnd_words_ = 0;
  bool urnd_seeded_ = false;
  uint64_t urnd_s_[2] = {1, 2};
  bool urnd_stopped_ = false, urnd_used_while_stopped_ = false, urnd_restoring_ = false;
  unsigned urnd_restore_words_ = 0;
  OtbnWide urnd_restore_;
  // KMAC interface.
  KmacState kstate_ = kKIdle;
  uint32_t kmac_cfg_ = 0, kmac_strb_ = 0, kmac_err_ = 0;
  bool kmac_rsp_valid_ = false, kmac_read_s0_ = false, kmac_read_s1_ = false;
  bool kmac_no_more_msg_ = false, kmac_rejected_ = false;
  std::vector<uint8_t> kmac_msg_;
  unsigned kx_ = 0, kstrength_ = 0, kmode_ = 0, rate_ = 136, kmac_left_ = 0;
  uint8_t kdomain_ = 0x06;
  OtbnKeccak ksponge_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTBN_H_
