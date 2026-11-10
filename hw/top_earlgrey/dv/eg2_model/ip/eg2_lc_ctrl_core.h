// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The life cycle controller's behaviour as the EG2 model has it, in plain C++ (no SystemC), shared by
// eg2sim (Eg2LcCtrl in eg2_mem_otp.h, a thin adapter) and a self-test
// (dpi/eg2_lc_ctrl_core_test.cc). The core reaches OTP, KMAC, the NVM controller, clkmgr and the
// alert handler only through Eg2LcCtrlPort.
//
// From the documents only, never the RTL: hw/ip/lc_ctrl/doc (theory_of_operation.md "ToO",
// programmers_guide.md "PG", registers.md "REG", interfaces.md "IF", the encoding, counter,
// function and access signal tables) and doc/security/specs/device_life_cycle/README.md ("DLC").
// Untimed (model decision 2026-11-08): a transition completes at once.
//
// Initialisation (ToO "Power Up Sequence"): the state and count are decoded from OTP's buffered
// LIFE_CYCLE words (lc_ctrl_encoding_table.md, lc_ctrl_counter_table.md). A word vector that
// matches no state or no count is INVALID (ToO "Any decoding that does not fall into the table above
// is considered INVALID"; the count then reads 31, REG LC_TRANSITION_CNT): STATUS.STATE_ERROR and
// fatal_state_error (REG STATUS.STATE_ERROR). A count of 24 reads SCRAP (REG LC_TRANSITION_CNT).
// LC_ID_STATE is PERSONALIZED once SECRET2 is locked (ToO "ID State of the Device").
//
// Transition interface (ToO "Life Cycle Request Interface", REG). One hardware mutex shared by
// software and the TAP: CLAIM_TRANSITION_IF written with kMuBi8True by a side claims it if it is
// free and reads back kMuBi8True to that side; any other value written by the holder releases it.
// The other side reads 0 (PG step 3: "If the value read back equals to 0, the mutex has already
// been claimed by the other interface"); a free mutex reads its reset value 0x69. All transition
// registers but STATUS read 0 to a side that does not hold the mutex, and only the holder writes them
// (TRANSITION_REGWEN). CLAIM_TRANSITION_IF_REGWEN gates software's claim only.
//
// TRANSITION_CMD.START (ToO "Life Cycle State Controller"): the count is incremented in OTP first
// (ToO "Transition Counter Limits": "FIRST increments the token count, and THEN checks"); then the
// transition is checked against DLC's transition table (TRANSITION_ERROR otherwise); a conditional
// one needs its token's OTP partition locked and the cSHAKE128 hash of the supplied token equal to
// the hashed token in OTP (TOKEN_ERROR otherwise; ToO "Conditional Transitions", "Token Hashing
// Mechanism"); RMA asks the NVM controller to wipe (NVM_RMA_ERROR if it fails); then the new state
// is programmed (an OTP error: OTP_ERROR and fatal_prog_error). The controller always ends in
// POST_TRANSITION (ToO "Post Transition Handling"): LC_STATE reads POST_TRANSITION, the count 31,
// every decoded output is off and HW_DEBUG_CLR on until reset. A count already at 24:
// TRANSITION_COUNT_ERROR without an OTP write. From INVALID or ESCALATE: TRANSITION_ERROR without an
// OTP write (DLC: INVALID allows no transitions; ESCALATE "behaves like a virtual SCRAP"; model
// choice to report it). The states only move forward: the table and the A->B / C->D encodings
// (DLC "when OTP states are transitioned from A to B, we cannot go back to A from B") never let a
// state go back, the count only increases, and SCRAP allows nothing.
//
// Volatile RAW unlock (PG "Volatile RAW -> TEST_UNLOCKED0", REG TRANSITION_CTRL): only when the
// configuration enables it (SecVolatileRawUnlockEn, a test-chip parameter); then the written token is
// compared with the hashed RAW unlock token directly ("the token is NOT passed through KMAC"). A
// success moves the decoded state to TEST_UNLOCKED0 without OTP write, count increment or
// POST_TRANSITION, and further commands are accepted. A failure (TRANSITION_ERROR for a state other
// than RAW or a target other than TEST_UNLOCKED0, TOKEN_ERROR for a wrong token) is terminal like
// every error (REG STATUS: "all errors are terminal and require a reset cycle").
//
// EXT_CLOCK_EN (ToO "CLK_BYP_REQ", REG TRANSITION_CTRL): set by the holder in RAW, TEST* or RMA, it
// asks clkmgr for the external clock at once; STATUS.EXT_CLOCK_SWITCHED when clkmgr acknowledges.
//
// Escalation (ToO "Security Escalation"): either escalation input moves the state to ESCALATE until
// reset. The same document's "Escalation Handling" says the first channel only asserts
// lc_escalate_en (status/rtl_tickets.md); the model follows "Security Escalation".
//
// Broadcast signals: lc_ctrl_function_signals_table.md and lc_ctrl_access_signals_table.md with
// ToO's notes on the Y* entries. NVM_DEBUG_EN is off in TEST_UNLOCKED7 (ToO "NVM_DEBUG_EN").
// CREATOR_SEED_SW_RW_EN: RMA, or DEV/PROD/PROD_END while SECRET2 is unlocked. SEED_HW_RD_EN:
// DEV/PROD/PROD_END/RMA once SECRET2 is locked (ToO "SEED_HW_RD_EN" and "OTP Accessibility Summary":
// "only becomes active if SECRET2_DIGEST has a nonzero value in DEV, PROD, PROD_END and RMA"; the
// access table marks RMA plain "Y": status/rtl_tickets.md). ESCALATE_EN in SCRAP, INVALID and
// ESCALATE. CHECK_BYP_EN from a transition command until reset.

#ifndef EG2_MODEL_IP_EG2_LC_CTRL_CORE_H_
#define EG2_MODEL_IP_EG2_LC_CTRL_CORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"

// ---- Life-cycle encodings ----
// The 16-bit OTP words of the state (A0..A19, B0..B19) and count (C0..C23, D0..D23): netlist
// constants the silicon creator generates (ToO "Life Cycle Manufacturing State Encodings"); these
// are the values of the generated lc_ctrl_state_pkg.sv. Copy them again if it is regenerated.
inline constexpr uint16_t kLcA[20] = {0xa352, 0x7181, 0x39ca, 0x406b, 0x7070, 0x6a88, 0x960e,
                                      0x79e0, 0x5cd4, 0x9489, 0xc6e5, 0x97e4, 0xb403, 0x5e90,
                                      0x8c7c, 0xaab0, 0x24cd, 0x5618, 0xdb21, 0xc949};
inline constexpr uint16_t kLcB[20] = {0xb35a, 0xfbd1, 0x3ddb, 0x40ff, 0xfcf2, 0xfb9e, 0xf6ff,
                                      0x7df5, 0xfdd4, 0xb5a9, 0xc7fd, 0xbfed, 0xb763, 0xfe9d,
                                      0xeefc, 0xaffc, 0x75ef, 0x5ebd, 0xdf3f, 0xf97d};
inline constexpr uint16_t kLcC[24] = {0x3399, 0x2e0d, 0x10f5, 0x386a, 0xf4b0, 0x463d,
                                      0x6612, 0x2945, 0x3230, 0x418c, 0x7bb0, 0x11d8,
                                      0x3c40, 0xe92a, 0x14ec, 0x5622, 0xa506, 0x131b,
                                      0x6883, 0x3938, 0x28d2, 0x07c8, 0x4c44, 0x13e9};
inline constexpr uint16_t kLcD[24] = {0x73fb, 0xaf6f, 0xb1fd, 0x387f, 0xfef7, 0xe6bd,
                                      0x7732, 0xe97f, 0xfaf4, 0x59fc, 0x7ff8, 0xd5db,
                                      0xfd68, 0xef7b, 0xb6fe, 0x567b, 0xafc6, 0x9bbf,
                                      0x6bdf, 0x3f3e, 0x3ed7, 0x57cd, 0x6ccc, 0x5fff};
// Decoded states (REG LC_STATE, TRANSITION_TARGET).
enum Eg2LcState : uint32_t {
  kLcRaw = 0,
  kLcTestUnlocked0 = 1,  // TEST_UNLOCKEDn = 1 + 2n, TEST_LOCKEDn = 2 + 2n
  kLcTestUnlocked7 = 15,
  kLcDev = 16,
  kLcProd = 17,
  kLcProdEnd = 18,
  kLcRma = 19,
  kLcScrap = 20,
  kLcPostTransition = 21,
  kLcEscalate = 22,
  kLcInvalid = 23,
};
// Each state's 20 words, word 0 first (lc_ctrl_encoding_table.md): 0, A = An, B = Bn.
inline constexpr const char *kLcStateWords[21] = {
    "00000000000000000000", "BAAAAAAAAAAAAAAAAAAA", "BBAAAAAAAAAAAAAAAAAA",
    "BBBAAAAAAAAAAAAAAAAA", "BBBBAAAAAAAAAAAAAAAA", "BBBBBAAAAAAAAAAAAAAA",
    "BBBBBBAAAAAAAAAAAAAA", "BBBBBBBAAAAAAAAAAAAA", "BBBBBBBBAAAAAAAAAAAA",
    "BBBBBBBBBAAAAAAAAAAA", "BBBBBBBBBBAAAAAAAAAA", "BBBBBBBBBBBAAAAAAAAA",
    "BBBBBBBBBBBBAAAAAAAA", "BBBBBBBBBBBBBAAAAAAA", "BBBBBBBBBBBBBBAAAAAA",
    "BBBBBBBBBBBBBBBAAAAA", "BBBBBBBBBBBBBBBBAAAA", "BBBBBBBBBBBBBBBABAAA",
    "BBBBBBBBBBBBBBBAABAA", "BBBBBBBBBBBBBBBBBABB", "BBBBBBBBBBBBBBBBBBBB"};

inline bool eg2_lc_test_unlocked(uint32_t s) { return s >= 1 && s <= 15 && (s & 1); }
inline bool eg2_lc_test_locked(uint32_t s) { return s >= 2 && s <= 14 && !(s & 1); }
inline uint16_t eg2_lc_state_word(uint32_t s, int i) {
  char c = kLcStateWords[s][i];
  return c == '0' ? 0 : c == 'A' ? kLcA[i] : kLcB[i];
}
inline uint16_t eg2_lc_count_word(uint32_t n, int j) {
  return n == 0 ? 0 : j < int(n) ? kLcD[j] : kLcC[j];
}
inline uint32_t eg2_lc_rep5(uint32_t v) {
  uint32_t r = 0;
  for (int i = 0; i < 6; ++i) r |= (v & 0x1f) << (5 * i);
  return r;
}

// What the core reads from OTP at initialisation (otp_ctrl interfaces.md "State, Counter and Token
// Output", and the HW_CFG0 broadcast).
struct Eg2LcOtpInput {
  bool valid = false;
  uint16_t state[20] = {}, count[24] = {};
  bool test_tokens_valid = false, rma_token_valid = false, secrets_valid = false;
  std::vector<uint8_t> test_unlock_hash, test_exit_hash, rma_hash;  // 16 bytes each when valid
  uint32_t device_id[8] = {}, manuf_state[8] = {};
};

class Eg2LcCtrlPort {
 public:
  virtual ~Eg2LcCtrlPort() = default;
  virtual Eg2LcOtpInput otp_input() = 0;
  // lc_otp_program: words[0..23] the count, words[24..43] the state. false: OTP reported an error.
  virtual bool otp_program(const uint16_t words[44]) = 0;
  virtual bool otp_lc_partition_error() { return false; }  // STATUS.OTP_PARTITION_ERROR
  virtual bool nvm_rma() { return true; }                  // NVM_RMA_REQ answered successfully
  virtual bool clk_byp() { return true; }                  // CLK_BYP_REQ acknowledged
  virtual void fatal_alert(const char *name) = 0;          // held until reset by the binding
  virtual void outputs_changed() {}                        // the broadcast signals may have changed
  virtual void transition_done(uint32_t target) { (void)target; }
  virtual void note(const std::string &) {}
};

class Eg2LcCtrlCore {
 public:
  enum Reg : unsigned {
    kStatus = 0, kClaimRegwen, kClaim, kTransRegwen, kCmd, kCtrl, kToken0,
    kTarget = kToken0 + 4, kVendorCtrl, kVendorStatus, kLcState, kLcCnt, kIdState, kHwRev0,
    kHwRev1, kDeviceId0, kManufState0 = kDeviceId0 + 8, kNumRegs = kManufState0 + 8,
  };
  enum Agent : unsigned { kNone = 0, kSw = 1, kTap = 2 };
  enum Status : uint32_t {
    kInitialized = 1u << 0, kReady = 1u << 1, kExtClockSwitched = 1u << 2,
    kTransitionSuccessful = 1u << 3, kTransitionCountError = 1u << 4, kTransitionError = 1u << 5,
    kTokenError = 1u << 6, kNvmRmaError = 1u << 7, kOtpError = 1u << 8, kStateError = 1u << 9,
    kBusIntegError = 1u << 10, kOtpPartitionError = 1u << 11,
  };
  static constexpr uint32_t kMuBi8True = 0x96, kMuBi8False = 0x69;
  static constexpr uint32_t kErrorBits = kTransitionCountError | kTransitionError | kTokenError |
                                         kNvmRmaError | kOtpError | kStateError | kBusIntegError;

  struct Config {
    uint16_t silicon_creator_id = 0x4001, product_id = 0x0010;  // top_earlgrey's lc_ctrl params
    uint8_t revision_id = 0x01;
    bool volatile_raw_unlock_en = false;  // SecVolatileRawUnlockEn
    std::vector<uint8_t> raw_unlock_hash;  // the hashed RAW_UNLOCK netlist constant (16 bytes)
  };

  static std::string reg_name(unsigned r) {
    static const char *kNames[] = {"STATUS", "CLAIM_TRANSITION_IF_REGWEN", "CLAIM_TRANSITION_IF",
                                   "TRANSITION_REGWEN", "TRANSITION_CMD", "TRANSITION_CTRL"};
    static const char *kMid[] = {"TRANSITION_TARGET", "OTP_VENDOR_TEST_CTRL",
                                 "OTP_VENDOR_TEST_STATUS", "LC_STATE", "LC_TRANSITION_CNT",
                                 "LC_ID_STATE", "HW_REVISION0", "HW_REVISION1"};
    if (r < kToken0) return kNames[r];
    if (r < kTarget) return "TRANSITION_TOKEN_" + std::to_string(r - kToken0);
    if (r < kDeviceId0) return kMid[r - kTarget];
    if (r < kManufState0) return "DEVICE_ID_" + std::to_string(r - kDeviceId0);
    if (r < kNumRegs) return "MANUF_STATE_" + std::to_string(r - kManufState0);
    return "";
  }

  explicit Eg2LcCtrlCore(Eg2LcCtrlPort *port) : port_(port) {}
  Config &config() { return cfg_; }

  // ToO "Token Hashing Mechanism": cSHAKE128, function name "", customization "LC_CTRL", 128 bits.
  static std::vector<uint8_t> hash_token(const std::vector<uint8_t> &token) {
    digestpp::cshake128 shake;
    shake.set_function_name("", 0);
    shake.set_customization("LC_CTRL", 7);
    shake.absorb(token.data(), token.size());
    std::vector<uint8_t> out(16);
    shake.squeeze(out.data(), out.size());
    return out;
  }

  // ---- Initialisation ----
  void power_up() {
    post_ = esc_ = trans_cmd_ = terminal_ = volatile_done_ = false;
    mutex_ = kNone;
    ctrl_ = vendor_ctrl_ = target_ = 0;
    for (uint32_t &t : token_) t = 0;
    claim_regwen_ = 1;
    otp_ = port_->otp_input();
    dec_ = kLcInvalid;
    cnt_ = 31;
    decode();
    status_ = kInitialized;
    if (dec_ == kLcInvalid) {
      status_ |= kStateError;
      port_->note("lc_ctrl: the life-cycle state or count in OTP matches no encoding: INVALID");
      port_->fatal_alert("fatal_state_error");
    } else {
      status_ |= kReady;
    }
    port_->outputs_changed();
  }

  // ---- CSRs ----
  uint32_t csr_read(Agent a, unsigned r) const {
    bool held = mutex_ == a;
    switch (r) {
      case kStatus: {
        uint32_t s = status_;
        if (port_->otp_lc_partition_error()) s |= kOtpPartitionError;
        return s;
      }
      case kClaimRegwen: return claim_regwen_;
      case kClaim: return held ? kMuBi8True : mutex_ == kNone ? kMuBi8False : 0;
      case kTransRegwen: return held && regwen() ? 1 : 0;
      case kCmd: return 0;
      case kCtrl: return held ? ctrl_ : 0;
      case kTarget: return held ? target_ : 0;
      case kVendorCtrl: return held ? vendor_ctrl_ : 0;
      case kVendorStatus: return 0;  // the vendor test interface is storage only
      case kLcState: return eg2_lc_rep5(state());
      case kLcCnt: return post_ || dec_ == kLcInvalid ? 31u : cnt_;
      case kIdState: return otp_.secrets_valid ? 0x55555555u : 0u;
      case kHwRev0: return (uint32_t(cfg_.silicon_creator_id) << 16) | cfg_.product_id;
      case kHwRev1: return cfg_.revision_id;
      default:
        if (r >= kToken0 && r < kTarget) return held ? token_[r - kToken0] : 0;
        if (r >= kDeviceId0 && r < kManufState0) return otp_.device_id[r - kDeviceId0];
        if (r >= kManufState0 && r < kNumRegs) return otp_.manuf_state[r - kManufState0];
        return 0;
    }
  }
  void csr_write(Agent a, unsigned r, uint32_t wdata, uint32_t wmask = 0xffffffffu) {
    uint32_t w = wdata & wmask;
    if (r == kClaimRegwen) {
      if (a == kSw) claim_regwen_ &= (w | ~wmask) & 1;
      return;
    }
    if (r == kClaim) {
      if (a == kSw && !(claim_regwen_ & 1)) return;
      if ((w & 0xff) == kMuBi8True) {
        if (mutex_ == kNone) mutex_ = a;
      } else if (mutex_ == a) {
        mutex_ = kNone;
      }
      return;
    }
    if (mutex_ != a || !regwen()) return;
    switch (r) {
      case kCmd:
        if (w & 1) start();
        return;
      case kCtrl: {
        // EXT_CLOCK_EN is rw1s; VOLATILE_RAW_UNLOCK is rw where the feature exists, else reads 0
        // (REG: "Software can check whether VOLATILE_RAW_UNLOCK is available by writing 1").
        uint32_t next = (ctrl_ & ~(wmask & 2)) | (w & 2) | (ctrl_ & 1) | (w & 1);
        if (!cfg_.volatile_raw_unlock_en) next &= ~2u;
        bool clk = (next & 1) && !(ctrl_ & 1);
        ctrl_ = next;
        if (clk && clk_byp_req() && port_->clk_byp()) status_ |= kExtClockSwitched;
        return;
      }
      case kTarget: target_ = (target_ & ~wmask) | (w & 0x3fffffff); return;
      case kVendorCtrl: vendor_ctrl_ = (vendor_ctrl_ & ~wmask) | w; return;
      default:
        if (r >= kToken0 && r < kTarget) token_[r - kToken0] = (token_[r - kToken0] & ~wmask) | w;
        return;
    }
  }

  // A TAP transition as the chip bench drives it (claim, target, token, start).
  void tap_transition(uint32_t target_raw, const std::vector<uint8_t> &token, bool volatile_unlock) {
    csr_write(kTap, kClaim, kMuBi8True);
    if (mutex_ != kTap) {
      port_->note("lc_ctrl: the TAP could not claim the transition mutex (software holds it)");
      return;
    }
    if (volatile_unlock) csr_write(kTap, kCtrl, 2, 2);
    csr_write(kTap, kTarget, target_raw);
    for (unsigned w = 0; w < 4; ++w) {
      uint32_t x = 0;
      for (unsigned i = 0; i < 4 && 4 * w + i < token.size(); ++i) {
        x |= uint32_t(token[4 * w + i]) << (8 * i);
      }
      csr_write(kTap, kToken0 + w, x);
    }
    csr_write(kTap, kCmd, 1);
  }

  // An escalation on either input (ToO "Security Escalation").
  void escalate() {
    if (esc_) return;
    esc_ = true;
    port_->outputs_changed();
  }

  // ---- The decoded state and the broadcast signals ----
  uint32_t state() const {
    return esc_ ? uint32_t(kLcEscalate) : post_ ? uint32_t(kLcPostTransition) : dec_;
  }
  uint32_t decoded_state() const { return dec_; }
  uint32_t count() const { return cnt_; }
  uint32_t status() const { return csr_read(kSw, kStatus); }
  bool personalized() const { return otp_.secrets_valid; }
  Agent mutex() const { return mutex_; }

  bool cpu_en() const {
    uint32_t s = state();
    return eg2_lc_test_unlocked(s) || mission(s) || s == kLcRma;
  }
  bool hw_debug_en() const {
    uint32_t s = state();
    return eg2_lc_test_unlocked(s) || s == kLcDev || s == kLcRma;
  }
  bool dft_en() const {
    uint32_t s = state();
    return eg2_lc_test_unlocked(s) || s == kLcRma;
  }
  bool nvm_debug_en() const {
    uint32_t s = state();
    return (eg2_lc_test_unlocked(s) && s != kLcTestUnlocked7) || s == kLcRma;
  }
  bool keymgr_en() const { return mission(state()) || state() == kLcRma; }
  bool creator_seed_sw_rw_en() const {
    return state() == kLcRma || (mission(state()) && !otp_.secrets_valid);
  }
  bool owner_seed_sw_rw_en() const { return keymgr_en(); }
  bool seed_hw_rd_en() const { return keymgr_en() && otp_.secrets_valid; }
  bool iso_part_sw_rd_en() const {
    uint32_t s = state();
    return s == kLcProd || s == kLcProdEnd || s == kLcRma;
  }
  bool iso_part_sw_wr_en() const { return eg2_lc_test_unlocked(state()) || keymgr_en(); }
  bool rma_state() const { return state() == kLcRma; }
  bool escalate_en() const {
    uint32_t s = state();
    return s == kLcScrap || s == kLcInvalid || s == kLcEscalate;
  }
  bool hw_debug_clr() const { return escalate_en() || state() == kLcPostTransition; }
  bool check_byp_en() const { return trans_cmd_; }
  bool clk_byp_req() const {
    uint32_t s = state();
    return (ctrl_ & 1) && (s == kLcRaw || eg2_lc_test_unlocked(s) || eg2_lc_test_locked(s) ||
                           s == kLcRma);
  }

  // DLC's transition table: 0 illegal, 1 unconditional, 2..5 the token.
  enum Tok { kIllegal = 0, kUncond, kRawUnlock, kTestUnlock, kTestExit, kRmaUnlock };
  static int rule(uint32_t from, uint32_t to) {
    bool tu_to = eg2_lc_test_unlocked(to), tl_to = eg2_lc_test_locked(to);
    if (from > kLcScrap || to > kLcScrap) return kIllegal;
    if (to == kLcScrap) return from == kLcScrap ? kIllegal : kUncond;
    if (from == kLcRaw) return tu_to ? kRawUnlock : kIllegal;
    if (eg2_lc_test_locked(from)) {
      if (tu_to) return to > from ? kTestUnlock : kIllegal;  // the encoding moves forward only
      return mission(to) ? kTestExit : kIllegal;
    }
    if (eg2_lc_test_unlocked(from)) {
      if (tl_to) return to > from ? kUncond : kIllegal;
      if (mission(to)) return kTestExit;
      return to == kLcRma ? kUncond : kIllegal;
    }
    if (from == kLcDev || from == kLcProd) return to == kLcRma ? kRmaUnlock : kIllegal;
    return kIllegal;  // PROD_END, RMA: only SCRAP; SCRAP: nothing
  }

 private:
  static bool mission(uint32_t s) { return s == kLcDev || s == kLcProd || s == kLcProdEnd; }
  bool regwen() const { return !post_ && !terminal_ && !esc_; }

  void decode() {
    if (!otp_.valid) return;
    int k = -1;
    for (int n = 0; n <= 24 && k < 0; ++n) {
      bool ok = true;
      for (int j = 0; j < 24 && ok; ++j) ok = otp_.count[j] == eg2_lc_count_word(uint32_t(n), j);
      if (ok) k = n;
    }
    int s = -1;
    for (int t = 0; t <= int(kLcScrap) && s < 0; ++t) {
      bool ok = true;
      for (int i = 0; i < 20 && ok; ++i) ok = otp_.state[i] == eg2_lc_state_word(uint32_t(t), i);
      if (ok) s = t;
    }
    if (k < 0 || s < 0) return;
    cnt_ = uint32_t(k);
    dec_ = cnt_ >= 24 ? uint32_t(kLcScrap) : uint32_t(s);
  }

  bool program(uint32_t state, uint32_t count) {
    uint16_t w[44];
    for (int j = 0; j < 24; ++j) w[j] = eg2_lc_count_word(count, j);
    for (int i = 0; i < 20; ++i) w[24 + i] = eg2_lc_state_word(state, i);
    return port_->otp_program(w);
  }
  // The OTP state the controller programs over: the decoded one, or RAW after a volatile unlock.
  uint32_t otp_state() const { return volatile_done_ ? uint32_t(kLcRaw) : dec_; }

  void start() {
    uint32_t to = target_ & 0x1f;
    std::vector<uint8_t> token(16);
    for (unsigned w = 0; w < 4; ++w) {
      for (unsigned i = 0; i < 4; ++i) token[4 * w + i] = uint8_t(token_[w] >> (8 * i));
    }
    if (ctrl_ & 2) {
      volatile_unlock(to, token);
      return;
    }
    trans_cmd_ = true;  // CHECK_BYP_EN
    uint32_t st = 0;
    if (dec_ == kLcInvalid || esc_) {
      st = kTransitionError;
    } else if (cnt_ >= 24) {
      st = kTransitionCountError;
    } else if (!program(otp_state(), cnt_ + 1)) {
      st = kOtpError;  // the count could not be programmed
    } else {
      ++cnt_;
      int tok = eg2_lc_rep5(to) == target_ ? rule(dec_, to) : int(kIllegal);
      if (tok == kIllegal) {
        st = kTransitionError;
        port_->note("lc_ctrl: rejected illegal transition " + std::to_string(dec_) + " -> " +
                    std::to_string(to));
      } else if (tok != kUncond && !token_ok(tok, token)) {
        st = kTokenError;
      } else if (to == kLcRma && !port_->nvm_rma()) {
        st = kNvmRmaError;
        port_->note("lc_ctrl: RMA transition failed because the NVM wipe failed");
      } else if (!program(to, cnt_)) {
        st = kOtpError;
      } else {
        st = kTransitionSuccessful;
        port_->note("lc_ctrl: programmed OTP life-cycle state " + std::to_string(to));
      }
    }
    if (st & kOtpError) port_->fatal_alert("fatal_prog_error");
    status_ |= st;
    post_ = true;
    port_->outputs_changed();
    port_->transition_done(to);
  }

  bool token_ok(int tok, const std::vector<uint8_t> &token) {
    // ToO "Conditional Transitions": "only allowed if the OTP partition holding the corresponding
    // token has been provisioned and locked".
    const std::vector<uint8_t> *exp = nullptr;
    if (tok == kRawUnlock) exp = &cfg_.raw_unlock_hash;
    else if (tok == kTestUnlock && otp_.test_tokens_valid) exp = &otp_.test_unlock_hash;
    else if (tok == kTestExit && otp_.test_tokens_valid) exp = &otp_.test_exit_hash;
    else if (tok == kRmaUnlock && otp_.rma_token_valid) exp = &otp_.rma_hash;
    if (!exp || exp->size() != 16) {
      port_->note("lc_ctrl: rejected transition: its token partition is not locked");
      return false;
    }
    if (hash_token(token) != *exp) {
      port_->note("lc_ctrl: rejected transition: the token hash does not match OTP");
      return false;
    }
    return true;
  }

  void volatile_unlock(uint32_t to, const std::vector<uint8_t> &token) {
    if (dec_ != kLcRaw || volatile_done_ || to != kLcTestUnlocked0 ||
        eg2_lc_rep5(to) != target_) {
      status_ |= kTransitionError;
      terminal_ = true;
    } else if (cfg_.raw_unlock_hash.size() != 16 || token != cfg_.raw_unlock_hash) {
      status_ |= kTokenError;
      terminal_ = true;
    } else {
      dec_ = kLcTestUnlocked0;
      volatile_done_ = true;
      status_ |= kTransitionSuccessful;
      port_->note("lc_ctrl: volatile RAW unlock to TEST_UNLOCKED0");
    }
    port_->outputs_changed();
  }

  Eg2LcCtrlPort *port_;
  Config cfg_;
  Eg2LcOtpInput otp_;
  uint32_t dec_ = kLcInvalid, cnt_ = 31, status_ = 0;
  uint32_t ctrl_ = 0, target_ = 0, vendor_ctrl_ = 0, token_[4] = {}, claim_regwen_ = 1;
  Agent mutex_ = kNone;
  bool post_ = false, esc_ = false, trans_cmd_ = false, terminal_ = false, volatile_done_ = false;
};

#endif  // EG2_MODEL_IP_EG2_LC_CTRL_CORE_H_
