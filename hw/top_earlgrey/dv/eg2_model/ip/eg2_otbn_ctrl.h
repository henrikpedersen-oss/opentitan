// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

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

#ifndef EG2_MODEL_IP_EG2_OTBN_CTRL_H_
#define EG2_MODEL_IP_EG2_OTBN_CTRL_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_edn.h"
#include "eg2_kmac.h"
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
    for (unsigned i = 0; i < 8; ++i) {
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

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTBN_CTRL_H_
