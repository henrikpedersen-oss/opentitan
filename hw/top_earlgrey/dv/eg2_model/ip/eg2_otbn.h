// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The OTBN processor of the EG2 SystemC model (decision 6: a C++ model, no Python): it runs the
// program software starts. Eg2Otbn (eg2_crypto_keymgr.h) is the block around it: registers,
// commands, the memories over the bus, timing, interrupt and alerts. This file needs no SystemC,
// so a program can also be run on its own.
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

namespace eg2_model {

// A 256-bit value (a WDR or WSR), q[0] the least significant 64 bits.
struct OtbnWide {
  uint64_t q[4] = {0, 0, 0, 0};

  uint32_t word(unsigned i) const { return uint32_t(q[i / 2] >> (32 * (i % 2))); }
  void set_word(unsigned i, uint32_t v) {
    unsigned s = 32 * (i % 2);
    q[i / 2] = (q[i / 2] & ~(uint64_t(0xffffffffu) << s)) | (uint64_t(v) << s);
  }
  bool zero() const { return !(q[0] | q[1] | q[2] | q[3]); }
  bool bit(unsigned i) const { return (q[i / 64] >> (i % 64)) & 1; }
  bool operator==(const OtbnWide &o) const {
    return q[0] == o.q[0] && q[1] == o.q[1] && q[2] == o.q[2] && q[3] == o.q[3];
  }

  static OtbnWide shl(const OtbnWide &a, unsigned n) {
    OtbnWide r;
    if (n >= 256) return r;
    unsigned w = n / 64, b = n % 64;
    for (int i = 3; i >= int(w); --i) {
      r.q[i] = a.q[i - w] << b;
      if (b && i - int(w) - 1 >= 0) r.q[i] |= a.q[i - w - 1] >> (64 - b);
    }
    return r;
  }
  static OtbnWide shr(const OtbnWide &a, unsigned n) {
    OtbnWide r;
    if (n >= 256) return r;
    unsigned w = n / 64, b = n % 64;
    for (unsigned i = 0; i + w < 4; ++i) {
      r.q[i] = a.q[i + w] >> b;
      if (b && i + w + 1 < 4) r.q[i] |= a.q[i + w + 1] << (64 - b);
    }
    return r;
  }
  // a + b + cin; *cout the carry out of bit 255.
  static OtbnWide add(const OtbnWide &a, const OtbnWide &b, unsigned cin, unsigned *cout) {
    OtbnWide r;
    unsigned __int128 c = cin;
    for (unsigned i = 0; i < 4; ++i) {
      c += (unsigned __int128)a.q[i] + b.q[i];
      r.q[i] = uint64_t(c);
      c >>= 64;
    }
    if (cout) *cout = unsigned(c);
    return r;
  }
  // a - b - bin; *bout 1 when the result is negative (bit 256 of the two's complement result).
  static OtbnWide sub(const OtbnWide &a, const OtbnWide &b, unsigned bin, unsigned *bout) {
    OtbnWide r;
    unsigned borrow = bin;
    for (unsigned i = 0; i < 4; ++i) {
      uint64_t d = a.q[i] - b.q[i];
      unsigned b1 = a.q[i] < b.q[i];
      uint64_t e = d - borrow;
      unsigned b2 = d < uint64_t(borrow);
      r.q[i] = e;
      borrow = b1 | b2;
    }
    if (bout) *bout = borrow;
    return r;
  }
  static bool less(const OtbnWide &a, const OtbnWide &b) {
    for (int i = 3; i >= 0; --i) {
      if (a.q[i] != b.q[i]) return a.q[i] < b.q[i];
    }
    return false;
  }
};

// A Keccak-f[1600] sponge (FIPS 202) for the KMAC interface, on digestpp's permutation (the KMAC
// block's DV reference, hw/ip/kmac/dv/dpi/vendor).
class OtbnKeccak {
 public:
  void reset(unsigned rate) {
    std::memset(a_, 0, sizeof a_);
    rate_ = rate;
    pos_ = 0;
  }
  void absorb(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      xor_byte(pos_++, p[i]);
      if (pos_ == rate_) {
        digestpp::detail::sha3_functions::transform<24>(a_);
        pos_ = 0;
      }
    }
  }
  void absorb(const std::vector<uint8_t> &v) { absorb(v.data(), v.size()); }
  // bytepad(x, rate) (NIST SP 800-185 2.3.3).
  void absorb_bytepad(const std::vector<uint8_t> &x) {
    absorb(left_encode(rate_));
    absorb(x);
    while (pos_) {
      uint8_t z = 0;
      absorb(&z, 1);
    }
  }
  void finish(uint8_t domain) {
    xor_byte(pos_, domain);
    xor_byte(rate_ - 1, 0x80);
    digestpp::detail::sha3_functions::transform<24>(a_);
    pos_ = 0;
  }
  // The next byte of output, permuting at the end of each rate.
  uint8_t squeeze() {
    if (pos_ == rate_) {
      digestpp::detail::sha3_functions::transform<24>(a_);
      pos_ = 0;
    }
    uint8_t b = uint8_t(a_[pos_ / 8] >> (8 * (pos_ % 8)));
    ++pos_;
    return b;
  }
  static std::vector<uint8_t> left_encode(uint64_t x) {
    std::vector<uint8_t> b;
    do {
      b.insert(b.begin(), uint8_t(x));
      x >>= 8;
    } while (x);
    b.insert(b.begin(), uint8_t(b.size()));
    return b;
  }
  static std::vector<uint8_t> right_encode(uint64_t x) {
    std::vector<uint8_t> b;
    do {
      b.insert(b.begin(), uint8_t(x));
      x >>= 8;
    } while (x);
    b.push_back(uint8_t(b.size()));
    return b;
  }
  static std::vector<uint8_t> encode_string(const std::vector<uint8_t> &s) {
    std::vector<uint8_t> e = left_encode(uint64_t(s.size()) * 8);
    e.insert(e.end(), s.begin(), s.end());
    return e;
  }

 private:
  void xor_byte(unsigned i, uint8_t b) { a_[i / 8] ^= uint64_t(b) << (8 * (i % 8)); }
  uint64_t a_[25] = {};
  unsigned rate_ = 136, pos_ = 0;
};

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
  void kmac_reset() {
    kstate_ = kKIdle;
    kmac_cfg_ = kmac_strb_ = kmac_err_ = 0;
    kmac_rsp_valid_ = kmac_read_s0_ = kmac_read_s1_ = false;
    kmac_no_more_msg_ = kmac_rejected_ = false;
    kmac_msg_.clear();
  }
  bool kmac_ready() const {
    return kstate_ == kKIdle || kstate_ == kKWaitForMsg || kstate_ == kKReceiving;
  }
  uint32_t kmac_status() const {
    return uint32_t(kmac_ready()) | uint32_t(kmac_rsp_valid_) << 1 | kmac_err_;
  }
  // KMAC_CFG: EN_XOF (0), STRENGTH (3:1), MODE (5:4), and their inverses (21:16). MODE: SHA3 0,
  // SHAKE 1, cSHAKE 2, KMAC 3; STRENGTH: L128 0, L224 1, L256 2, L384 3, L512 4.
  bool kmac_decode() {
    uint32_t c = kmac_cfg_;
    kx_ = c & 1;
    kstrength_ = (c >> 1) & 7;
    kmode_ = (c >> 4) & 3;
    if (((c >> 16) & 1) != (~c & 1) || ((c >> 17) & 7) != (~(c >> 1) & 7) ||
        ((c >> 20) & 3) != (~(c >> 4) & 3))
      return false;
    if (kstrength_ > 4) return false;
    if (kx_ && (kmode_ == 0 || kmode_ == 3)) return false;
    if (kmode_ == 0) return kstrength_ != 0;
    return kstrength_ == 0 || kstrength_ == 2;
  }
  void kmac_cmd(uint32_t cmd) {
    if (!cmd) return;
    enum { kStart = 1, kSend = 2, kProcess = 4, kDone = 8, kClose = 16 };
    uint32_t allowed = kstate_ == kKIdle         ? kStart
                       : kstate_ == kKWaitForMsg ? (kmac_no_more_msg_ ? kProcess : kSend | kProcess)
                       : kstate_ == kKReceiving  ? kDone
                                                 : kClose;
    if ((cmd & (cmd - 1)) || !(cmd & allowed)) {
      kmac_err_ |= kKmacCtrlError;  // the command is ignored
      return;
    }
    switch (cmd) {
      case kStart:
        kmac_rejected_ = !kmac_decode();
        kmac_msg_.clear();
        kmac_no_more_msg_ = false;
        kstate_ = kKWaitForMsg;
        break;
      case kSend:
        // Up to four 64-bit beats, least significant first, share 0 XOR share 1, KMAC_STRB a
        // byte strobe; an empty or partial beat ends the message.
        for (unsigned beat = 0; beat < 4; ++beat) {
          unsigned strb = (kmac_strb_ >> (8 * beat)) & 0xff;
          if (!strb) {
            kmac_no_more_msg_ = true;
            break;
          }
          uint64_t v = kmac_s0_.q[beat] ^ kmac_s1_.q[beat];
          unsigned n = 0;
          while (n < 8 && (strb >> n) & 1) ++n;
          for (unsigned i = 0; i < n; ++i) kmac_msg_.push_back(uint8_t(v >> (8 * i)));
          if (strb != 0xff) {
            kmac_no_more_msg_ = true;
            break;
          }
        }
        break;
      case kProcess:
        kstate_ = kKReceiving;
        if (kmac_rejected_) {
          kmac_err_ |= kKmacRspError;
          kmac_rsp_valid_ = true;
        } else {
          kmac_digest_start();
          kmac_next_beat();
        }
        break;
      case kDone:
        if (kmac_rejected_) kmac_err_ |= kKmacRspError;
        kstate_ = kKWaitForClose;
        kmac_rsp_valid_ = true;  // the final response
        break;
      case kClose:
        kmac_err_ = 0;
        kmac_rsp_valid_ = false;
        kstate_ = kKIdle;
        break;
    }
  }
  void kmac_digest_start() {
    static const unsigned kRate[] = {168, 144, 136, 104, 72};
    static const unsigned kBytes[] = {16, 28, 32, 48, 64};
    rate_ = kRate[kstrength_];
    ksponge_.reset(rate_);
    if (kmode_ == 2 || kmode_ == 3) {
      // cSHAKE: bytepad(encode_string(N) || encode_string(S)) from the KMAC block's PREFIX; with
      // N and S empty it is SHAKE (NIST SP 800-185 3.3). KMAC: N = "KMAC", the key,
      // right_encode(L).
      std::vector<uint8_t> prefix;
      if (kmode_ == 3) {
        prefix = OtbnKeccak::encode_string({'K', 'M', 'A', 'C'});
        std::vector<uint8_t> s = OtbnKeccak::encode_string({});
        prefix.insert(prefix.end(), s.begin(), s.end());
      } else if (hooks.kmac_prefix) {
        prefix = hooks.kmac_prefix();
      }
      bool empty = kmode_ == 2 && (prefix.size() < 4 || (prefix[0] == 1 && prefix[1] == 0 &&
                                                         prefix[2] == 1 && prefix[3] == 0));
      if (!empty) {
        ksponge_.absorb_bytepad(prefix);
        kdomain_ = 0x04;
      } else {
        kdomain_ = 0x1f;
      }
      if (kmode_ == 3) {
        std::vector<uint8_t> key = hooks.kmac_key ? hooks.kmac_key() : std::vector<uint8_t>(32, 0);
        ksponge_.absorb_bytepad(OtbnKeccak::encode_string(key));
      }
    } else {
      kdomain_ = kmode_ == 0 ? 0x06 : 0x1f;
    }
    ksponge_.absorb(kmac_msg_);
    if (kmode_ == 3) ksponge_.absorb(OtbnKeccak::right_encode(384));
    ksponge_.finish(kdomain_);
    // Bytes the digest has: SHA3 its size, KMAC 384 bits (fixed by the KMAC block); SHAKE and
    // cSHAKE one rate, or without end with EN_XOF.
    kmac_left_ = kmode_ == 0 ? kBytes[kstrength_] : kmode_ == 3 ? 48 : kx_ ? ~0u : rate_;
  }
  // The next 64-bit digest beat into the low 64 bits of KMAC_DATA_S0/S1.
  void kmac_next_beat() {
    if (!kmac_left_) {
      kmac_rsp_valid_ = false;
      return;
    }
    uint64_t v = 0;
    unsigned n = std::min(kmac_left_, 8u);
    for (unsigned i = 0; i < n; ++i) v |= uint64_t(ksponge_.squeeze()) << (8 * i);
    if (kmac_left_ != ~0u) kmac_left_ -= n;  // ~0u: XOF output without end
    uint64_t m = urnd_.q[0] ^ urnd_.q[2];
    kmac_s0_.q[0] = v ^ m;
    kmac_s1_.q[0] = m;
    kmac_rsp_valid_ = true;
    kmac_read_s0_ = kmac_read_s1_ = false;
  }
  OtbnWide kmac_read(bool s1) {
    OtbnWide v = s1 ? kmac_s1_ : kmac_s0_;
    if (kstate_ == kKReceiving && kmac_rsp_valid_ && !kmac_rejected_) {
      (s1 ? kmac_read_s1_ : kmac_read_s0_) = true;
      if (kmac_read_s0_ && kmac_read_s1_) kmac_next_beat();
    }
    return v;
  }

  // ---- Masking accelerator interface (theory of operation "Masking accelerator interface") ----
  static bool mai_op_valid(uint32_t op) {
    return op == kMaiA2b || op == kMaiB2a || op == kMaiSecAdd || op == kMaiSecAddMod;
  }
  bool mai_busy() const { return cycles_ < mai_busy_until_; }
  bool mai_input_ready() const { return cycles_ >= mai_ready_at_; }
  // MAI_SOFTWARE_ERROR conditions of a write to MAI_CTRL: reserved bits, START or a change of
  // OPERATION while busy, START with an invalid OPERATION.
  bool mai_ctrl_ok(uint32_t v) const {
    if (v & ~0x3fu) return false;
    if (mai_busy() && ((v & 1) || ((v >> 1) & 0x1f) != mai_op_)) return false;
    if ((v & 1) && !mai_op_valid((v >> 1) & 0x1f)) return false;
    return true;
  }
  // The results land when BUSY clears.
  void mai_land() {
    if (mai_pending_ && !mai_busy()) {
      mai_[0] = mai_res_[0];
      mai_[1] = mai_res_[1];
      mai_pending_ = false;
    }
  }
  void mai_ctrl(uint32_t v) {
    mai_op_ = (v >> 1) & 0x1f;
    if (!(v & 1)) return;
    unsigned vector_cycles = mai_op_ == kMaiSecAdd ? 14 : mai_op_ == kMaiB2a ? 23 : 22;
    mai_busy_until_ = cycles_ + 3 + vector_cycles;
    mai_ready_at_ = cycles_ + 9;
    mai_pending_ = true;
    // Eight 32-bit lanes; MOD[31:0] the modulus.
    uint32_t mod = uint32_t(mod_.q[0]);
    OtbnWide &r0 = mai_res_[0], &r1 = mai_res_[1];
    const OtbnWide &a0 = mai_[2], &a1 = mai_[3], &b0 = mai_[4], &b1 = mai_[5];
    OtbnWide mask = urnd_;
    for (unsigned i = 0; i < 8; ++i) {
      uint32_t x0 = a0.word(i), x1 = a1.word(i), y0 = b0.word(i), y1 = b1.word(i);
      uint32_t m = mask.word(i);
      if (mai_op_ == kMaiB2a) {
        // Boolean shares of a in, arithmetic shares out: (s0 + s1) mod MOD = a mod MOD.
        uint32_t a = x0 ^ x1;
        uint32_t s0 = mod ? m % mod : 0;
        uint32_t s1 = mod ? uint32_t((uint64_t(a % mod) + mod - s0) % mod) : a;
        r0.set_word(i, s0);
        r1.set_word(i, s1);
        continue;
      }
      uint32_t res;
      if (mai_op_ == kMaiSecAdd) {
        res = (x0 ^ x1) + (y0 ^ y1);
      } else if (mai_op_ == kMaiSecAddMod) {
        // The inputs carry 2^32 - MOD: a sum that does not overflow 32 bits gets MOD back.
        uint64_t t = uint64_t(x0 ^ x1) + (y0 ^ y1);
        res = (t >> 32) ? uint32_t(t) : uint32_t(t + mod);
      } else {  // A2B: arithmetic shares of a in, Boolean shares of a mod MOD out
        uint64_t t = uint64_t(x0) + x1;
        res = mod ? uint32_t(t % mod) : uint32_t(t);
      }
      r0.set_word(i, res ^ m);
      r1.set_word(i, m);
    }
  }

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
  Stop step() {
    if (ended_) return Stop::kEnd;
    if (paused_) return Stop::kWfi;
    mai_land();
    const uint32_t pc = pc_;
    const uint32_t w = imem32(pc);
    const uint32_t op = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31, rs1 = (w >> 15) & 31,
                   rs2 = (w >> 20) & 31;

    // URND advances every cycle the processor runs; stopped, only when it is read (URND_CTRL).
    if (urnd_stopped_) {
      bool reads = (op == 0x73 && (f3 == 1 || f3 == 2) && (w >> 20) == kUrnd) ||
                   (op == 0x0b && f3 == 7 && !(w >> 31) && ((w >> 20) & 0xff) == kWUrnd);
      if (reads) {
        urnd_advance();
        urnd_used_while_stopped_ = true;
      }
    } else {
      urnd_advance();
    }

    // The GPR reads (x1 pops the call stack once, however often it is read) and writes (x1
    // pushes) of the instruction, applied at its end.
    bool pop = false, cs_err = false;
    auto rg = [&](uint32_t r) -> uint32_t {
      if (r == 0) return 0;
      if (r == 1) {
        if (cstack_.empty()) {
          cs_err = true;
          return 0;
        }
        pop = true;
        return cstack_.back();
      }
      return gpr_[r];
    };
    struct GprWrite {
      uint32_t r, v;
    } gw[2];
    unsigned ngw = 0;
    auto wg = [&](uint32_t r, uint32_t v) { gw[ngw++] = {r, v}; };

    uint32_t err = 0;
    uint32_t next = pc + 4;
    unsigned cycles = 1;
    bool control = false;  // a jump, branch or loop instruction ("may not appear as the last
                           // instruction in a loop")
    bool wfi = false, ecall = false;
    // Effects beyond the GPRs, WDRs and flags, applied when the instruction succeeds.
    enum { kNoEffect, kStore32, kStore256, kCsrAccess, kLoopPush, kSetAcc, kAccUrnd, kWsrRead,
           kWsrWrite } effect = kNoEffect;
    uint32_t e_addr = 0, e_val = 0, e_csr = 0;
    bool e_reads = false, e_writes = false;
    OtbnWide e_wide;
    Loop e_loop{0, 0, 0};
    int wdr_dst = -1;
    OtbnWide wdr_val;
    int fg_dst = -1;
    uint8_t fg_val = 0;

    auto illegal = [&]() { err |= kIllegalInsn; };

    switch (op) {
      case 0x33: {  // OP: ADD SUB SLL SRL SRA AND OR XOR
        uint32_t f7 = w >> 25;
        uint32_t a = rg(rs1), b = rg(rs2);
        uint32_t r = 0;
        if (f7 == 0x00 && f3 == 0) r = a + b;
        else if (f7 == 0x20 && f3 == 0) r = a - b;
        else if (f7 == 0x00 && f3 == 1) r = a << (b & 31);
        else if (f7 == 0x00 && f3 == 5) r = a >> (b & 31);
        else if (f7 == 0x20 && f3 == 5) r = uint32_t(int32_t(a) >> (b & 31));
        else if (f7 == 0x00 && f3 == 7) r = a & b;
        else if (f7 == 0x00 && f3 == 6) r = a | b;
        else if (f7 == 0x00 && f3 == 4) r = a ^ b;
        else { illegal(); break; }
        if (cs_err) { err |= kCallStack; break; }
        wg(rd, r);
        break;
      }
      case 0x13: {  // OP-IMM: ADDI ANDI ORI XORI SLLI SRLI SRAI
        uint32_t imm = sext(w >> 20, 12);
        uint32_t r = 0;
        bool shift = f3 == 1 || f3 == 5;
        if (shift) {
          // Is: arithmetic (30); bits 31 and 29:25 zero.
          bool arith = (w >> 30) & 1;
          if ((w & 0xbe000000u) || (arith && f3 == 1)) { illegal(); break; }
        } else if (f3 == 2 || f3 == 3) {
          illegal();
          break;
        }
        uint32_t a = rg(rs1);
        if (cs_err) { err |= kCallStack; break; }
        unsigned sh = rs2;
        switch (f3) {
          case 0: r = a + imm; break;
          case 7: r = a & imm; break;
          case 6: r = a | imm; break;
          case 4: r = a ^ imm; break;
          case 1: r = a << sh; break;
          default: r = (w >> 30) & 1 ? uint32_t(int32_t(a) >> sh) : a >> sh; break;
        }
        wg(rd, r);
        break;
      }
      case 0x37:  // LUI
        wg(rd, w & 0xfffff000u);
        break;
      case 0x03: {  // LW
        if (f3 != 2) { illegal(); break; }
        uint32_t a = rg(rs1);
        if (cs_err) { err |= kCallStack; break; }
        uint32_t addr = a + sext(w >> 20, 12);
        if (!valid32(addr)) { err |= kBadDataAddr; break; }
        wg(rd, dmem32(addr));
        cycles = 2;
        break;
      }
      case 0x23: {  // SW
        if (f3 != 2) { illegal(); break; }
        uint32_t a = rg(rs1), v = rg(rs2);
        uint32_t addr = a + sext(((w >> 25) << 5) | ((w >> 7) & 31), 12);
        if (cs_err) err |= kCallStack;
        if (!valid32(addr) && !(cs_err && rs1 == 1)) err |= kBadDataAddr;
        if (err) break;
        effect = kStore32;
        e_addr = addr;
        e_val = v;
        break;
      }
      case 0x63: {  // BEQ BNE
        if (f3 > 1) { illegal(); break; }
        control = true;
        cycles = 2;
        uint32_t a = rg(rs1), b = rg(rs2);
        if (cs_err) { err |= kCallStack; break; }
        uint32_t off = sext(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3f) << 5) |
                                (((w >> 8) & 0xf) << 1),
                            13);
        if ((a == b) == (f3 == 0)) {
          if (!valid_pc(pc + off)) { err |= kBadInsnAddr; break; }
          next = pc + off;
        }
        break;
      }
      case 0x6f: {  // JAL
        control = true;
        cycles = 2;
        uint32_t off = sext(((w >> 31) << 20) | (((w >> 12) & 0xff) << 12) |
                                (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3ff) << 1),
                            21);
        wg(rd, pc + 4);
        if (!valid_pc(pc + off)) err |= kBadInsnAddr;
        next = pc + off;
        break;
      }
      case 0x67: {  // JALR
        if (f3 != 0) { illegal(); break; }
        control = true;
        cycles = 2;
        uint32_t a = rg(rs1);
        if (cs_err) { err |= kCallStack; break; }
        uint32_t t = a + sext(w >> 20, 12);
        wg(rd, pc + 4);
        if (!valid_pc(t)) err |= kBadInsnAddr;
        next = t;
        break;
      }
      case 0x73: {  // SYSTEM: CSRRS CSRRW ECALL WFI
        uint32_t csr = w >> 20;
        if (f3 == 0) {
          if (rd || rs1) illegal();
          else if (csr == 0) ecall = true;
          else if (csr == 0x105) {
            if (!wfi_enabled) illegal();
            else wfi = true;
          } else {
            illegal();
          }
          break;
        }
        if (f3 != 1 && f3 != 2) { illegal(); break; }
        if (!valid_csr(csr)) { illegal(); break; }
        uint32_t s = rg(rs1);
        if (cs_err) { err |= kCallStack; break; }
        bool reads = f3 == 2 || rd != 0;  // CSRRW with grd = x0 does not read the CSR
        bool writes = f3 == 1 || rs1 != 0;
        if (reads && csr == kRnd && !rnd_fill()) return Stop::kWaitEdn;  // stall for EDN
        uint32_t nv = f3 == 1 ? s : csr_peek(csr) | s;
        if (writes && csr == kMaiCtrl && !mai_ctrl_ok(nv)) { err |= kMaiError; break; }
        if (reads) wg(rd, csr_peek(csr));
        effect = kCsrAccess;
        e_csr = csr;
        e_reads = reads;
        e_writes = writes;
        e_val = nv;
        break;
      }
      case 0x7b: {  // custom3: LOOP LOOPI BN.AND BN.OR BN.XOR BN.NOT BN.RSHI
        if (f3 == 0 || f3 == 1) {
          control = true;
          uint32_t iters = f3 == 0 ? rg(rs1) : (rs1 << 5) | rd;
          if (cs_err) { err |= kCallStack; break; }
          if (!iters || lstack_.size() == kStackDepth) { err |= kLoop; break; }
          uint32_t body = (w >> 20) + 1;
          effect = kLoopPush;
          e_loop = Loop{pc + 4, pc + 4 * body, iters - 1};
          break;
        }
        const OtbnWide &a = wdr_[rs1], &b = wdr_[rs2];
        if (f3 == 3 || f3 == 7) {  // BN.RSHI: {wrs1, wrs2} >> imm
          unsigned imm = ((w >> 25) << 1) | ((w >> 14) & 1);
          wdr_val = imm ? OtbnWide{} : b;
          if (imm) {
            OtbnWide lo = OtbnWide::shr(b, imm), hi = OtbnWide::shl(a, 256 - imm);
            for (unsigned i = 0; i < 4; ++i) wdr_val.q[i] = lo.q[i] | hi.q[i];
          }
          wdr_dst = int(rd);
          break;
        }
        unsigned fg = w >> 31;
        if (f3 == 5) {  // BN.NOT: wrs in 24:20
          OtbnWide s = shifted(wdr_[rs2], w);
          for (unsigned i = 0; i < 4; ++i) wdr_val.q[i] = ~s.q[i];
        } else {
          OtbnWide s = shifted(b, w);
          for (unsigned i = 0; i < 4; ++i) {
            wdr_val.q[i] = f3 == 2 ? a.q[i] & s.q[i] : f3 == 4 ? a.q[i] | s.q[i] : a.q[i] ^ s.q[i];
          }
        }
        wdr_dst = int(rd);
        fg_dst = int(fg);
        fg_val = mlz(flags_[fg] & kC, wdr_val);
        break;
      }
      case 0x2b: {  // custom1: BN.ADD BN.SUB BN.ADDC BN.SUBB BN.ADDI BN.SUBI BN.ADDM BN.SUBM
        const OtbnWide &a = wdr_[rs1], &b = wdr_[rs2];
        unsigned fg = w >> 31;
        unsigned c = 0;
        if (f3 <= 3) {
          OtbnWide s = shifted(b, w);
          unsigned cin = (f3 & 2) ? (flags_[fg] & kC) : 0;
          wdr_val = f3 & 1 ? OtbnWide::sub(a, s, cin, &c) : OtbnWide::add(a, s, cin, &c);
        } else if (f3 == 4) {
          OtbnWide imm;
          imm.q[0] = (w >> 20) & 0x3ff;
          wdr_val = (w >> 30) & 1 ? OtbnWide::sub(a, imm, 0, &c) : OtbnWide::add(a, imm, 0, &c);
        } else if (f3 == 5) {
          if ((w >> 30) & 1) {  // BN.SUBM: add MOD when negative
            unsigned neg;
            wdr_val = OtbnWide::sub(a, b, 0, &neg);
            if (neg) wdr_val = OtbnWide::add(wdr_val, mod_, 0, nullptr);
          } else {  // BN.ADDM: subtract MOD when the WLEN+1-bit sum is at least MOD
            unsigned carry;
            wdr_val = OtbnWide::add(a, b, 0, &carry);
            if (carry || !OtbnWide::less(wdr_val, mod_)) {
              wdr_val = OtbnWide::sub(wdr_val, mod_, 0, nullptr);
            }
          }
          wdr_dst = int(rd);
          break;
        } else {
          illegal();
          break;
        }
        wdr_dst = int(rd);
        fg_dst = int(fg);
        fg_val = mlz(c, wdr_val);
        break;
      }
      case 0x3b: {  // custom2: BN.MULQACC, .WO, .SO
        unsigned fg = w >> 31, so = (w >> 30) & 1, wb0 = (w >> 29) & 1;
        unsigned qs2 = (w >> 27) & 3, qs1 = (w >> 25) & 3, sh = ((w >> 13) & 3) * 64;
        unsigned __int128 p = (unsigned __int128)wdr_[rs1].q[qs1] * wdr_[rs2].q[qs2];
        OtbnWide prod;
        prod.q[0] = uint64_t(p);
        prod.q[1] = uint64_t(p >> 64);
        OtbnWide acc = (w >> 12) & 1 ? OtbnWide() : acc_;
        acc = OtbnWide::add(acc, OtbnWide::shl(prod, sh), 0, nullptr);
        effect = kSetAcc;
        e_wide = acc;
        if (!so && wb0) {
          wdr_dst = int(rd);
          wdr_val = acc;
          fg_dst = int(fg);
          fg_val = mlz(flags_[fg] & kC, acc);
        } else if (so) {
          // The low half-word goes to half wb0 of wrd, ACC keeps the high half.
          OtbnWide lo = acc, hi = OtbnWide::shr(acc, 128);
          lo.q[2] = lo.q[3] = 0;
          wdr_val = wdr_[rd];
          wdr_val.q[2 * wb0] = lo.q[0];
          wdr_val.q[2 * wb0 + 1] = lo.q[1];
          wdr_dst = int(rd);
          uint8_t f = flags_[fg];
          if (wb0) {
            f = uint8_t((f & (kC | kL)) | ((lo.q[1] >> 63) ? kM : 0) |
                        ((f & kZ) && lo.zero() ? kZ : 0));
          } else {
            f = uint8_t((f & (kC | kM)) | ((lo.q[0] & 1) ? kL : 0) | (lo.zero() ? kZ : 0));
          }
          fg_dst = int(fg);
          fg_val = f;
          e_wide = hi;
        }
        break;
      }
      case 0x0b: {  // custom0: BN.SEL BN.CMP BN.CMPB BN.LID BN.SID BN.MOV BN.MOVR BN.WSRR BN.WSRW
        unsigned fg = w >> 31;
        switch (f3) {
          case 0: {  // BN.SEL: flag (26:25) C, M, L, Z
            unsigned flag = (w >> 25) & 3;
            wdr_val = (flags_[fg] >> flag) & 1 ? wdr_[rs1] : wdr_[rs2];
            wdr_dst = int(rd);
            break;
          }
          case 1:
          case 3: {  // BN.CMP, BN.CMPB
            unsigned c;
            unsigned bin = f3 == 3 ? flags_[fg] & kC : 0;
            OtbnWide r = OtbnWide::sub(wdr_[rs1], shifted(wdr_[rs2], w), bin, &c);
            fg_dst = int(fg);
            fg_val = mlz(c, r);
            break;
          }
          case 4:
          case 5: {  // BN.LID, BN.SID: offset {11:9, 31:25} << 5; grs1_inc (8), grd/grs2_inc (7)
            bool inc1 = (w >> 8) & 1, inc2 = (w >> 7) & 1;
            if (inc1 && inc2) { illegal(); break; }
            uint32_t off = sext((((w >> 9) & 7) << 7) | (w >> 25), 10) << 5;
            uint32_t a = rg(rs1);
            uint32_t addr = a + off;
            uint32_t g = rg(rs2);
            bool bad1 = cs_err && rs1 == 1, bad2 = cs_err && rs2 == 1;
            if (cs_err) err |= kCallStack;
            if (g > 31 && !bad2) err |= kIllegalInsn;
            if (!valid256(addr) && !bad1) err |= kBadDataAddr;
            if (err) break;
            if (inc2) wg(rs2, g + 1);
            if (inc1) wg(rs1, a + 32);
            cycles = 2;
            if (f3 == 4) {
              wdr_dst = int(g & 31);
              wdr_val = dmem256(addr);
            } else {
              effect = kStore256;
              e_addr = addr;
              e_wide = wdr_[g & 31];
            }
            break;
          }
          case 6: {
            if (!(w >> 31)) {  // BN.MOV
              wdr_val = wdr_[rs1];
              wdr_dst = int(rd);
              break;
            }
            // BN.MOVR: grd (24:20), grs (19:15), grs_inc (9), grd_inc (7)
            bool incs = (w >> 9) & 1, incd = (w >> 7) & 1;
            if (incs && incd) { illegal(); break; }
            uint32_t d = rg(rs2), s = rg(rs1);
            bool badd = cs_err && rs2 == 1, bads = cs_err && rs1 == 1;
            if (cs_err) err |= kCallStack;
            if (d > 31 && !badd) err |= kIllegalInsn;
            if (s > 31 && !bads) err |= kIllegalInsn;
            if (err) break;
            if (incd) wg(rs2, d + 1);
            if (incs) wg(rs1, s + 1);
            wdr_val = wdr_[s & 31];
            wdr_dst = int(d & 31);
            cycles = 2;
            break;
          }
          case 7: {  // BN.WSRR / BN.WSRW: wsr (27:20)
            uint32_t wsr = (w >> 20) & 0xff;
            if (wsr >= kNumWsrs) { illegal(); break; }
            if (!(w >> 31)) {
              if (wsr == kWRnd && !rnd_fill()) return Stop::kWaitEdn;  // stall for EDN
              if (wsr >= kWKeyS0L && wsr <= kWKeyS1H && !key_valid_) { err |= kKeyInvalid; break; }
              effect = kWsrRead;
            } else {
              // The MAI's input WSRs take writes only while INPUT_READY is set.
              if (wsr >= kWMaiIn0S0 && wsr <= kWMaiIn1S1 && !mai_input_ready()) {
                err |= kMaiError;
                break;
              }
              effect = kWsrWrite;
              e_wide = wdr_[rs1];
            }
            e_csr = wsr;
            break;
          }
          default:
            illegal();
            break;
        }
        break;
      }
      case 0x5b: {  // custom4: the vector instructions
        unsigned elen = (w >> 25) & 3;
        const OtbnWide &a = wdr_[rs1], &b = wdr_[rs2];
        switch (f3) {
          case 0: {  // BN.ADDV(M), BN.SUBV(M): .8s only
            if (elen != 0) { illegal(); break; }
            bool sub = (w >> 30) & 1, m = (w >> 28) & 1;
            uint32_t q = uint32_t(mod_.q[0]);
            for (unsigned i = 0; i < 8; ++i) {
              uint32_t x = a.word(i), y = b.word(i), r;
              if (!sub) {
                uint64_t t = uint64_t(x) + y;
                if (m && t >= q) t -= q;
                r = uint32_t(t);
              } else {
                int64_t t = int64_t(x) - int64_t(y);
                if (m && t < 0) t += q;
                r = uint32_t(t);
              }
              wdr_val.set_word(i, r);
            }
            wdr_dst = int(rd);
            break;
          }
          case 3:
          case 4: {  // BN.MULV(L), BN.MULVM(L): lane (30:28), use_lane (27); .8s only
            if (elen != 0) { illegal(); break; }
            bool lane = (w >> 27) & 1;
            uint32_t l = (w >> 28) & 7;
            uint64_t q = uint32_t(mod_.q[0]), mu = uint32_t(mod_.q[0] >> 32);
            for (unsigned i = 0; i < 8; ++i) {
              uint64_t x = a.word(i), y = b.word(lane ? l : i);
              uint64_t c = x * y;
              uint32_t r;
              if (f3 == 3) {
                r = uint32_t(c);
              } else {
                // Montgomery without the final subtraction: [c + [[c]_d * mu]_d * q]^d, d = 32.
                uint64_t t = uint32_t(uint32_t(c) * mu);
                unsigned __int128 s = (unsigned __int128)c + (unsigned __int128)t * q;
                r = uint32_t(uint64_t(s >> 32));
              }
              wdr_val.set_word(i, r);
            }
            wdr_dst = int(rd);
            cycles = f3 == 3 ? 4 : 12;
            // ACC assembles the result and is cleared with URND at the end.
            effect = kAccUrnd;
            break;
          }
          case 5: {  // BN.TRN1, BN.TRN2: odd (30); .8s, .4d, .2q
            if (elen == 3) { illegal(); break; }
            unsigned ew = 1u << elen;  // element size in 32-bit words
            unsigned odd = (w >> 30) & 1;
            for (unsigned e = 0; e < 8 / ew; e += 2) {
              for (unsigned k = 0; k < ew; ++k) {
                wdr_val.set_word(e * ew + k, a.word((e + odd) * ew + k));
                wdr_val.set_word((e + 1) * ew + k, b.word((e + odd) * ew + k));
              }
            }
            wdr_dst = int(rd);
            break;
          }
          case 6: {  // BN.PACK, BN.UNPK: is_pack (30), shift (28:27) * 64
            unsigned sh = ((w >> 27) & 3) * 64;
            uint32_t v[16] = {}, r[16] = {};
            if ((w >> 30) & 1) {
              // {64'd0, dense(wrs1), dense(wrs2), 64'd0} >> shift
              for (unsigned i = 0; i < 8; ++i) {
                put_bits(v, 64 + 24 * i, 24, b.word(i) & 0xffffff);
                put_bits(v, 256 + 24 * i, 24, a.word(i) & 0xffffff);
              }
              for (unsigned i = 0; i + sh / 32 < 16; ++i) r[i] = v[i + sh / 32];
              for (unsigned i = 0; i < 8; ++i) wdr_val.set_word(i, r[i]);
            } else {
              // {wrs1, wrs2} >> shift, eight 24-bit elements zero-extended to 32 bits
              for (unsigned i = 0; i < 8; ++i) {
                v[i] = b.word(i);
                v[8 + i] = a.word(i);
              }
              for (unsigned i = 0; i + sh / 32 < 16; ++i) r[i] = v[i + sh / 32];
              for (unsigned i = 0; i < 8; ++i) wdr_val.set_word(i, bits_of(r, 24 * i, 24));
            }
            wdr_dst = int(rd);
            break;
          }
          case 7: {  // BN.SHV: shift_type (30), shift_bits (19:15), wrs (24:20); .8s only
            if (elen != 0) { illegal(); break; }
            unsigned n = rs1;
            bool right = (w >> 30) & 1;
            for (unsigned i = 0; i < 8; ++i) {
              uint32_t x = wdr_[rs2].word(i);
              wdr_val.set_word(i, right ? x >> n : x << n);
            }
            wdr_dst = int(rd);
            break;
          }
          default:
            illegal();
            break;
        }
        break;
      }
      default:
        illegal();
        break;
    }

    // A jump, branch or loop instruction may not end a loop body (LOOP, LOOPI).
    bool loop_end = !lstack_.empty() && pc == lstack_.back().last;
    if (loop_end && control) err |= kLoop;
    // x1 written: a push onto the call stack, which must not be full unless the instruction
    // popped it too.
    unsigned pushes = 0;
    for (unsigned i = 0; i < ngw; ++i) pushes += gw[i].r == 1;
    if (pushes && cstack_.size() - (pop ? 1 : 0) + pushes > kStackDepth) err |= kCallStack;
    if (!err && loop_end && lstack_.back().restarts) next = lstack_.back().start;
    if (!err && !ecall && !valid_pc(next)) err |= kBadInsnAddr;
    if (err) return end(err);

    // The instruction succeeds: apply its effects.
    if (pop) cstack_.pop_back();
    for (unsigned i = 0; i < ngw; ++i) {
      if (gw[i].r == 1) cstack_.push_back(gw[i].v);
      else if (gw[i].r) gpr_[gw[i].r] = gw[i].v;
    }
    if (wdr_dst >= 0) wdr_[wdr_dst] = wdr_val;
    if (fg_dst >= 0) flags_[fg_dst] = fg_val;
    switch (effect) {
      case kStore32: std::memcpy(&dmem[e_addr], &e_val, 4); break;
      case kStore256: std::memcpy(&dmem[e_addr], e_wide.q, 32); break;
      case kCsrAccess:
        if (e_reads) csr_read(e_csr);
        if (e_writes) csr_write(e_csr, e_val);
        break;
      case kLoopPush: lstack_.push_back(e_loop); break;
      case kSetAcc: acc_ = e_wide; break;
      case kAccUrnd: acc_ = urnd_; break;
      case kWsrRead: wdr_[rd] = wsr_read(e_csr); break;
      case kWsrWrite: wsr_write(e_csr, e_wide); break;
      case kNoEffect: break;
    }
    if (insn_cnt_ != 0xffffffffu) ++insn_cnt_;
    cycles_ += cycles;
    if (ecall) {
      ended_ = true;
      return Stop::kEnd;
    }
    if (ended_) return Stop::kEnd;  // a fatal error the instruction's effect raised
    if (loop_end) {
      Loop &l = lstack_.back();
      if (l.restarts) --l.restarts;
      else lstack_.pop_back();
    }
    pc_ = next;
    if (wfi) {
      paused_ = true;
      return Stop::kWfi;
    }
    return Stop::kBudget;
  }

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
