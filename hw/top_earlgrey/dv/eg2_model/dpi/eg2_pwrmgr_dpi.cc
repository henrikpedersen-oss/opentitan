// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The power manager's reference model for the pwrmgr block bench (pwrmgr_cmodel_sb): the core of
// eg2_pwrmgr_core.h behind a C interface for SystemVerilog DPI (eg2_pwrmgr_cmodel_pkg.sv).
//
// From pwrmgr's documents (hw/top_earlgrey/ip_autogen/pwrmgr: data/pwrmgr.hjson,
// doc/theory_of_operation.md, doc/programmers_guide.md), never from its RTL.
//
// The register file is this binding's, with the access rules of pwrmgr.hjson: CTRL_CFG_REGWEN on
// CONTROL, WAKEUP_EN_REGWEN and RESET_EN_REGWEN (rw0c), INTR_STATE and WAKE_INFO (rw1c), INTR_TEST
// setting INTR_STATE, CFG_CDC_SYNC self-clearing, the read-only status registers. The inputs are the
// block's ports as levels (wake-ups, reset requests, the software reset request, escalation, WFI,
// the idle indications of otp_ctrl, lc_ctrl and the NVM controller, main power-ok), plus two of the
// documented handshakes: fetch_en (the fast FSM is in Active: "The processor is allowed to start
// executing, the fast FSM transitions to Active state") and the main power request to the AST (the
// power-ok checks are "valid only during states that power should be on": "any state where power
// manager has requested the power to be turned on").
//
// The documents give no cycle timing for the clock-domain crossings and the fast/slow FSM steps, and
// leave some outcomes open. A read therefore returns bounds, not one value: `lo` the bits that must
// read 1, `hi` the bits that may read 1 (a bit predicted exactly has lo == hi). Not predicted:
// - while the design settles (the scoreboard's `quiet` is 0: an input, a handshake or a
//   configuration write within its settle window), the fields hardware updates (hw_bits);
// - a CONTROL write while CTRL_CFG_REGWEN is not settled (the lock's timing is not documented);
// - WAKE_STATUS and RESET_STATUS with either the enables before or after a WAKEUP_EN / RESET_EN
//   write until a CFG_CDC_SYNC has followed it (programmers_guide.md: "Set and poll CFG_CDC_SYNC to
//   ensure above settings propagate across clock domains");
// - WAKE_INFO.REASONS for a wake-up level seen while capture runs ("The wake info recording begins
//   whenever the device begins a valid low power entry. This capture is continued until it is
//   explicitly disabled"; whether a source not enabled in WAKEUP_EN is captured is not written);
// - WAKE_INFO.FALL_THROUGH / ABORT and the wakeup interrupt when an entry may have fallen through
//   (WFI dropped during the entry) or aborted (an idle input low during the entry): the point at
//   which the fast FSM checks is not documented, and whether the interrupt is raised then is not
//   written (INTR_STATE: "Wake from low power state");
// - the wakeup interrupt after a reset request ended a low-power state (no wake-up);
// - ESCALATE_RESET_STATUS once the escalation request has dropped ("When 1, an escalation reset has
//   been seen": sticky, or the request's level).
// Predicted exactly: what software alone writes, CTRL_CFG_REGWEN when settled (0 from the low-power
// trigger until Active again: "It unlocks whenever a low power transition has completed (transition
// back to the ACTIVE state) for any reason"), CONTROL.LOW_POWER_HINT when settled (cleared "once the
// power manager begins a low power transition, or if a valid reset request is registered"),
// WAKE_STATUS / RESET_STATUS when settled ("current wake / reset requests post enable mask"),
// FAULT_STATUS.MAIN_PD_GLITCH (sticky), the reasons the core records for a wake-up from low power,
// ABORT for an entry with an idle input low throughout, the interrupt for a wake-up from low power.
//
// An internal reset the core requests does not reset pwrmgr's registers (pwrmgr is on the POR reset
// tree); only the bench's reset of the block does (eg2_pwr_reset).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../ip/eg2_pwrmgr_core.h"

namespace {

enum : uint32_t {
  kIntrState = 0x00, kIntrEnable = 0x04, kIntrTest = 0x08, kAlertTest = 0x0c,
  kCtrlCfgRegwen = 0x10, kControl = 0x14, kCfgCdcSync = 0x18, kWakeupEnRegwen = 0x1c,
  kWakeupEn = 0x20, kWakeStatus = 0x24, kResetEnRegwen = 0x28, kResetEn = 0x2c,
  kResetStatus = 0x30, kEscResetStatus = 0x34, kWakeInfoCaptureDis = 0x38, kWakeInfo = 0x3c,
  kFaultStatus = 0x40, kNumRegs = 0x44 / 4,
};
// LOW_POWER_HINT, CORE_CLK_EN, IO_CLK_EN, USB_CLK_EN_LP, USB_CLK_EN_ACTIVE, MAIN_PD_N
constexpr uint32_t kControlMask = 0x1f1;
constexpr uint32_t kFallThrough = 1u << Eg2PwrmgrCore::kFallThroughBit;
constexpr uint32_t kAbort = 1u << Eg2PwrmgrCore::kAbortBit;
constexpr uint32_t kMainPdGlitch = 1u << 2;

inline uint32_t I(uint32_t addr) { return (addr & 0x7f) / 4; }

class PwrDpi : public Eg2PwrmgrPort {
 public:
  PwrDpi() : core_(*this) { por(); }

  // The bench's reset of the block (POR).
  void por() {
    memset(r_, 0, sizeof(r_));
    memset(unk_, 0, sizeof(unk_));
    r_[I(kCtrlCfgRegwen)] = 1;
    r_[I(kControl)] = 0x180;  // USB_CLK_EN_ACTIVE 1, MAIN_PD_N 1
    r_[I(kWakeupEnRegwen)] = 1;
    r_[I(kResetEnRegwen)] = 1;
    wake_lvl_ = rst_lvl_ = 0;
    wake_en_old_ = rst_en_old_ = wake_en_now_ = rst_en_now_ = 0;
    cfg_pending_ = sync_after_cfg_ = false;
    esc_lvl_ = esc_seen_ = sleeping_ = sw_rst_ = false;
    main_pok_ = true;
    idle_ = true;
    active_ = false;
    pd_n_req_ = true;
    entry_ = rec_hw_ = false;
    resets_ = entries_ = 0;
    core_.after_reset(true, false, 0);
  }

  // ---- Software ----

  void write(uint32_t addr, uint32_t data, bool quiet) {
    uint32_t i = I(addr);
    switch (addr & 0x7f) {
      case kIntrState:
        r_[i] &= ~(data & 1);
        if (!entry_) unk_[i] &= ~(data & 1);
        break;
      case kIntrEnable: r_[i] = data & 1; break;
      case kIntrTest:
        if (data & 1) {
          r_[I(kIntrState)] |= 1;
          unk_[I(kIntrState)] &= ~1u;
        }
        break;
      case kControl: {
        uint32_t v = data & kControlMask;
        if (quiet) {
          if (r_[I(kCtrlCfgRegwen)] & 1) {
            r_[i] = v;
            unk_[i] = 0;
          }
        } else {
          // CTRL_CFG_REGWEN may be changing: the write may or may not have been taken.
          unk_[i] |= (r_[i] ^ v);
          r_[i] = v;
        }
        cfg_written();
        // "a combination of WFI and software low power hint": the hint written while WFI is up.
        if ((r_[i] & 1) && sleeping_ && active_ && !entry_) start_entry();
        break;
      }
      case kCfgCdcSync:
        if (data & 1) {
          if (cfg_pending_) sync_after_cfg_ = true;
          r_[i] = 1;
          core_.reg_written(Eg2PwrReg::kCdcSync);  // the core syncs at once and clears it
          // The self-clear's time is not documented: open until the design settles with no
          // low-power transition or Active edge since this write (a sync across one has been seen
          // to finish later, 2026-11-07, pwrmgr_cmodel_random seed 2).
          unk_[i] |= 1;
          cdc_clean_ = true;
        }
        break;
      case kWakeupEnRegwen: r_[i] &= data & 1; break;
      case kWakeupEn:
        if (r_[I(kWakeupEnRegwen)] & 1) {
          r_[i] = data & 0xff;
          core_.reg_written(Eg2PwrReg::kWakeEn);
          cfg_written();
        }
        break;
      case kResetEnRegwen: r_[i] &= data & 1; break;
      case kResetEn:
        if (r_[I(kResetEnRegwen)] & 1) {
          r_[i] = data & 3;
          cfg_written();
        }
        break;
      case kWakeInfoCaptureDis:
        r_[i] = data & 1;
        if (data & 1) rec_hw_ = false;  // "explicitly disabled"
        capture_may();
        break;
      case kWakeInfo:
        r_[i] &= ~(data & 0x3ff);
        if (!entry_) unk_[i] &= ~(data & 0x3ff);
        capture_may();
        break;
      default: break;  // ALERT_TEST, the read-only registers
    }
  }

  // Bounds of a read: lo must read 1, hi may read 1.
  // `model_view`: the model's own value, whatever the phase (the self-test, coverage context).
  void read(uint32_t addr, bool quiet, uint32_t *lo, uint32_t *hi, bool model_view = false) const {
    uint32_t i = I(addr), v = 0, u = 0;
    switch (addr & 0x7f) {
      case kIntrTest: case kAlertTest: break;
      case kWakeStatus: {
        uint32_t n = r_[I(kWakeupEn)], o = cfg_pending_ ? wake_en_old_ : n;
        v = wake_lvl_ & n & o;
        u = wake_lvl_ & (n ^ o);
        break;
      }
      case kResetStatus: {
        uint32_t n = r_[I(kResetEn)], o = cfg_pending_ ? rst_en_old_ : n;
        v = rst_lvl_ & n & o;
        u = rst_lvl_ & (n ^ o);
        break;
      }
      case kEscResetStatus:
        v = esc_lvl_;
        u = esc_seen_ && !esc_lvl_;
        break;
      default:
        if (i < kNumRegs) {
          v = r_[i];
          u = unk_[i];
        }
        break;
    }
    // Out of Active (or between the trigger and the end of the entry) pwrmgr is in its power
    // sequence, whose steps the documents do not time; it can be quiet at its ports for longer than
    // the scoreboard's settle window there (2026-11-07, pwrmgr_cmodel_random seed 4: the wakeup
    // interrupt during a deep-sleep entry).
    if (!model_view && (!quiet || !active_ || entry_)) u |= hw_bits(addr);
    *lo = v & ~u;
    *hi = v | u;
  }

  // The fields hardware updates (pwrmgr.hjson hwaccess hwo / hrw).
  static uint32_t hw_bits(uint32_t addr) {
    switch (addr & 0x7f) {
      case kIntrState: return 1;
      case kCtrlCfgRegwen: return 1;
      case kControl: return 1;  // LOW_POWER_HINT
      case kCfgCdcSync: return 1;
      case kWakeStatus: return 0xff;
      case kResetStatus: return 3;
      case kEscResetStatus: return 1;
      case kWakeInfo: return 0x3ff;
      case kFaultStatus: return 7;
      default: return 0;
    }
  }

  // The wakeup interrupt output's bounds (INTR_STATE & INTR_ENABLE).
  void intr(bool quiet, uint32_t *lo, uint32_t *hi) const {
    uint32_t l, h;
    read(kIntrState, quiet, &l, &h);
    *lo = l & r_[I(kIntrEnable)] & 1;
    *hi = h & r_[I(kIntrEnable)] & 1;
  }

  // The design has been quiet for the scoreboard's settle window.
  void settled() {
    if (cdc_clean_) unk_[I(kCfgCdcSync)] &= ~1u;
    if (sync_after_cfg_ && cdc_clean_) {
      cfg_pending_ = sync_after_cfg_ = false;
      wake_en_old_ = r_[I(kWakeupEn)];
      rst_en_old_ = r_[I(kResetEn)];
    }
    // Quiet in Active after a trigger: the fast FSM never left Active (or came back, which
    // active_'s edge handles): the entry is over.
    if (entry_ && active_) end_entry();
  }

  // ---- Ports ----

  void inputs(uint32_t wake, uint32_t rst, bool esc, bool sleeping, bool sw_rst, bool main_pok,
              bool idle, bool active, bool pd_n_req) {
    pd_n_req_ = pd_n_req;
    idle_ = idle;
    if (active != active_) cdc_clean_ = false;
    if (active && !active_) {
      active_ = true;
      if (entry_) end_entry();
      // "Once in Active state, if the low power entry request is still present, transition to low
      // power state."
      if (sleeping && (r_[I(kControl)] & 1)) start_entry();
    }
    if (!active) active_ = false;

    for (unsigned s = 0; s < Eg2PwrmgrCore::kWakeCount; ++s) {
      bool now = (wake >> s) & 1, was = (wake_lvl_ >> s) & 1;
      if (now && !was) {
        wake_lvl_ |= 1u << s;
        if (entry_ && ((r_[I(kWakeupEn)] >> s) & 1)) woke_ = true;
        call([&] { core_.wakeup(s); });
      }
      if (!now && was) {
        wake_lvl_ &= ~(1u << s);
        core_.wakeup_clear(s);
      }
    }
    capture_may();

    uint32_t rose = rst & ~rst_lvl_ & 3;
    rst_lvl_ = rst & 3;
    for (unsigned s = 0; s < 2; ++s) {
      if ((rose >> s) & 1) {
        if (entry_ && ((r_[I(kResetEn)] >> s) & 1)) woke_ = true;
        call([&] { core_.reset_request(s); });
      }
    }
    if (sw_rst && !sw_rst_) {
      if (entry_) woke_ = true;
      call([&] { core_.sw_reset_request(); });
    }
    sw_rst_ = sw_rst;
    if (esc && !esc_lvl_) {
      esc_seen_ = true;
      if (entry_) woke_ = true;
      call([&] { core_.escalation(true); });
    }
    esc_lvl_ = esc;
    if (!main_pok && main_pok_ && pd_n_req_) {
      call([&] {
        if (!core_.main_pok_glitch()) {
          // This binding does not inject events inside lowpower_event(). If
          // that changes, none of the ensuing reset state is predictable.
          for (auto &mask : unk_) mask = 0xffffffffu;
          note_ = "undocumented: main power glitch during deep-sleep entry";
        }
      });
      r_[I(kFaultStatus)] |= kMainPdGlitch;  // the request to the AST says power is meant to be on
    }
    main_pok_ = main_pok;

    if (entry_ && !woke_) {
      // The fast FSM's checks (theory_of_operation.md "Fall Through Handling", "Abort Handling")
      // are untimed: WFI dropping or an idle input low during the entry may be either.
      if (!sleeping && sleeping_) maybe_exception(kFallThrough);
      if (!idle) maybe_exception(kAbort);
      if (idle && abort_all_along_) {
        // The idle input rose again during the entry: the abort is no longer certain.
        abort_all_along_ = false;
        r_[I(kWakeInfo)] &= ~kAbort;
        maybe_exception(kAbort);
      }
    }
    if (sleeping && !sleeping_ && active_ && (r_[I(kControl)] & 1)) start_entry();
    sleeping_ = sleeping;
  }

  uint32_t resets() const { return resets_; }
  uint32_t entries() const { return entries_; }
  bool core_low_power() const { return core_.low_power(); }
  const std::string &last_note() const { return note_; }

  // ---- Eg2PwrmgrPort ----
  uint32_t rd(Eg2PwrReg r) override { return r_[I(off(r))]; }
  void wr(Eg2PwrReg r, uint32_t v) override {
    r_[I(off(r))] = v;
    // The core writes CONTROL only to clear the hint (the end of a low-power state).
    if (r == Eg2PwrReg::kControl) unk_[I(kControl)] &= ~1u;
  }
  void set_wakeup_intr() override { intr_pending_ = true; }
  void request_reset(Eg2PwrReset why, const std::string &what, int, bool) override {
    // "if a valid reset request is registered, this bit is automatically cleared by HW"
    clear_hint();
    ++resets_;
    reset_why_ = why;
    reset_now_ = true;
    note_ = what;
  }
  void note(const std::string &s) override { note_ = s; }

 private:
  // A core call: the wakeup interrupt it raises is predicted for a wake-up; after a reset request
  // that ended low power it may or may not be raised.
  template <typename F>
  void call(F f) {
    intr_pending_ = reset_now_ = false;
    f();
    if (intr_pending_) {
      if (reset_now_ && reset_why_ != Eg2PwrReset::kLowPowerExit) {
        unk_[I(kIntrState)] |= 1;
      } else {
        r_[I(kIntrState)] |= 1;
        unk_[I(kIntrState)] &= ~1u;
      }
    }
    intr_pending_ = reset_now_ = false;
  }

  void clear_hint() {
    r_[I(kControl)] &= ~1u;
    unk_[I(kControl)] &= ~1u;
  }

  void cfg_written() {
    if (!cfg_pending_) {
      wake_en_old_ = wake_en_now_;
      rst_en_old_ = rst_en_now_;
    }
    cfg_pending_ = true;
    sync_after_cfg_ = false;
    wake_en_now_ = r_[I(kWakeupEn)];
    rst_en_now_ = r_[I(kResetEn)];
  }

  // The low-power trigger: WFI with the hint set, in Active.
  void start_entry() {
    entry_ = true;
    cdc_clean_ = false;
    woke_ = exc_ = false;
    ++entries_;
    r_[I(kCtrlCfgRegwen)] = 0;
    rec_hw_ = true;  // "recording begins when the fast FSM transitions out of Active state"
    abort_all_along_ = !idle_;
    call([&] { core_.enter_low_power(idle_); });
    if (!idle_) maybe_exception(0);  // the interrupt on an abort is not documented
    clear_hint();
    capture_may();
  }

  void end_entry() {
    entry_ = false;
    r_[I(kCtrlCfgRegwen)] = 1;
    if (core_.low_power()) {
      // Active again with no wake-up or reset request the core took and no sign of a fall through
      // or abort at the ports: either may have happened.
      if (!exc_) maybe_exception(kFallThrough | kAbort);
      core_.resume();
    }
  }

  // An exception (fall through / abort, `bits`) may have happened: those WAKE_INFO bits and the
  // interrupt are not predicted until software clears them.
  void maybe_exception(uint32_t bits) {
    exc_ = true;
    if (!(r_[I(kWakeInfoCaptureDis)] & 1)) unk_[I(kWakeInfo)] |= bits & ~r_[I(kWakeInfo)];
    unk_[I(kIntrState)] |= 1 & ~r_[I(kIntrState)];
  }

  // While capture runs, any wake-up level may be captured.
  void capture_may() {
    if (rec_hw_ && !(r_[I(kWakeInfoCaptureDis)] & 1)) {
      unk_[I(kWakeInfo)] |= wake_lvl_ & ~r_[I(kWakeInfo)] & 0xff;
    }
  }

  static uint32_t off(Eg2PwrReg r) {
    switch (r) {
      case Eg2PwrReg::kControl: return kControl;
      case Eg2PwrReg::kCdcSync: return kCfgCdcSync;
      case Eg2PwrReg::kWakeEn: return kWakeupEn;
      case Eg2PwrReg::kWakeStatus: return kWakeStatus;
      case Eg2PwrReg::kResetEn: return kResetEn;
      case Eg2PwrReg::kResetStatus: return kResetStatus;
      case Eg2PwrReg::kWakeInfo: return kWakeInfo;
      case Eg2PwrReg::kCaptureDis: return kWakeInfoCaptureDis;
      case Eg2PwrReg::kEscStatus: return kEscResetStatus;
      case Eg2PwrReg::kFaultStatus: return kFaultStatus;
    }
    return kControl;
  }

  uint32_t r_[kNumRegs];
  uint32_t unk_[kNumRegs];
  uint32_t wake_lvl_ = 0, rst_lvl_ = 0;
  uint32_t wake_en_old_ = 0, rst_en_old_ = 0, wake_en_now_ = 0, rst_en_now_ = 0;
  bool cfg_pending_ = false, sync_after_cfg_ = false;
  bool esc_lvl_ = false, esc_seen_ = false, sleeping_ = false, sw_rst_ = false;
  bool main_pok_ = true, idle_ = true, active_ = false, pd_n_req_ = true;
  bool cdc_clean_ = true;
  bool entry_ = false, woke_ = false, exc_ = false, rec_hw_ = false, abort_all_along_ = false;
  bool intr_pending_ = false, reset_now_ = false;
  Eg2PwrReset reset_why_ = Eg2PwrReset::kHwRequest;
  uint32_t resets_ = 0, entries_ = 0;
  std::string note_;
  Eg2PwrmgrCore core_;
};

PwrDpi *H(void *h) { return static_cast<PwrDpi *>(h); }

}  // namespace

extern "C" {

void *eg2_pwr_create() { return new PwrDpi(); }
void eg2_pwr_release(void *h) { delete H(h); }
void eg2_pwr_reset(void *h) { H(h)->por(); }
void eg2_pwr_csr_write(void *h, unsigned int addr, unsigned int data, int quiet) {
  H(h)->write(addr & 0x7c, data, quiet != 0);
}
void eg2_pwr_csr_read(void *h, unsigned int addr, int quiet, unsigned int *lo, unsigned int *hi) {
  uint32_t l, u;
  H(h)->read(addr & 0x7c, quiet != 0, &l, &u);
  *lo = l;
  *hi = u;
}
void eg2_pwr_intr(void *h, int quiet, unsigned int *lo, unsigned int *hi) {
  uint32_t l, u;
  H(h)->intr(quiet != 0, &l, &u);
  *lo = l;
  *hi = u;
}
void eg2_pwr_inputs(void *h, unsigned int wake, unsigned int rst, int esc, int sleeping,
                    int sw_rst, int main_pok, int idle, int active, int pd_n_req) {
  H(h)->inputs(wake, rst, esc, sleeping, sw_rst, main_pok, idle, active, pd_n_req);
}
void eg2_pwr_settled(void *h) { H(h)->settled(); }
// The model's value of a register (the bits it predicts when settled in Active), any phase.
unsigned int eg2_pwr_csr_value(void *h, unsigned int addr) {
  uint32_t l, u;
  H(h)->read(addr & 0x7c, true, &l, &u, true);
  return l;
}
unsigned int eg2_pwr_resets(void *h) { return H(h)->resets(); }
unsigned int eg2_pwr_entries(void *h) { return H(h)->entries(); }
int eg2_pwr_core_low_power(void *h) { return H(h)->core_low_power(); }
const char *eg2_pwr_last_note(void *h) { return H(h)->last_note().c_str(); }

}  // extern "C"
