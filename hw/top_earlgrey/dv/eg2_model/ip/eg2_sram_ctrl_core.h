// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The SRAM controller's behaviour as the EG2 model has it, in plain C++ (no SystemC): one core for
// its bindings, as the power and reset managers' (eg2_pwrmgr_core.h, eg2_rstmgr_core.h):
//   eg2sim      Eg2SramCtrl (eg2_mem.h), forwarding software's register accesses and the chip's
//               escalation to the core, the RAM being eg2sim's Eg2Memory bytes;
//   self-test   dpi/eg2_sram_ctrl_core_test.cc, the core with its own memory.
// The core holds every register of sram_ctrl itself; it reaches its peers only through
// Eg2SramCtrlPort.
//
// From sram_ctrl's documents only (hw/ip/sram_ctrl: doc/theory_of_operation.md "ToO",
// doc/programmers_guide.md "PG", doc/registers.md "REG", data/sram_ctrl.hjson countermeasures
// "CM"; lc_ctrl doc/interfaces.md for the life-cycle signal encodings), never from its RTL:
// - Registers (REG): ALERT_TEST (fatal_error wo, regwen rw0c in bit 31), STATUS (ro), EXEC_REGWEN /
//   CTRL_REGWEN / READBACK_REGWEN (rw0c), EXEC and READBACK (rw MuBi4, reset MuBi4False, each
//   behind its REGWEN), CTRL (wo: RENEW_SCR_KEY, INIT; behind CTRL_REGWEN), SCR_KEY_ROTATED
//   (MuBi4, "Write kMultiBitBool4True to clear", reset MuBi4False).
// - Key request (REG CTRL.RENEW_SCR_KEY, PG "Initialization"): STATUS.SCR_KEY_VALID is cleared,
//   memory transactions are blocked (stalled) until the new key arrives; on arrival SCR_KEY_VALID,
//   SCR_KEY_SEED_VALID ("derived from a valid key seed in OTP") and SCR_KEY_ROTATED = MuBi4True.
//   "Requesting a key implicitly wipes all data in the SRAM": the old contents read as garbage.
//   A RENEW while a key request or an INIT is pending has no effect.
// - INIT (ToO "LFSR Initialization Feature", "Zero Initialization"; REG CTRL.INIT): the whole
//   memory is overwritten with pseudo-random words with matching integrity bits, or with zeros
//   when the instance is built with SecZeroInit; STATUS.INIT_DONE when complete. RENEW and INIT in
//   one write: the key request completes first, then INIT. An INIT while one is pending has no
//   effect. The model is not timing-accurate (decision 2026-11-08): INIT completes at once unless it
//   waits for a key; only "busy until done" is kept where software can see it.
// - Escalation (REG STATUS.ESCALATED, CM KEY.GLOBAL_ESC): lc_escalate_en != Off ("In case of
//   ESCALATE_EN, all values different from OFF must be interpreted as ON", lc_ctrl interfaces.md)
//   sets ESCALATED, resets the key and nonce to their defaults, blocks every later memory request;
//   terminal until reset. SCR_KEY_ROTATED is cleared ("HW does not auto-clear the register except
//   during escalation").
//   interfaces.md "Global and Local Escalation" says the same: keys "discarded and reset to
//   RndCnstSramKey and RndCnstSramNonce", "Any subsequent memory request ... blocked".
// - Local escalation (CM KEY.LOCAL_ESC: "due to bus integrity or counter errors"; interfaces.md:
//   "identical to global escalation via lc_escalate_en_i"): a bus integrity
//   error (STATUS.BUS_INTEG_ERROR; ToO "Integrity Error Handling": latched, fatal_bus_integ_error
//   until reset, "Disallow any transaction (read or write) on the actual memory macro") or an
//   initialisation counter error (STATUS.INIT_ERROR) reset the key and block the memory too.
// - Other fatal errors (REG STATUS): SRAM_ALERT (a multi-bit encoding error inside the RAM) and
//   READBACK_ERROR (a failed readback check, only while READBACK.EN is not MuBi4False: "Any other
//   value than kMultiBitBool4False ... is interpreted as kMultiBitBool4True"); both terminal and
//   fatal_error, neither documented to block the memory, so the core does not block for them.
//   Every fatal error holds the fatal alert until reset.
// - Execution (ToO "Code Execution from SRAM", the table; CM INSTR.BUS.LC_GATED): never without
//   InstrExec; else EXEC.EN == MuBi4True if OTP's EN_SRAM_IFETCH == MuBi8True, HW_DEBUG_EN == On
//   otherwise ("all values different from ON must be interpreted as OFF").
// - Range (PG "Accesses Outside of the Implemented Memory Range"): the decoded range is the next
//   power of two of the size; an access inside it but beyond the size gets a TL-UL error and does
//   not touch the memory. That applies even while a key request or INIT is pending (ToO).
// - Contents: scrambling is not visible to software, so the core keeps plain contents, with a
//   state per byte: known, valid (integrity good, value not predicted: LFSR INIT), garbage (old key:
//   value not predicted, an integrity error expected), open (no prediction at all).
//   Out of reset the controller uses its predefined key (ToO), so data written under a renewed key
//   is garbage after a reset; data written under the predefined key is not (same key again).
// - NumAddrScrRounds (sram_ctrl.hjson "0 disables address scrambling"; CM ADDR.SCRAMBLE): the
//   logical-to-physical word map is the identity with 0 rounds, a keyed S&P network otherwise
//   (physical_word: -1, not modelled). Nothing software sees changes: the data keystream still
//   depends on key, nonce and address, so a key change still makes every word garbage.
//
// Not documented, so the core does not predict it (STATUS / SCR_KEY_ROTATED bits in `unknown` of
// csr_read; a binding that must give a value takes the core's guess, value):
// - INIT_DONE after a key renewal without INIT (guess: unchanged), and while an INIT waits
//   (guess: 0, cleared when the INIT is requested);
// - ESCALATED after a local escalation (guess: 0);
// - SCR_KEY_VALID / SCR_KEY_SEED_VALID after an escalation (guess: unchanged); SCR_KEY_ROTATED after
//   a local escalation (guess: unchanged), after a write of a value other than MuBi4True (guess:
//   unchanged);
// - anything CTRL does after an escalation (the core ignores it and leaves the key bits open);
// - how the bus answers a blocked request (Access::kBlocked) or a refused fetch (Access::kRefused);
// - the bytes a sub-word write leaves on a garbage word (read-modify-write: open).

#ifndef EG2_MODEL_IP_EG2_SRAM_CTRL_CORE_H_
#define EG2_MODEL_IP_EG2_SRAM_CTRL_CORE_H_

#include <cstdint>
#include <string>
#include <vector>

// What the core needs from its binding. Every hook may be left as it is.
class Eg2SramCtrlPort {
 public:
  virtual ~Eg2SramCtrlPort() = default;
  // The RAM's bytes (eg2sim: the Eg2Memory's); nullptr: the core's own.
  virtual std::vector<uint8_t> *memory() { return nullptr; }
  // A key request to OTP; the binding answers with Eg2SramCtrlCore::key_response, at once (from
  // inside this call) or later.
  virtual void key_request() {}
  // A fatal error began: fatal_error is held until reset.
  virtual void fatal_alert() {}
  // ALERT_TEST raised one fatal_error event.
  virtual void alert_test() {}
  virtual void note(const std::string &) {}
};

class Eg2SramCtrlCore {
 public:
  // Register offsets (registers.md "Summary").
  static constexpr uint32_t kAlertTest = 0x0, kStatus = 0x4, kExecRegwen = 0x8, kExec = 0xc,
                            kCtrlRegwen = 0x10, kCtrl = 0x14, kScrKeyRotated = 0x18,
                            kReadbackRegwen = 0x1c, kReadback = 0x20;
  // STATUS bits.
  static constexpr uint32_t kBusIntegError = 1u << 0, kInitError = 1u << 1, kEscalated = 1u << 2,
                            kScrKeyValid = 1u << 3, kScrKeySeedValid = 1u << 4,
                            kInitDone = 1u << 5, kReadbackError = 1u << 6, kSramAlert = 1u << 7;
  static constexpr uint32_t kMuBi4True = 0x6, kMuBi4False = 0x9, kMuBi8True = 0x96;
  static constexpr uint32_t kLcOn = 0xa, kLcOff = 0x5;  // lc_tx_t, 4 bits

  // Per-instance parameters (top_earlgrey.hjson param_decl; sram_ctrl.hjson defaults otherwise).
  struct Params {
    std::string name;
    uint32_t bytes = 0x8000;
    bool instr_exec = true;          // default 1
    bool sec_zero_init = false;      // default 0
    unsigned num_addr_scr_rounds = 2;
    unsigned num_prince_rounds_half = 3;
  };
  static Params instance(const std::string &inst) {
    Params p;
    p.name = inst;
    if (inst == "sram_ctrl_main") {
      p.bytes = 0x20000, p.instr_exec = true, p.num_prince_rounds_half = 2;
    } else if (inst == "sram_ctrl_sec") {
      p.bytes = 0x10000, p.instr_exec = true, p.num_prince_rounds_half = 2;
    } else if (inst == "sram_ctrl_meta") {
      // 9728 words = 38 KiB; neither a power of two nor a multiple of 4096 words.
      p.bytes = 0x9800, p.instr_exec = false, p.num_addr_scr_rounds = 0, p.sec_zero_init = true,
      p.num_prince_rounds_half = 2;
    } else if (inst == "sram_ctrl_ret") {
      p.bytes = 0x2000, p.instr_exec = false;
    }
    return p;
  }

  // Memory access results.
  enum class Access {
    kOk,       // done
    kStall,    // a key request or INIT is pending: the access waits ("blocked until the request
               // has completed"); retry it after key_response
    kBlocked,  // escalated or a local escalation: terminal until reset (the bus answer is not
               // documented)
    kError,    // outside the implemented memory: a TL-UL error, the memory untouched
    kRefused,  // an instruction fetch while execution is not enabled (the answer is not documented)
  };
  // Byte states (see the header comment).
  enum ByteState : uint8_t { kGarbage = 0, kOpen = 1, kValid = 2, kKnown = 3 };
  struct Read {
    Access acc = Access::kOk;
    uint32_t data = 0;
    uint32_t unknown = 0;   // bits whose value the model does not predict
    bool garbage = false;   // a byte read under another key: an integrity error is expected
  };

  explicit Eg2SramCtrlCore(const Params &p, Eg2SramCtrlPort *port = nullptr)
      : p_(p), port_(port ? port : &null_port_) {
    decoded_ = 4;
    while (decoded_ < p_.bytes) decoded_ <<= 1;
    own_.assign(p_.bytes, 0);
    state_.assign(p_.bytes, kOpen);  // power-up contents: unknown
    reset_regs();
  }
  Eg2SramCtrlCore(const Eg2SramCtrlCore &) = delete;
  Eg2SramCtrlCore &operator=(const Eg2SramCtrlCore &) = delete;
  const Params &params() const { return p_; }
  uint32_t decoded_bytes() const { return decoded_; }
  uint32_t words() const { return p_.bytes / 4; }

  // ---- Inputs ----
  void set_otp_en_sram_ifetch(uint32_t mubi8) { otp_ifetch_ = mubi8 & 0xff; }
  void set_lc_hw_debug_en(uint32_t lc) { lc_debug_ = lc & 0xf; }
  void set_lc_escalate_en(uint32_t lc) {
    if ((lc & 0xf) != kLcOff) escalate();
  }

  // ---- Reset: registers to their reset values, latched errors and escalation gone, the
  // predefined key again (contents written under a renewed key turn to garbage). ----
  void reset() {
    if (keyed_) garble("reset: the predefined key again");
    keyed_ = false;
    reset_regs();
  }

  // ---- Registers ----
  void csr_write(uint32_t addr, uint32_t data, uint32_t mask = 0xffffffffu) {
    uint32_t w = data & mask;
    switch (addr) {
      case kAlertTest:
        // The write's test bit is gated by the regwen as it was before the write; a 0 written to
        // bit 31 (rw0c) then disables later tests.
        if ((w & 1) && alert_regwen_) {
          ++alert_tests_;
          port_->alert_test();
        }
        if ((mask >> 31 & 1) && !(w >> 31 & 1)) alert_regwen_ = false;
        break;
      case kExecRegwen:
        if ((mask & 1) && !(w & 1)) exec_regwen_ = false;
        break;
      case kExec:
        if (exec_regwen_) exec_ = (exec_ & ~mask & 0xf) | (w & 0xf);
        break;
      case kCtrlRegwen:
        if ((mask & 1) && !(w & 1)) ctrl_regwen_ = false;
        break;
      case kReadbackRegwen:
        if ((mask & 1) && !(w & 1)) readback_regwen_ = false;
        break;
      case kReadback:
        if (readback_regwen_) readback_ = (readback_ & ~mask & 0xf) | (w & 0xf);
        break;
      case kScrKeyRotated:
        if ((mask & 0xf) != 0xf) break;
        if ((w & 0xf) == kMuBi4True) {
          rotated_ = kMuBi4False;
        } else if (w & 0xf) {
          rotated_unknown_ = true;  // a rw1c of other bits: not documented
        }
        break;
      case kCtrl:
        if (ctrl_regwen_) ctrl(w & 3);
        break;
      default:
        break;  // STATUS (ro) and unmapped offsets
    }
  }

  uint32_t csr_read(uint32_t addr, uint32_t *unknown = nullptr) const {
    uint32_t u = 0, v = 0;
    switch (addr) {
      case kAlertTest: v = alert_regwen_ ? 1u << 31 : 0; break;
      case kStatus: v = status_; u = status_unknown_; break;
      case kExecRegwen: v = exec_regwen_; break;
      case kExec: v = exec_; break;
      case kCtrlRegwen: v = ctrl_regwen_; break;
      case kCtrl: v = 0; break;  // wo
      case kScrKeyRotated: v = rotated_; u = rotated_unknown_ ? 0xf : 0; break;
      case kReadbackRegwen: v = readback_regwen_; break;
      case kReadback: v = readback_; break;
      default: break;
    }
    if (unknown) *unknown = u;
    return v;
  }

  // ---- The key request's answer from OTP ----
  bool key_pending() const { return key_pending_; }
  bool init_pending() const { return init_pending_; }
  void key_response(bool seed_valid) {
    if (!key_pending_) return;
    key_pending_ = false;
    garble("key renewal: the old contents are unreadable");
    keyed_ = true;
    status_ |= kScrKeyValid;
    status_ = seed_valid ? (status_ | kScrKeySeedValid) : (status_ & ~kScrKeySeedValid);
    status_unknown_ &= ~(kScrKeyValid | kScrKeySeedValid);
    rotated_ = kMuBi4True;
    rotated_unknown_ = false;
    ++key_renewals_;
    if (init_pending_) run_init();
  }

  // ---- Faults ----
  void bus_integrity_error() { local_escalation(kBusIntegError); }
  void init_counter_error() { local_escalation(kInitError); }
  void ram_encoding_error() { fatal(kSramAlert); }
  // A readback mismatch on an access: counts only while the readback check is enabled.
  void readback_mismatch() {
    if (readback_enabled()) fatal(kReadbackError);
  }
  bool readback_enabled() const { return readback_ != kMuBi4False; }
  bool fatal_alert() const { return fatal_; }
  bool escalated() const { return status_ & kEscalated; }
  bool blocked() const { return blocked_; }

  // ---- Execution ----
  bool exec_enabled() const {
    if (!p_.instr_exec) return false;
    if (otp_ifetch_ == kMuBi8True) return exec_ == kMuBi4True;
    return lc_debug_ == kLcOn;
  }

  // ---- Memory (byte offsets into the RAM window) ----
  Read mem_read(uint32_t off, bool fetch = false) {
    Read r;
    r.acc = gate(off, fetch);
    if (r.acc != Access::kOk) return r;
    uint32_t w = off & ~3u;
    const std::vector<uint8_t> &m = mem();
    for (unsigned i = 0; i < 4; ++i) {
      r.data |= uint32_t(m[w + i]) << (8 * i);
      if (state_[w + i] != kKnown) r.unknown |= 0xffu << (8 * i);
      if (state_[w + i] == kGarbage) r.garbage = true;
    }
    return r;
  }
  // be: byte enables of the word at off & ~3. A sub-word write is a read-modify-write (ToO): the
  // whole word gets fresh integrity bits, so a garbage byte it leaves becomes open.
  Access mem_write(uint32_t off, uint32_t data, uint8_t be = 0xf) {
    Access a = gate(off, false);
    if (a != Access::kOk) return a;
    uint32_t w = off & ~3u;
    std::vector<uint8_t> &m = mem();
    for (unsigned i = 0; i < 4; ++i) {
      if (be >> i & 1) {
        m[w + i] = uint8_t(data >> (8 * i));
        state_[w + i] = kKnown;
      } else if (state_[w + i] == kGarbage) {
        state_[w + i] = kOpen;
      }
    }
    return Access::kOk;
  }
  ByteState byte_state(uint32_t off) const { return ByteState(state_[off]); }
  // The physical word a logical word is stored in: the identity without address scrambling, -1
  // (a keyed S&P network, not modelled) with it.
  int64_t physical_word(uint32_t word) const {
    if (word >= words()) return -1;
    return p_.num_addr_scr_rounds == 0 ? int64_t(word) : -1;
  }

  // ---- Counts, for reports ----
  unsigned key_renewals() const { return key_renewals_; }
  unsigned inits() const { return inits_; }
  unsigned alert_tests() const { return alert_tests_; }

 private:
  void reset_regs() {
    alert_regwen_ = exec_regwen_ = ctrl_regwen_ = readback_regwen_ = true;
    exec_ = kMuBi4False;
    readback_ = kMuBi4False;
    rotated_ = kMuBi4False;
    rotated_unknown_ = false;
    status_ = 0;
    status_unknown_ = 0;
    key_pending_ = init_pending_ = false;
    blocked_ = false;
    fatal_ = false;
  }

  std::vector<uint8_t> &mem() {
    std::vector<uint8_t> *e = port_->memory();
    if (e && e->size() >= p_.bytes) return *e;
    return own_;
  }

  Access gate(uint32_t off, bool fetch) const {
    if (off >= p_.bytes) return Access::kError;
    if (blocked_) return Access::kBlocked;
    if (key_pending_ || init_pending_) return Access::kStall;
    if (fetch && !exec_enabled()) return Access::kRefused;
    return Access::kOk;
  }

  void ctrl(uint32_t w) {
    if (blocked_ || (status_ & kEscalated)) {
      // After an escalation the documents say nothing of CTRL: no effect on the memory, the key
      // and INIT bits open.
      if (w) {
        status_unknown_ |= kScrKeyValid | kScrKeySeedValid | kInitDone;
        rotated_unknown_ = true;
      }
      return;
    }
    if (w & 1) {
      if (key_pending_ || init_pending_) return;  // "has no effect"
      key_pending_ = true;
      status_ &= ~kScrKeyValid;
      if (w & 2) start_init_waiting();
      port_->key_request();  // may answer at once (key_response)
      return;
    }
    if (w & 2) {
      if (init_pending_) return;  // "has no effect"
      if (key_pending_) {
        start_init_waiting();  // waits for the key (not documented: as with RENEW|INIT)
        status_unknown_ |= kInitDone;
      } else {
        run_init();
      }
    }
  }
  void start_init_waiting() {
    init_pending_ = true;
    status_ &= ~kInitDone;
  }
  void run_init() {
    init_pending_ = false;
    std::vector<uint8_t> &m = mem();
    for (uint32_t i = 0; i < p_.bytes; ++i) {
      m[i] = p_.sec_zero_init ? 0 : next_byte();
      state_[i] = p_.sec_zero_init ? kKnown : kValid;
    }
    status_ |= kInitDone;
    status_unknown_ &= ~kInitDone;
    ++inits_;
  }

  void escalate() {
    if (status_ & kEscalated) return;
    status_ |= kEscalated;
    status_unknown_ |= kScrKeyValid | kScrKeySeedValid;
    rotated_ = kMuBi4False;
    rotated_unknown_ = false;
    to_default_key("escalation");
  }
  void local_escalation(uint32_t bit) {
    fatal(bit);
    if (blocked_) return;
    // interfaces.md: "The behavior of local escalation is identical to global escalation"; whether
    // that includes STATUS.ESCALATED ("has received an escalate request") is not said.
    status_unknown_ |= kScrKeyValid | kScrKeySeedValid | kEscalated;
    rotated_unknown_ = true;
    to_default_key(bit == kBusIntegError ? "bus integrity error" : "init counter error");
  }
  void to_default_key(const char *why) {
    key_pending_ = init_pending_ = false;
    blocked_ = true;
    if (keyed_) garble(std::string(why) + ": the predefined key again");
    keyed_ = false;
    port_->note(p_.name + ": " + why + ", memory blocked until reset");
  }
  void fatal(uint32_t bit) {
    status_ |= bit;
    if (!fatal_) {
      fatal_ = true;
      port_->fatal_alert();
    }
  }

  // Every byte changes (a non-zero XOR), and its value is no longer predicted.
  void garble(const std::string &why) {
    std::vector<uint8_t> &m = mem();
    for (uint32_t i = 0; i < p_.bytes; ++i) {
      m[i] ^= next_byte();
      state_[i] = kGarbage;
    }
    port_->note(p_.name + ": " + why);
  }
  uint8_t next_byte() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    return uint8_t(lfsr_ | 1);  // never 0
  }

  Params p_;
  Eg2SramCtrlPort null_port_;
  Eg2SramCtrlPort *port_;
  uint32_t decoded_ = 0;
  std::vector<uint8_t> own_, state_;
  bool alert_regwen_ = true, exec_regwen_ = true, ctrl_regwen_ = true, readback_regwen_ = true;
  uint32_t exec_ = kMuBi4False, readback_ = kMuBi4False, rotated_ = kMuBi4False;
  bool rotated_unknown_ = false;
  uint32_t status_ = 0, status_unknown_ = 0;
  bool key_pending_ = false, init_pending_ = false, keyed_ = false, blocked_ = false, fatal_ = false;
  uint32_t otp_ifetch_ = 0x69, lc_debug_ = kLcOff;  // MuBi8False, Off
  uint32_t lfsr_ = 0x1234567;
  unsigned key_renewals_ = 0, inits_ = 0, alert_tests_ = 0;
};

#endif  // EG2_MODEL_IP_EG2_SRAM_CTRL_CORE_H_
