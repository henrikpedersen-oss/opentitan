// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The power manager's behaviour as the EG2 model has it, in plain C++ (no SystemC): one core for
// two bindings, as the debug module's (eg2_dm_core.h):
//   eg2sim      Eg2Pwrmgr (eg2_aon.h), on the SystemC model's register store and system events;
//   block bench eg2_pwrmgr_dpi.cc, its own register file, driven by the pwrmgr block bench's ports
//               and TL-UL accesses (pwrmgr_cmodel_sb).
// The core reaches its registers and the system only through Eg2PwrmgrPort.
//
// From pwrmgr's documents (hw/top_earlgrey/ip_autogen/pwrmgr: doc/theory_of_operation.md,
// doc/programmers_guide.md, data/pwrmgr.hjson), never from its RTL. CFG_CDC_SYNC is done at once
// (the model has no clock domains). A WFI with CONTROL.LOW_POWER_HINT set enters low power
// unless an enabled interrupt is already pending and CONTROL.CORE_CLK_EN keeps the core clock on
// (the immediate fall through, WAKE_INFO.FALL_THROUGH). In low power an enabled wake-up
// (WAKEUP_EN) records its reason in WAKE_INFO, clears the hint and raises the wakeup interrupt.
// From deep sleep (CONTROL.MAIN_PD_N 0 at the entry) the chip wakes through a reset recorded as a
// low-power exit and restarts in ROM (programmers_guide.md "Exiting from Deep Sleep"). From normal
// sleep (MAIN_PD_N 1: "All power domains are kept on for fast resume", theory_of_operation.md
// "Normal Sleep") there is no reset: software "continues executing the instruction after the
// wait-for-interrupt invocation" (programmers_guide.md "Exiting from Normal Sleep"). An enabled
// peripheral reset request (RESET_EN) resets the chip in any state. A reset request in low power is
// recorded as a low-power exit too only after a deep-sleep entry (rstmgr theory_of_operation.md
// "Reset Information": "Low power entry reset has been triggered by pwrmgr").
//
// Escalation (theory_of_operation.md "Escalation Reset Request"): the alert handler's escalation
// resets the chip at once, in any state; ESCALATE_RESET_STATUS reads 1 while the request is seen.
// The escalation clock timeout is not modelled.
//
// Aborts (theory_of_operation.md "Abort Handling": a non-volatile operation of otp_ctrl, lc_ctrl or
// the NVM controller in flight at the entry) are taken when the binding reports the idle inputs
// (eg2sim has none and never aborts).

#ifndef EG2_MODEL_IP_EG2_PWRMGR_CORE_H_
#define EG2_MODEL_IP_EG2_PWRMGR_CORE_H_

#include <cstdint>
#include <string>

// The registers the core reads and writes (pwrmgr.hjson).
enum class Eg2PwrReg : unsigned {
  kControl, kCdcSync, kWakeEn, kWakeStatus, kResetEn, kResetStatus, kWakeInfo, kCaptureDis,
  kEscStatus, kFaultStatus,
};

// Why the core asks for a reset.
enum class Eg2PwrReset : unsigned { kLowPowerExit, kHwRequest, kEscalation, kSwRequest };

// What the core needs from its binding.
class Eg2PwrmgrPort {
 public:
  virtual ~Eg2PwrmgrPort() = default;
  virtual uint32_t rd(Eg2PwrReg r) = 0;
  virtual void wr(Eg2PwrReg r, uint32_t v) = 0;
  virtual void set_wakeup_intr() = 0;
  // `idx`: the request index rstmgr records (-1: none); `lp_reset`: a low-power entry reset was
  // made, so rstmgr records a low-power exit as well.
  virtual void request_reset(Eg2PwrReset why, const std::string &what, int idx, bool lp_reset) = 0;
  // The low-power entry as the chip sees it (eg2sim: chip_event "lowpower").
  virtual void lowpower_event() {}
  virtual bool reset_pending() { return false; }
  virtual void note(const std::string &) {}
};

class Eg2PwrmgrCore {
 public:
  static constexpr unsigned kWakeCount = 8;  // pwrmgr.hjson NumWkups (top_earlgrey)
  static constexpr unsigned kFallThroughBit = kWakeCount;      // WAKE_INFO.FALL_THROUGH
  static constexpr unsigned kAbortBit = kWakeCount + 1;        // WAKE_INFO.ABORT

  explicit Eg2PwrmgrCore(Eg2PwrmgrPort &p) : p_(p) {}

  // After a reset the binding has applied to the registers (`fault`: FAULT_STATUS as it was
  // before: pwrmgr is on the POR reset tree, so it survives every internally requested reset).
  void after_reset(bool por, bool low_power_exit, uint32_t fault) {
    if (!por) p_.wr(Eg2PwrReg::kFaultStatus, fault);
    low_power_ = false;
    p_.wr(Eg2PwrReg::kEscStatus, 0);
    // A reset other than a low-power exit resets the wake-up sources too: their requests drop.
    if (!low_power_exit) {
      wake_req_ = 0;
      show_wake_status();
    }
    // The synchronised copy follows CONTROL as the reset leaves it (resval 1).
    usb_clk_active_ = (p_.rd(Eg2PwrReg::kControl) >> 7) & 1;
  }

  // A wake-up request from source src: a level, until the source withdraws it (wakeup_clear); it
  // wakes the chip from low power whenever its WAKEUP_EN bit is set, also if it came first.
  void wakeup(unsigned src) {
    wake_req_ |= 1u << src;
    show_wake_status();
    if (!((p_.rd(Eg2PwrReg::kWakeEn) >> src) & 1)) return;
    if (!low_power_) return;
    if (!(p_.rd(Eg2PwrReg::kCaptureDis) & 1)) {
      p_.wr(Eg2PwrReg::kWakeInfo, p_.rd(Eg2PwrReg::kWakeInfo) | (1u << src));
    }
    bool deep = deep_sleep_;
    exit_low_power();
    if (!deep) {
      p_.note("pwrmgr: wake-up " + std::to_string(src) + " from normal sleep, no reset");
      return;
    }
    p_.request_reset(Eg2PwrReset::kLowPowerExit, "low-power exit, wake-up " + std::to_string(src),
                     -1, false);
  }

  void wakeup_clear(unsigned src) {
    wake_req_ &= ~(1u << src);
    show_wake_status();
  }

  // A peripheral reset request from source src (index in the top's peripheral reset requests).
  void reset_request(unsigned src) {
    if (!((p_.rd(Eg2PwrReg::kResetEn) >> src) & 1)) return;
    p_.wr(Eg2PwrReg::kResetStatus, p_.rd(Eg2PwrReg::kResetStatus) | (1u << src));
    bool asleep = low_power_reset();
    exit_low_power();
    p_.request_reset(Eg2PwrReset::kHwRequest, "peripheral reset request " + std::to_string(src),
                     int(src), asleep);
  }

  // The software reset request rstmgr forwards (theory_of_operation.md "Reset Request Handling").
  void sw_reset_request() {
    bool asleep = low_power_reset();
    exit_low_power();
    p_.request_reset(Eg2PwrReset::kSwRequest, "software reset request", -1, asleep);
  }

  // The alert handler's escalation reset request (a level).
  void escalation(bool level) {
    if (!level) return;
    p_.wr(Eg2PwrReg::kEscStatus, 1);
    bool asleep = low_power_reset();
    exit_low_power();
    p_.note("pwrmgr: escalation reset request (esc_rst)");
    p_.request_reset(Eg2PwrReset::kEscalation, "alert escalation reset (pwrmgr esc_rst)", -1,
                     asleep);
  }

  // theory_of_operation.md "Main Power Unstable Reset Requests": an internal reset, like the
  // escalation's, only while the main domain is meant to be on; FAULT_STATUS.MAIN_PD_GLITCH is
  // sticky (pwrmgr is on the POR reset tree).
  // False means the reset classification is undocumented at the entry boundary;
  // the binding must stop judging it instead of selecting LOW_POWER_EXIT.
  bool main_pok_glitch() {
    if (low_power_ && deep_sleep_ && !entering_) {
      p_.note("pwrmgr: main power-ok low while the main domain is powered down: no glitch reset");
      return true;
    }
    p_.wr(Eg2PwrReg::kFaultStatus, p_.rd(Eg2PwrReg::kFaultStatus) | (1u << 2));
    if (entering_ && deep_sleep_) return false;
    bool lp_reset = low_power_reset();
    exit_low_power();
    p_.note("pwrmgr: main power glitch: internal reset request");
    p_.request_reset(Eg2PwrReset::kHwRequest, "main power glitch (pwrmgr internal reset)", 2,
                     lp_reset);
    return true;
  }

  // The core asks for low power (WFI). `idle`: otp_ctrl, lc_ctrl and the NVM controller idle
  // (theory_of_operation.md "Abort Handling"; eg2sim: always). False if the hint is not set.
  bool enter_low_power(bool idle = true) {
    if (!(p_.rd(Eg2PwrReg::kControl) & 1)) return false;
    if (!idle) {
      // Aborted: the system returns to active and WAKE_INFO.ABORT records it (pwrmgr.hjson).
      if (!(p_.rd(Eg2PwrReg::kCaptureDis) & 1)) {
        p_.wr(Eg2PwrReg::kWakeInfo, p_.rd(Eg2PwrReg::kWakeInfo) | (1u << kAbortBit));
      }
      p_.note("pwrmgr: low-power entry aborted (a non-volatile operation in flight)");
      return true;
    }
    low_power_ = true;
    deep_sleep_ = !((p_.rd(Eg2PwrReg::kControl) >> 8) & 1);  // MAIN_PD_N 0: main powered down
    entering_ = true;
    p_.lowpower_event();
    entering_ = false;
    if (p_.reset_pending()) return true;
    // A wake-up already requested and enabled wakes the chip at once.
    uint32_t w = p_.rd(Eg2PwrReg::kWakeStatus) & p_.rd(Eg2PwrReg::kWakeEn);
    for (unsigned s = 0; s < kWakeCount; ++s) {
      if ((w >> s) & 1) {
        wakeup(s);
        break;
      }
    }
    return true;
  }

  // theory_of_operation.md "Fall Through Handling": right after WFI, an interrupt can make the
  // entry invalid and return the system to active (normal sleep with the core clock kept on).
  void interrupt_wakeup() {
    if (!low_power_ || deep_sleep_ || !(p_.rd(Eg2PwrReg::kControl) & (1u << 4))) return;
    if (!(p_.rd(Eg2PwrReg::kCaptureDis) & 1)) {
      p_.wr(Eg2PwrReg::kWakeInfo, p_.rd(Eg2PwrReg::kWakeInfo) | (1u << kFallThroughBit));
    }
    p_.note("pwrmgr: processor interrupt cancelled normal-sleep entry (fall through)");
    exit_low_power(false);
  }

  // The block bench's binding: the device is active again although the core saw no wake-up (a fall
  // through or abort the binding cannot time); no interrupt is raised here.
  void resume() { exit_low_power(false); }

  // A register written by software (after the binding stored the value).
  void reg_written(Eg2PwrReg r) {
    if (r == Eg2PwrReg::kWakeEn) show_wake_status();
    if (r == Eg2PwrReg::kCdcSync) {
      usb_clk_active_ = (p_.rd(Eg2PwrReg::kControl) >> 7) & 1;
      p_.wr(Eg2PwrReg::kCdcSync, 0);
    }
  }

  bool low_power() const { return low_power_; }
  bool deep_sleep() const { return deep_sleep_; }
  bool usb_clk_active() const { return usb_clk_active_; }
  uint32_t wake_requests() const { return wake_req_; }

 private:
  bool low_power_reset() const { return low_power_ && deep_sleep_; }
  void exit_low_power(bool raise_wakeup_interrupt = true) {
    if (!low_power_) return;
    low_power_ = false;
    p_.wr(Eg2PwrReg::kControl, p_.rd(Eg2PwrReg::kControl) & ~1u);
    if (raise_wakeup_interrupt) p_.set_wakeup_intr();
  }
  void show_wake_status() {
    p_.wr(Eg2PwrReg::kWakeStatus, wake_req_ & p_.rd(Eg2PwrReg::kWakeEn));
  }

  Eg2PwrmgrPort &p_;
  bool usb_clk_active_ = true;
  bool low_power_ = false;
  bool deep_sleep_ = false;
  bool entering_ = false;
  uint32_t wake_req_ = 0;
};

#endif  // EG2_MODEL_IP_EG2_PWRMGR_CORE_H_
