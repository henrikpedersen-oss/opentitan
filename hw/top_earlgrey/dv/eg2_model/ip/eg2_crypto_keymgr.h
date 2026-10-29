// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// keymgr_dpe and otbn of the EG2 SystemC model (Phase B, crypto group).
//
// keymgr_dpe  From hw/ip/keymgr_dpe/doc/theory_of_operation.md and keymgr_dpe.hjson: the working
//             states Reset / Available / Disabled / Invalid, the slots (valid, boot_stage,
//             max_key_version, key_policy), and the commands Advance, Erase Slot, Generate SW
//             Output, Generate HW Output, Disable and Load Root Key with the validity rules of the
//             theory of operation. Every operation completes at once: OP_STATUS reports
//             DONE_SUCCESS or DONE_ERROR (ERR_CODE.INVALID_OP, alert recov_operation_err) and
//             op_done fires.
//             The KDF is KMAC256 (Eg2Kmac::app_kmac) with the parent's key as key and the message
//             of the theory of operation ("Advance", "Versioned Key Generation"). Its inputs come
//             from the models the top connects: the UDS is otp_ctrl's creator root key
//             (Eg2OtpCtrl::creator_root_key; not valid while SECRET2 is unlocked, and then the
//             latching advance moves the key manager to Invalid: "If the OTP creator root key is
//             not valid during the latching cycle, keymgr_dpe moves to Invalid"), device_identifier
//             otp_ctrl's DEVICE_ID, rom_descriptors rom_ctrl's DIGEST, creator_seed and owner_seed
//             rram_ctrl's seeds (the top wires them from rram_ctrl, the doc names SECRET2/SECRET3;
//             an invalid seed counts as zero). lc_ctrl's keymgr_en deactivates the key manager
//             (Invalid, everything wiped: "Life Cycle Connection"). Netlist constants stay
//             stand-ins, SHAKE256("EG2 model keymgr_dpe <name>"): hw_revision_seed, dest_seed,
//             output_key, and health_st_measurement, lc_ctrl's 128-bit diversification constant per
//             life-cycle group (invalid, TEST_UNLOCKED*, DEV, PROD/PROD_END, RMA; interfaces.md
//             RndCnstLcKeymgrDiv*). The padding to AdvDataWidth / GenDataWidth is not reproduced
//             and the OTP shares are not descrambled, so the keys are not the chip's keys.
//             Generated keys: share 0 holds the key, share 1 zero.
//             NumBootStages = 3 (Earl Grey: keymgr_dpe_earlgrey).
// otbn        The processor of eg2_otbn.h behind OTBN's register interface (hw/ip/otbn/doc/
//             theory_of_operation.md, registers.md). After reset STATUS is BUSY_SEC_WIPE_INT
//             until edn0 gives URND, then IDLE. EXECUTE runs the program (BUSY_EXECUTE): it is
//             executed ahead, and STATUS, ERR_BITS, INSN_CNT and the done interrupt change at the
//             main-clock cycle it ends (its cycles, the waits for EDN and 100 cycles of internal
//             secure wipe, which reseeds URND from edn0); WFI pauses (PAUSED, done; RESUME goes
//             on). A software error is recoverable (IDLE, alert recov) unless
//             CTRL.software_errs_fatal makes it FATAL_SOFTWARE; a fatal error locks OTBN
//             (LOCKED, FATAL_ALERT_CAUSE, alert fatal, IMEM and DMEM wiped). Fatal errors from
//             outside the program: an IMEM or DMEM access over the bus while OTBN is busy
//             (ILLEGAL_BUS_ACCESS; DMEM is open while paused), and life cycle escalation
//             (lc_escalate_en, from escalation severities 1 and 2 through lc_ctrl).
//             SEC_WIPE_DMEM / SEC_WIPE_IMEM clear the memory; STATUS shows BUSY_SEC_WIPE_DMEM /
//             _IMEM for 64 cycles (the new key's request to OTP; its length is not documented),
//             then IDLE and done.
//             LOAD_CHECKSUM: CRC-32 over {imem, index, data} of each 32-bit memory write.
//             DMEM is 32 KiB, the window shows the first 16 KiB. The sideloaded key: share 0 is
//             KEY_S0_L, share 1 KEY_S1_L; the model's keymgr gives 256 bits, so KEY_S0_H and
//             KEY_S1_H read zero with a valid key.

#ifndef EG2_MODEL_IP_EG2_CRYPTO_KEYMGR_H_
#define EG2_MODEL_IP_EG2_CRYPTO_KEYMGR_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_crypto_aes.h"
#include "eg2_crypto_entropy.h"
#include "eg2_crypto_hash.h"
#include "eg2_mem.h"
#include "eg2_otbn.h"

namespace eg2_model {

class Eg2Otbn : public Eg2IpModel {
 public:
  enum Cmd { kExecute = 0xd8, kSecWipeDmem = 0xc3, kSecWipeImem = 0x1e, kResume = 0xa6 };
  enum Status {
    kIdle = 0, kBusyExecute = 1, kBusySecWipeDmem = 2, kBusySecWipeImem = 3,
    kBusySecWipeInt = 4, kPaused = 5, kLocked = 0xff
  };
  static constexpr uint64_t kRetryPs = 1000000;  // 1 us: the next look at EDN while it has none
  static constexpr uint64_t kBudget = 1000000;   // instructions executed per model event
  static constexpr uint64_t kWipeCycles = 100;   // the internal secure wipe at the end
  // SEC_WIPE_DMEM / SEC_WIPE_IMEM: the new scrambling key's request to OTP takes "multiple
  // cycles" (theory of operation "Secure Wipe"; the number is not documented).
  static constexpr uint64_t kMemWipeCycles = 64;

  Eg2Otbn(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    cmd_ = reg("CMD");
    ctrl_ = reg("CTRL");
    status_ = reg("STATUS");
    err_bits_ = reg("ERR_BITS");
    fatal_cause_ = reg("FATAL_ALERT_CAUSE");
    insn_cnt_ = reg("INSN_CNT");
    load_checksum_ = reg("LOAD_CHECKSUM");
    for (uint16_t r : {status_, err_bits_, fatal_cause_, insn_cnt_, load_checksum_}) {
      set_modelled(r);
    }
    imem_ = uint16_t(win_index("IMEM"));
    dmem_ = uint16_t(win_index("DMEM"));
    // RND from edn1, URND from edn0 (top_earlgrey.hjson: otbn.edn_rnd, otbn.edn_urnd).
    core_.hooks.edn_rnd = [this](uint32_t *w) { return edn_get("edn1", w); };
    core_.hooks.edn_urnd = [this](uint32_t *w) { return edn_get("edn0", w); };
    core_.hooks.kmac_prefix = [this] { return kmac_prefix(); };
    core_.hooks.kmac_key = [this] { return kmac_key(); };
    // lc_escalate_en: lc_ctrl asserts it on escalation severity 1 or 2.
    for (unsigned sev : {1u, 2u}) {
      sys_.esc_receiver(sev, [this](bool level) {
        if (level) fatal_error(Eg2OtbnCore::kLifecycleEscalation);
      });
    }
    core_.reset();
    sys_.schedule(this, kRetryPs);
  }

  // keymgr's sideload interface: share 0 and share 1, 256 bits each from the model's keymgr.
  void set_sideload_key(const uint32_t *s0, const uint32_t *s1, bool valid) {
    OtbnWide k0, k1;
    for (unsigned i = 0; valid && i < 8; ++i) {
      k0.set_word(i, s0[i]);
      k1.set_word(i, s1[i]);
    }
    core_.set_key(k0, OtbnWide(), k1, OtbnWide(), valid);
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    core_.reset();
    phase_ = kNoOp;
    urnd_words_ = 0;  // STATUS resets to BUSY_SEC_WIPE_INT
    sys_.schedule(this, sys_.now_ps() + kRetryPs);
  }

  void on_time(uint64_t now) override {
    switch (phase_) {
      case kNoOp:
        // The secure wipe after reset ends once edn0 delivers URND.
        finish_init_wipe();
        if (value(status_) == kBusySecWipeInt) sys_.schedule(this, now + kRetryPs);
        break;
      case kRunning:
        if (stop_ == Eg2OtbnCore::Stop::kWaitEdn) {
          // The wait for EDN counts in the operation's cycles.
          uint64_t elapsed = sys_.clock_cycles(Eg2System::Clk::kMain, now) - t0_;
          if (elapsed > core_.cycles()) core_.add_cycles(elapsed - core_.cycles());
        }
        advance(now);
        break;
      case kPausing:
        set_value(status_, kPaused);
        phase_ = kHeld;
        set_intr("done");
        break;
      case kEnding:
        end_execution(now);
        break;
      case kMemWipe:
        set_value(status_, kIdle);
        phase_ = kNoOp;
        set_intr("done");
        break;
      case kHeld:
        break;
    }
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    uint32_t st = value(status_);
    if (r == ctrl_ || r == err_bits_ || r == insn_cnt_) {
      // CTRL: writes are ignored unless OTBN is idle. ERR_BITS and INSN_CNT: any write clears
      // them, ignored while OTBN runs.
      bool running = st != kIdle && st != kLocked;
      if (r == ctrl_) set_value(r, st == kIdle ? value(r) : old);
      else set_value(r, running ? old : 0);
      return;
    }
    if (r != cmd_) return;
    uint32_t cmd = (wdata & wmask) & 0xff;
    // EXECUTE, SEC_WIPE_DMEM and SEC_WIPE_IMEM act only when idle, RESUME only when paused;
    // other values are ignored (registers.md CMD).
    if (cmd == kResume && st == kPaused) {
      resume_execution();
      return;
    }
    if (st != kIdle) return;
    switch (cmd) {
      case kExecute:
        start_execution();
        break;
      case kSecWipeDmem:
      case kSecWipeImem: {
        // The memory's scrambling key is replaced: its contents are gone. BUSY_SEC_WIPE_DMEM /
        // _IMEM while the new key comes from OTP, then IDLE and done.
        std::vector<uint8_t> &m = cmd == kSecWipeDmem ? core_.dmem : core_.imem;
        std::fill(m.begin(), m.end(), 0);
        set_value(status_, cmd == kSecWipeDmem ? kBusySecWipeDmem : kBusySecWipeImem);
        phase_ = kMemWipe;
        uint64_t now = sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps());
        sys_.schedule(this, sys_.clock_time_of_cycle(Eg2System::Clk::kMain, now + kMemWipeCycles));
        break;
      }
      default:
        break;
    }
  }
  void reg_before_read(uint16_t r) override {
    if (r == status_ && phase_ == kNoOp) finish_init_wipe();
    if (r == insn_cnt_ && value(status_) == kLocked) set_value(insn_cnt_, 0);  // reads 0 locked
  }

  // IMEM and DMEM over the bus: open while idle (DMEM also while paused). Otherwise a read
  // returns zero and a write is dropped, and while OTBN is busy the access is an
  // ILLEGAL_BUS_ACCESS fatal error ("Memories").
  bool win_read(uint16_t win, uint32_t off, uint8_t *data, unsigned len) override {
    if (!mem_open(win)) {
      std::memset(data, 0, len);
      bus_violation();
      return true;
    }
    std::memcpy(data, &mem(win)[off], len);
    return true;
  }
  void win_write(uint16_t win, uint32_t off, unsigned len) override {
    if (!mem_open(win)) {
      bus_violation();
      return;
    }
    std::memcpy(&mem(win)[off], &win_mem(win)[off], len);
    // LOAD_CHECKSUM: CRC-32-IEEE over the 48-bit {imem, idx (15 bits), wdata} of every 32-bit
    // aligned write, least significant byte first ("Memory Load Integrity").
    if (len == 4 && !(off & 3)) {
      uint8_t b[6];
      std::memcpy(b, &win_mem(win)[off], 4);
      uint32_t idx = (off >> 2) & 0x7fff;
      b[4] = uint8_t(idx);
      b[5] = uint8_t((idx >> 8) | (win == imem_ ? 0x80 : 0));
      set_value(load_checksum_, crc32(value(load_checksum_), b, 6));
    }
  }

 private:
  // The model's progress through an operation: none, executing (the program runs ahead of time;
  // the stop it reached takes effect at its cycle), pausing on WFI, held there, ending; a memory's
  // secure wipe.
  enum Phase { kNoOp, kRunning, kPausing, kHeld, kEnding, kMemWipe };

  std::vector<uint8_t> &mem(uint16_t win) { return win == imem_ ? core_.imem : core_.dmem; }
  bool mem_open(uint16_t win) const {
    uint32_t st = value(status_);
    return st == kIdle || (st == kPaused && win == dmem_);
  }
  void bus_violation() {
    uint32_t st = value(status_);
    if (st != kIdle && st != kLocked) fatal_error(Eg2OtbnCore::kIllegalBusAccess);
  }

  bool edn_get(const char *inst, uint32_t *w) {
    auto *edn = dynamic_cast<Eg2Edn *>(sys_.model(inst));
    return edn && edn->get(w);
  }
  // The KMAC block's PREFIX (cSHAKE's encode_string(N) || encode_string(S)) and its software key
  // (KEY_SHARE0 ^ KEY_SHARE1, KEY_LEN), as its application interface uses them.
  std::vector<uint8_t> kmac_prefix() {
    std::vector<uint8_t> p;
    Eg2IpModel *k = sys_.model("kmac");
    int r = k ? k->reg_index("PREFIX_0") : -1;
    for (unsigned i = 0; r >= 0 && i < 11; ++i) {
      uint32_t v = k->value(uint16_t(r + int(i)));
      for (unsigned b = 0; b < 4; ++b) p.push_back(uint8_t(v >> (8 * b)));
    }
    return p;
  }
  std::vector<uint8_t> kmac_key() {
    static const unsigned kKeyBytes[] = {16, 24, 32, 48, 64};
    std::vector<uint8_t> key;
    Eg2IpModel *k = sys_.model("kmac");
    int s0 = k ? k->reg_index("KEY_SHARE0_0") : -1, s1 = k ? k->reg_index("KEY_SHARE1_0") : -1;
    int len = k ? k->reg_index("KEY_LEN") : -1;
    if (s0 < 0 || s1 < 0 || len < 0) return std::vector<uint8_t>(32, 0);
    unsigned kl = k->value(uint16_t(len)) & 7;
    unsigned n = kl < 5 ? kKeyBytes[kl] : 32;
    for (unsigned i = 0; i < n / 4; ++i) {
      uint32_t v = k->value(uint16_t(s0 + int(i))) ^ k->value(uint16_t(s1 + int(i)));
      for (unsigned b = 0; b < 4; ++b) key.push_back(uint8_t(v >> (8 * b)));
    }
    return key;
  }
  static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
      crc ^= p[i];
      for (unsigned k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
  }

  void finish_init_wipe() {
    if (value(status_) != kBusySecWipeInt) return;
    uint32_t w;
    while (urnd_words_ < Eg2OtbnCore::kUrndSeedWords && edn_get("edn0", &w)) ++urnd_words_;
    if (urnd_words_ == Eg2OtbnCore::kUrndSeedWords) set_value(status_, kIdle);
  }

  uint64_t time_at(uint64_t cycles) const {
    return sys_.clock_time_of_cycle(Eg2System::Clk::kMain, t0_ + cycles);
  }
  void start_execution() {
    core_.wfi_enabled = (value(ctrl_) >> 1) & 1;
    core_.urnd_ctrl_enabled = (value(ctrl_) >> 2) & 1;
    errs_fatal_ = value(ctrl_) & 1;
    core_.start();
    set_value(status_, kBusyExecute);
    set_value(insn_cnt_, 0);
    t0_ = sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps());
    phase_ = kRunning;
    advance(sys_.now_ps());
  }
  void resume_execution() {
    core_.resume();
    set_value(status_, kBusyExecute);
    // The cycles go on from now.
    t0_ = sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps()) - core_.cycles();
    phase_ = kRunning;
    advance(sys_.now_ps());
  }
  // Run the program up to its next stop and schedule the stop at its cycle.
  void advance(uint64_t now) {
    stop_ = core_.run(kBudget);
    switch (stop_) {
      case Eg2OtbnCore::Stop::kEnd:
        phase_ = kEnding;
        end_words_ = 0;
        sys_.schedule(this, std::max(now, time_at(core_.cycles() + kWipeCycles)));
        break;
      case Eg2OtbnCore::Stop::kWfi:
        phase_ = kPausing;
        sys_.schedule(this, std::max(now, time_at(core_.cycles())));
        break;
      case Eg2OtbnCore::Stop::kWaitEdn:
        sys_.schedule(this, std::max(now + kRetryPs, time_at(core_.cycles())));
        break;
      case Eg2OtbnCore::Stop::kBudget:
        sys_.schedule(this, std::max(now, time_at(core_.cycles())));
        break;
    }
  }
  // The end of an execution: the internal secure wipe (with a URND reseed from edn0), ERR_BITS,
  // INSN_CNT, then IDLE or LOCKED and the done interrupt ("Software Execution").
  void end_execution(uint64_t now) {
    uint32_t w;
    while (end_words_ < Eg2OtbnCore::kUrndSeedWords && edn_get("edn0", &w)) ++end_words_;
    if (end_words_ < Eg2OtbnCore::kUrndSeedWords) {
      sys_.schedule(this, now + kRetryPs);
      return;
    }
    uint32_t err = core_.err_bits();
    if (errs_fatal_ && (err & ~Eg2OtbnCore::kFatalMask)) err |= Eg2OtbnCore::kFatalSoftware;
    finish(err);
  }
  void finish(uint32_t err) {
    if (err) {
      char buf[120];
      std::snprintf(buf, sizeof buf,
                    "otbn: the program ended with ERR_BITS 0x%08x at pc 0x%04x after %u "
                    "instructions",
                    unsigned(err), unsigned(core_.pc()), unsigned(core_.insn_cnt()));
      sys_.note(buf);
    }
    core_.wipe_internal();
    phase_ = kNoOp;
    sys_.unschedule(this);
    set_value(err_bits_, err);
    set_value(insn_cnt_, core_.insn_cnt());
    set_intr("done");
    if (err & Eg2OtbnCore::kFatalMask) {
      lock(err);
    } else {
      set_value(status_, kIdle);
      if (err) raise_alert("recov");
    }
  }
  // A fatal error: a running operation ends with it; OTBN locks.
  void fatal_error(uint32_t bits) {
    uint32_t st = value(status_);
    if (st == kLocked) return;
    if (st == kBusyExecute || st == kPaused) {
      core_.fatal(bits);
      finish(core_.err_bits());
    } else {
      phase_ = kNoOp;
      sys_.unschedule(this);
      lock(bits);
    }
  }
  // LOCKED: FATAL_ALERT_CAUSE, IMEM and DMEM wiped, the fatal alert ("Reaction to Fatal Errors").
  void lock(uint32_t err) {
    set_value(status_, kLocked);
    set_value(fatal_cause_, value(fatal_cause_) | ((err >> 16) & 0xff));
    std::fill(core_.imem.begin(), core_.imem.end(), 0);
    std::fill(core_.dmem.begin(), core_.dmem.end(), 0);
    raise_alert("fatal");
  }

  uint16_t cmd_, ctrl_, status_, err_bits_, fatal_cause_, insn_cnt_, load_checksum_, imem_, dmem_;
  Eg2OtbnCore core_;
  Phase phase_ = kNoOp;
  Eg2OtbnCore::Stop stop_ = Eg2OtbnCore::Stop::kBudget;
  uint64_t t0_ = 0;  // the main-clock cycle the operation's cycle 0 is
  bool errs_fatal_ = false;
  unsigned urnd_words_ = 0, end_words_ = 0;

 public:
  bool idle() const override { return value(status_) == kIdle; }
};

// ---------------------------------------------------------------------------------------------
class Eg2KeymgrDpe : public Eg2IpModel {
 public:
  static constexpr unsigned kSlots = 4, kNumBootStages = 3;
  enum State { kReset = 0, kAvailable = 1, kDisabled = 2, kInvalid = 3 };
  enum Op { kAdvance = 0, kErase = 1, kGenSw = 2, kGenHw = 3, kDisable = 4, kLoadRoot = 5 };
  enum Dest { kNone = 0, kAes = 1, kKmac = 2, kOtbn = 3, kHmac = 4 };

  Eg2KeymgrDpe(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    start_ = reg("START");
    control_ = reg("CONTROL_SHADOWED");
    slot_policy_ = reg("SLOT_POLICY");
    key_version_ = reg("KEY_VERSION");
    max_key_ver_ = reg("MAX_KEY_VER_SHADOWED");
    working_state_ = reg("WORKING_STATE");
    op_status_ = reg("OP_STATUS");
    err_code_ = reg("ERR_CODE");
    sideload_clear_ = reg("SIDELOAD_CLEAR");
    load_root_lock_ = reg("LOAD_ROOT_KEY_LOCK");
    enforce_sw_binding_ = reg("ENFORCE_SW_BINDING");
    regwen_[0] = reg("SW_BINDING_REGWEN");
    regwen_[1] = reg("SLOT_POLICY_REGWEN");
    regwen_[2] = reg("MAX_KEY_VER_REGWEN");
    for (unsigned i = 0; i < 8; ++i) {
      sw_binding_[i] = reg(("SW_BINDING_" + std::to_string(i)).c_str());
      salt_[i] = reg(("SALT_" + std::to_string(i)).c_str());
      out0_[i] = reg(("SW_SHARE0_OUTPUT_" + std::to_string(i)).c_str());
      out1_[i] = reg(("SW_SHARE1_OUTPUT_" + std::to_string(i)).c_str());
      meta_lo_[i] = reg(("METADATA_LOW_" + std::to_string(i)).c_str());
      meta_hi_[i] = reg(("METADATA_HIGH_" + std::to_string(i)).c_str());
      for (uint16_t r : {out0_[i], out1_[i], meta_lo_[i], meta_hi_[i]}) set_modelled(r);
    }
    for (uint16_t r : {start_, working_state_, op_status_, err_code_}) set_modelled(r);
    update();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    state_ = kReset;
    root_loaded_ = false;
    for (auto &s : slots_) s = Slot();
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == start_ && (value(start_) & 1)) {
      bool ok = operate();
      // A successful advance unlocks SW_BINDING and SLOT_POLICY again (keymgr_dpe.hjson: "unlocked
      // by hardware upon a successful advance call", "After a successful advance operation, this
      // register is unlocked again"); for MAX_KEY_VER the hjson is silent, the RTL does the same
      // (keymgr_dpe.sv: all three prim_flop_2sync set by unlock_after_advance).
      if (ok && op() == kAdvance && state_ == kAvailable) {
        for (uint16_t rw : regwen_) set_value(rw, 1);
      }
      set_value(start_, 0);
      set_value(op_status_, ok ? 2 : 3);  // DONE_SUCCESS / DONE_ERROR
      if (!ok) {
        // INVALID_OP, a recoverable error, signalled by recov_operation_err ("Alert for key
        // manager operation errors. These errors could have been caused by software",
        // interfaces.md).
        set_value(err_code_, value(err_code_) | 1);
        raise_alert("recov_operation_err");
      }
      set_intr("op_done");
    } else if (r == sideload_clear_) {
      // VAL selects which sideload slot reads as cleared (a value other than 0-4 clears all).
      uint32_t v = value(sideload_clear_) & 7;
      if (v == 0 || v > 4) {
        for (unsigned dst = kAes; dst <= kHmac; ++dst) sideload(dst, nullptr);
      } else {
        sideload(v, nullptr);
      }
    }
    update();
  }

 private:
  struct Slot {
    bool valid = false;
    unsigned boot_stage = 0;
    uint32_t max_key_version = 0;
    unsigned policy = 0;  // allow_child (0), exportable (1), retain_parent (2), allow_keygen (3)
    std::vector<uint8_t> key;
  };

  static std::vector<uint8_t> standin(const char *name, size_t n = 32) {
    digestpp::shake256 h;
    std::string tag = std::string("EG2 model keymgr_dpe ") + name;
    h.absorb(reinterpret_cast<const unsigned char *>(tag.data()), tag.size());
    std::vector<uint8_t> out(n);
    h.squeeze(out.data(), n);
    return out;
  }
  std::vector<uint8_t> device_id() {
    auto *otp = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    return otp ? otp->device_id() : std::vector<uint8_t>(32, 0);
  }
  std::vector<uint8_t> health() {
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    uint32_t s = lc ? lc->state() : kLcInvalid;
    const char *g = eg2_lc_test_unlocked(s) ? "health_test_unlocked"
                    : s == kLcDev                          ? "health_dev"
                    : s == kLcProd || s == kLcProdEnd      ? "health_production"
                    : s == kLcRma                          ? "health_rma"
                                                           : "health_invalid";
    return standin(g, 16);
  }
  std::vector<uint8_t> rom_digest() {
    std::vector<uint8_t> d(32, 0);
    Eg2IpModel *rom = sys_.model("rom_ctrl.regs");
    int r = rom ? rom->reg_index("DIGEST_0") : -1;
    for (unsigned i = 0; r >= 0 && i < 8; ++i) {
      uint32_t w = rom->value(uint16_t(r + int(i)));
      for (unsigned b = 0; b < 4; ++b) d[4 * i + b] = uint8_t(w >> (8 * b));
    }
    return d;
  }
  std::vector<uint8_t> seed(bool owner) {
    std::vector<uint8_t> s(32, 0);
    auto *rram = dynamic_cast<Eg2RramCtrl *>(sys_.model("rram_ctrl.core"));
    if (rram && !rram->keymgr_seed(owner, &s)) s.assign(32, 0);
    return s;
  }
  static void append(std::vector<uint8_t> &m, const std::vector<uint8_t> &x) {
    m.insert(m.end(), x.begin(), x.end());
  }
  void append_words(std::vector<uint8_t> &m, const uint16_t *regs, unsigned n) const {
    for (unsigned i = 0; i < n; ++i) {
      for (unsigned b = 0; b < 4; ++b) m.push_back(uint8_t(value(regs[i]) >> (8 * b)));
    }
  }

  unsigned op() const { return (value(control_) >> 4) & 7; }
  unsigned dest() const { return (value(control_) >> 11) & 7; }
  unsigned src() const { return (value(control_) >> 14) & 7; }
  unsigned dst() const { return (value(control_) >> 18) & 7; }
  bool sw_binding_only() const { return (value(control_) >> 22) & 1; }

  // lc_ctrl's keymgr_en: off moves the key manager to Invalid and wipes it.
  bool lc_check() {
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    if (lc && !lc->keymgr_en() && state_ != kInvalid) invalidate();
    return state_ != kInvalid;
  }
  void invalidate() {
    state_ = kInvalid;
    for (auto &s : slots_) s = Slot();
    for (unsigned dst = kAes; dst <= kHmac; ++dst) sideload(dst, nullptr);
    for (unsigned i = 0; i < 8; ++i) {
      set_value(out0_[i], 0);
      set_value(out1_[i], 0);
    }
  }
  void reg_before_read(uint16_t r) override {
    if (r == working_state_) {
      lc_check();
      update();
    }
  }

  bool operate() {
    if (!lc_check() || state_ == kDisabled) return false;
    switch (op()) {
      case kAdvance: return state_ == kReset ? load_root() : advance();
      case kLoadRoot: return state_ == kAvailable && !(value(load_root_lock_) & 1) && load_root();
      case kErase:
        if (state_ != kAvailable || dst() >= kSlots || !slots_[dst()].valid) return false;
        slots_[dst()] = Slot();
        return true;
      case kGenSw:
      case kGenHw: return state_ == kAvailable && generate(op() == kGenHw);
      case kDisable:
        if (state_ != kAvailable) return false;
        state_ = kDisabled;
        for (auto &s : slots_) s = Slot();  // slots wiped, outputs kept
        return true;
      default: return false;
    }
  }
  // The first advance, or Load Root Key: the UDS into SLOT_DST_SEL, boot stage 0, the fixed UDS
  // policy (allow_child only).
  bool load_root() {
    if (dst() >= kSlots) return false;
    std::vector<uint8_t> uds;
    auto *otp = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    if (!otp || !otp->creator_root_key(&uds)) {
      invalidate();  // silently: the operation itself completes (DONE_SUCCESS)
      return true;
    }
    Slot &s = slots_[dst()];
    s.valid = true;
    s.boot_stage = 0;
    s.max_key_version = value(max_key_ver_);
    s.policy = 1;
    s.key = uds;
    state_ = kAvailable;
    root_loaded_ = true;
    return true;
  }
  bool advance() {
    if (src() >= kSlots || dst() >= kSlots) return false;
    Slot &p = slots_[src()];
    // No boot-stage limit. theory_of_operation.md lists "boot_stage + 1 < NumBootStages" as an
    // advance condition, but also has advances "from parents with boot_stage > 2" (message =
    // SW_CDI_INPUT) and keymgr_dpe_key_derivation_test advances its stage-3 OwnerKey; the RTL
    // (keymgr_dpe_ctrl.sv next_boot_stage) saturates boot_stage at BootStageRuntime (3).
    if (!p.valid || !(p.policy & 1)) return false;
    bool retain = p.policy & 4;
    if (retain && src() == dst()) return false;
    if (slots_[dst()].valid && dst() != src() && (slots_[dst()].policy & 4)) return false;
    bool enforce = mubi4_true(value(enforce_sw_binding_));  // a MuBi4 (keymgr_dpe.hjson)
    if (enforce && p.boot_stage <= 2 && !sw_binding_only()) return false;
    // message = SW_CDI_INPUT || stage inputs (NumBootStages = 3).
    std::vector<uint8_t> msg;
    append_words(msg, sw_binding_, 8);
    if (!sw_binding_only()) {
      if (p.boot_stage == 0) {
        append(msg, device_id());
        append(msg, health());
        append(msg, rom_digest());
        append(msg, standin("hw_revision_seed"));
      } else if (p.boot_stage == 1) {
        append(msg, seed(false));
      } else if (p.boot_stage == 2) {
        append(msg, seed(true));
      }
    }
    Slot child;
    child.valid = true;
    child.boot_stage = p.boot_stage < kNumBootStages ? p.boot_stage + 1 : kNumBootStages;
    child.max_key_version = value(max_key_ver_);
    child.policy = value(slot_policy_) & 0xf;
    child.key = kdf(p.key, msg);
    if (!retain) p = Slot();
    slots_[dst()] = child;
    return true;
  }
  bool generate(bool hw) {
    if (src() >= kSlots) return false;
    const Slot &s = slots_[src()];
    if (!s.valid || !(s.policy & 8) || value(key_version_) > s.max_key_version) return false;
    if (hw && (dest() == kNone || dest() > kHmac)) return false;
    static const char *kDest[] = {"dest_none", "dest_aes", "dest_kmac", "dest_otbn", "dest_hmac"};
    std::vector<uint8_t> msg;
    append_words(msg, &key_version_, 1);
    append_words(msg, salt_, 8);
    append(msg, standin(kDest[dest() <= kHmac ? dest() : 0]));
    append(msg, standin(hw ? "output_hw" : "output_sw"));
    std::vector<uint8_t> key = kdf(s.key, msg);
    if (hw) {
      // HMAC's sideload interface is wide (512 bits, wide_hw_key_req): the whole KDF output.
      wide_key_ = Eg2Kmac::app_kmac(s.key, {}, msg, 64);
      sideload(dest(), &key);
    } else {
      for (unsigned i = 0; i < 8; ++i) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; ++b) w |= uint32_t(key[4 * i + b]) << (8 * b);
        set_value(out0_[i], w);
        set_value(out1_[i], 0);
      }
    }
    return true;
  }
  static std::vector<uint8_t> kdf(const std::vector<uint8_t> &key,
                                  const std::vector<uint8_t> &msg) {
    std::vector<uint8_t> d = Eg2Kmac::app_kmac(key, {}, msg, 64);
    d.resize(32);
    return d;
  }
  // Load (key != nullptr) or clear a sideload slot.
  void sideload(unsigned dst, const std::vector<uint8_t> *key) {
    uint32_t s0[8] = {}, s1[8] = {};
    for (unsigned i = 0; key && i < 8; ++i) {
      for (unsigned b = 0; b < 4; ++b) s0[i] |= uint32_t((*key)[4 * i + b]) << (8 * b);
    }
    bool valid = key != nullptr;
    switch (dst) {
      case kAes:
        if (auto *m = dynamic_cast<Eg2Aes *>(sys_.model("aes"))) m->set_sideload_key(s0, s1, valid);
        break;
      case kKmac:
        if (auto *m = dynamic_cast<Eg2Kmac *>(sys_.model("kmac"))) m->set_sideload_key(s0, s1, valid);
        break;
      case kOtbn:
        if (auto *m = dynamic_cast<Eg2Otbn *>(sys_.model("otbn"))) m->set_sideload_key(s0, s1, valid);
        break;
      case kHmac:
        if (auto *m = dynamic_cast<Eg2Hmac *>(sys_.model("hmac"))) {
          uint32_t w[16] = {};
          for (unsigned i = 0; valid && i < 16 && 4 * i + 3 < wide_key_.size(); ++i) {
            for (unsigned b = 0; b < 4; ++b) w[i] |= uint32_t(wide_key_[4 * i + b]) << (8 * b);
          }
          m->set_sideload_key(w, valid);
        }
        break;
      default:
        break;
    }
  }

  void update() {
    set_value(working_state_, state_);
    for (unsigned i = 0; i < 8; ++i) {
      const Slot *s = i < kSlots ? &slots_[i] : nullptr;
      set_value(meta_lo_[i], s ? s->max_key_version : 0);
      // METADATA_HIGH: VALID (0), BOOT_STAGE (2:1), ALLOW_CHILD (3), EXPORTABLE (4),
      // RETAIN_PARENT (5), ALLOW_KEYGEN (6).
      uint32_t hi = 0;
      if (s && s->valid) hi = 1u | ((s->boot_stage & 3) << 1) | ((s->policy & 0xf) << 3);
      set_value(meta_hi_[i], hi);
    }
  }

  uint16_t start_, control_, slot_policy_, key_version_, max_key_ver_, working_state_, op_status_,
      err_code_, sideload_clear_, load_root_lock_, enforce_sw_binding_, regwen_[3];
  uint16_t sw_binding_[8], salt_[8], out0_[8], out1_[8], meta_lo_[8], meta_hi_[8];
  std::array<Slot, kSlots> slots_;
  State state_ = kReset;
  bool root_loaded_ = false;
  std::vector<uint8_t> wide_key_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CRYPTO_KEYMGR_H_
