// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The always-on IPs of the EG2 SystemC model (milestone M3): pwrmgr, rstmgr, aon_timer and
// sensor_ctrl's AST status. From each IP's documentation (hw/ip/aon_timer, hw/top_earlgrey/
// ip_autogen/{pwrmgr,rstmgr}, hw/top_earlgrey/ip/sensor_ctrl: README.md and doc/).
//
// Wake-up and reset-request indices follow the top (top_earlgrey.gen.hjson): wake-ups sysrst_ctrl
// 0, adc_ctrl 1, pinmux 2 (pin) and 3 (USB), aon_timer 4, sensor_ctrl 5; peripheral reset requests
// sysrst_ctrl 0, aon_timer 1. rstmgr's RESET_INFO.HW_REQ holds the peripheral requests, then the
// main power glitch, the escalation and the debug module's NDM request; that order is from the
// top's reset_requests list (peripheral, internal, debug) and the field's width, the documentation
// does not spell it out.

#ifndef EG2_MODEL_IP_EG2_AON_H_
#define EG2_MODEL_IP_EG2_AON_H_

#include <cstdint>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_core_system.h"
#include "eg2_pwrmgr_core.h"
#include "eg2_rstmgr_core.h"

namespace eg2_model {


// rstmgr: RESET_REQ written with MuBi4True is a software reset. RESET_INFO records every reset's
// cause (POR, LOW_POWER_EXIT, SW_RESET, HW_REQ) and keeps it through resets until software clears
// it; a power-on reset sets POR alone. A reset requested after a deep-sleep entry records
// LOW_POWER_EXIT with its own cause (theory_of_operation.md "Reset Information"; Eg2Pwrmgr). The
// per-IP software resets (SW_RST_CTRL_N) reset their block's model when written 0. Every reset but a
// power-on reset or a low-power exit also resets the registers of clk_i's domain (rst_ni is the
// lc_io_div4 leaf; see on_reset).
//
// The POR_N pad, as the chip bench pulses it (assert_por_reset): eg2sim --stim rstmgr.por=1@<point>
// is a power-on reset ("If this reset ever asserts, everything in the design is reset",
// theory_of_operation.md "POR Reset Tree"; "POR_N pad toggle", "Summary").
//
// Crash dump (doc/theory_of_operation.md "Crash Dump Information"): with ALERT_INFO_CTRL.EN set, a
// reset captures the alert handler's crash dump (its alert_dump input, Eg2System::alert_crashdump);
// ALERT_INFO_ATTR.CNT_AVAIL is the dump's number of 32-bit words ("the number of 32-bit values
// contained in the alert info dump", whether or not one was captured: they read 0 until then),
// ALERT_INFO the word ALERT_INFO_CTRL.INDEX selects. EN is cleared by every reset ("even if rstmgr
// is not reset"). The CPU dump (CPU_INFO_CTRL, CPU_INFO_ATTR, CPU_INFO) works the same way with the
// core's crash dump (Eg2System::cpu_crashdump: 8 words).
class Eg2Rstmgr : public Eg2IpModel, private Eg2RstmgrPort {
 public:
  Eg2Rstmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), core_(*this) {
    req_ = reg("RESET_REQ");
    info_ = reg("RESET_INFO");
    alert_ctrl_ = reg("ALERT_INFO_CTRL");
    alert_attr_ = reg("ALERT_INFO_ATTR");
    alert_info_ = reg("ALERT_INFO");
    cpu_ctrl_ = reg("CPU_INFO_CTRL");
    cpu_attr_ = reg("CPU_INFO_ATTR");
    cpu_info_ = reg("CPU_INFO");
    for (uint16_t r : {info_, alert_ctrl_, alert_attr_, alert_info_, cpu_attr_, cpu_info_}) {
      set_modelled(r);
    }
    for (unsigned i = 0; i < kSwResets; ++i) {
      sw_rst_[i] = reg(("SW_RST_CTRL_N_" + std::to_string(i)).c_str());
      sw_regwen_[i] = reg(("SW_RST_REGWEN_" + std::to_string(i)).c_str());
    }
    alert_regwen_ = reg("ALERT_REGWEN");
    cpu_regwen_ = reg("CPU_REGWEN");
    err_code_ = reg("ERR_CODE");
    sys_.held_in_reset = [this](const std::string &b) {
      for (unsigned i = 0; i < kSwResets; ++i) {
        if (b == kSwResetBlocks[i]) return !(value(sw_rst_[i]) & 1);
      }
      return false;
    };
    sys_.stim_handlers["rstmgr.por"] = [this](const std::string &) {
      sys_.request_reset(Eg2ResetCause::kPor, "power-on reset (POR_N pad)");
      return true;
    };
    sys_.stim_handlers["rstmgr.hw_reset"] = [this](const std::string &) {
      // pwrmgr theory_of_operation.md, "Reset Request Handling": "For reset requests, fall
      // through and aborts are not checked and the system simply resets directly."
      sys_.request_reset(Eg2ResetCause::kHwRequest, "life-cycle transition reset request");
      return true;
    };
  }

  // A triggered reset captures the enabled crash dumps (eg2_rstmgr_core.h).
  void before_reset(Eg2ResetCause c) override {
    if (c == Eg2ResetCause::kPor) return;
    std::vector<uint32_t> alert, cpu;
    bool alert_en = (value(alert_ctrl_) & 1) && sys_.alert_crashdump;
    bool cpu_en = (value(cpu_ctrl_) & 1) && sys_.cpu_crashdump;
    if (alert_en) alert = sys_.alert_crashdump();
    if (cpu_en) cpu = sys_.cpu_crashdump();
    core_.triggered_reset(alert, cpu, alert_en, cpu_en);
  }

  void on_reset(Eg2ResetCause c) override {
    if (c == Eg2ResetCause::kPor) {
      Eg2IpModel::on_reset(c);
      core_.power_on_reset();
      // eg2sim's power-on reset ends with the processor running: recording is allowed.
      core_.processor_reset_released();
      return;
    }
    core_.clear_enables();
    // rst_ni is the always-on lc_io_div4 leaf (top_earlgrey.gen.hjson reset_connections): every
    // reset but a low-power exit resets the registers of clk_i's domain (all but those rstmgr.hjson
    // marks `sync: clk_por_i`) to their reset values. 2026-11-07: SW_RST_CTRL_N and the REGWENs used
    // to survive a hardware or software reset here.
    if (c != Eg2ResetCause::kLowPowerExit) {
      for (unsigned i = 0; i < kSwResets; ++i) {
        set_value(sw_rst_[i], 1);
        set_value(sw_regwen_[i], 1);
      }
      set_value(alert_regwen_, 1);
      set_value(cpu_regwen_, 1);
      set_value(err_code_, 0);
    }
    uint32_t hw = 0;
    switch (c) {
      case Eg2ResetCause::kHwRequest:
        hw = sys_.reset_hw_req() >= 0 ? 1u << sys_.reset_hw_req() : 0;
        break;
      case Eg2ResetCause::kEscalation: hw = 1u << 3; break;  // HW_REQ b6
      case Eg2ResetCause::kNdm: hw = 1u << 4; break;         // HW_REQ b7
      default: break;
    }
    bool lp = c == Eg2ResetCause::kLowPowerExit ||
              (c != Eg2ResetCause::kLowPowerExit && sys_.reset_in_low_power());
    core_.record(hw, c == Eg2ResetCause::kSoftware, lp);
    core_.reset_complete();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == req_ && core_.sw_reset_requested()) {
      sys_.request_reset(Eg2ResetCause::kSoftware, "software reset (rstmgr RESET_REQ)");
    }
    // A peripheral's software reset: SW_RST_CTRL_N_<i> written 0 holds the block in reset (its
    // registers back to their reset values, and an access to it hangs: held_in_reset) until it is
    // written 1 (theory_of_operation.md "Software Controlled Resets").
    for (unsigned i = 0; i < kSwResets; ++i) {
      if (r != sw_rst_[i] || (value(r) & 1) || !*kSwResetBlocks[i]) continue;
      if (Eg2IpModel *m = sys_.model(kSwResetBlocks[i])) m->on_reset(Eg2ResetCause::kSoftware);
    }
  }
  void reg_before_read(uint16_t r) override {
    if (r == alert_attr_) {
      // The dump's size, captured or not (the words read 0 until a capture).
      set_value(alert_attr_, sys_.alert_crashdump ? uint32_t(sys_.alert_crashdump().size()) & 0xf
                                                  : 0);
    } else if (r == alert_info_) {
      set_value(alert_info_, Eg2RstmgrCore::word(core_.alert_dump(), (value(alert_ctrl_) >> 4) & 0xf));
    } else if (r == cpu_attr_) {
      set_value(cpu_attr_, sys_.cpu_crashdump ? 8 : 0);
    } else if (r == cpu_info_) {
      set_value(cpu_info_, Eg2RstmgrCore::word(core_.cpu_dump(), (value(cpu_ctrl_) >> 4) & 0xf));
    }
  }

 private:
  // Eg2RstmgrPort.
  uint16_t id(Eg2RstReg r) const {
    switch (r) {
      case Eg2RstReg::kResetReq: return req_;
      case Eg2RstReg::kResetInfo: return info_;
      case Eg2RstReg::kAlertCtrl: return alert_ctrl_;
      case Eg2RstReg::kCpuCtrl: return cpu_ctrl_;
    }
    return info_;
  }
  uint32_t rd(Eg2RstReg r) override { return value(id(r)); }
  void wr(Eg2RstReg r, uint32_t v) override { set_value(id(r), v); }
  void note(const std::string &s) override { sys_.note(s); }

  Eg2RstmgrCore core_;
  uint16_t req_, info_, alert_ctrl_, alert_attr_, alert_info_, cpu_ctrl_, cpu_attr_, cpu_info_;
  // SW_RST_CTRL_N_<i>: the blocks in the order of rstmgr.hjson's SW_RST countermeasures (usbdev_aon
  // has no model of its own).
  static constexpr unsigned kSwResets = 10;
  static constexpr const char *kSwResetBlocks[kSwResets] = {
      "spi_device", "spi_host0", "spi_host1", "usbdev", "", "i2c0", "i2c1", "i2c2", "i3c0", "i3c1"};
  uint16_t sw_rst_[kSwResets], sw_regwen_[kSwResets];
  uint16_t alert_regwen_, cpu_regwen_, err_code_;
};

// pwrmgr (doc/theory_of_operation.md, doc/programmers_guide.md). CFG_CDC_SYNC is done at once
// (the model has no clock domains). A WFI with CONTROL.LOW_POWER_HINT set enters low power
// unless an enabled interrupt is already pending and CONTROL.CORE_CLK_EN keeps the core clock on
// (the immediate fall through, WAKE_INFO.FALL_THROUGH). In
// low power an enabled wake-up (WAKEUP_EN) records its reason in WAKE_INFO, clears the hint and
// raises the wakeup interrupt. From deep sleep (CONTROL.MAIN_PD_N 0 at the entry) the chip wakes
// through a reset recorded as a low-power exit and restarts in ROM (programmers_guide.md "Exiting
// from Deep Sleep"). From normal sleep (MAIN_PD_N 1: "All power domains are kept on for fast
// resume", theory_of_operation.md "Normal Sleep") there is no reset: software "continues executing
// the instruction after the wait-for-interrupt invocation" (programmers_guide.md "Exiting from
// Normal Sleep"; OpenTitan's pwrmgr_normal_sleep_all_wake_ups and pwrmgr_lowpower_cancel_test
// expect it). An enabled peripheral reset request (RESET_EN) resets the chip in any state. Aborts
// (a non-volatile operation in flight) are not modelled. A reset request in low power is recorded
// as a low-power exit too only after a deep-sleep entry: rstmgr records the reasons pwrmgr
// authorises, and "Low power entry reset has been triggered by pwrmgr" is one of them (rstmgr
// theory_of_operation.md "Reset Information"), a reset only deep sleep makes ("SW low power entry:
// wait-for-interrupt deep sleep entry", "Summary"); normal sleep resets nothing.
//
// Escalation (theory_of_operation.md "Escalation Reset Request"): the alert handler's severity 3
// (the top's esc_rst receiver) resets the chip at once, in any state, recorded in rstmgr as the
// escalation's hardware request; ESCALATE_RESET_STATUS reads 1 while the request is seen (until the
// reset, which resets the alert handler). The escalation clock timeout is not modelled.
// pwrmgr's wake-up sources, as the top numbers them (top_earlgrey.h
// kTopEarlgreyPowerManagerWakeUps*; the upstream merge of 2026-11-05 put I3C0/I3C1's reset detect at
// 0 and 1, which moved every other source up by two).
enum Eg2WakeSrc : unsigned {
  kEg2WakeI3c0 = 0, kEg2WakeI3c1 = 1, kEg2WakeSysrstCtrl = 2, kEg2WakeAdcCtrl = 3,
  kEg2WakePinmuxPin = 4, kEg2WakePinmuxUsb = 5, kEg2WakeAonTimer = 6, kEg2WakeSensorCtrl = 7,
  kEg2WakeCount = 8,
};

// eg2sim's binding of the power manager's core (eg2_pwrmgr_core.h): the registers are this
// model's, the resets and events are the SystemC system's. The behaviour is the core's.
class Eg2Pwrmgr : public Eg2IpModel, private Eg2PwrmgrPort {
 public:
  Eg2Pwrmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), core_(*this) {
    esc_status_ = reg("ESCALATE_RESET_STATUS");
    set_modelled(esc_status_);
    sys_.esc_receiver(3, [this](bool level) { core_.escalation(level); });
    control_ = reg("CONTROL");
    cdc_ = reg("CFG_CDC_SYNC");
    wake_en_ = reg("WAKEUP_EN");
    wake_status_ = reg("WAKE_STATUS");
    reset_en_ = reg("RESET_EN");
    reset_status_ = reg("RESET_STATUS");
    wake_info_ = reg("WAKE_INFO");
    capture_dis_ = reg("WAKE_INFO_CAPTURE_DIS");
    fault_status_ = reg("FAULT_STATUS");
    set_modelled(cdc_);
    set_modelled(control_);
    set_modelled(wake_status_);
    set_modelled(reset_status_);
    set_modelled(wake_info_);
    set_modelled(fault_status_);
    sys_.low_power_requested = [this] { return core_.enter_low_power(); };
    sys_.in_low_power = [this] { return core_.low_power(); };
    sys_.cpu_interrupt_wakeup = [this] { core_.interrupt_wakeup(); };
    // The AST's main power-ok dips for a moment (the chip bench's ast_supply_if
    // glitch_vcmain_pok): eg2sim --stim pwrmgr.main_pok_glitch=1@<point>.
    sys_.stim_handlers["pwrmgr.main_pok_glitch"] = [this](const std::string &) {
      core_.main_pok_glitch();
      return true;
    };
  }

  void on_reset(Eg2ResetCause c) override {
    uint32_t fault = value(fault_status_);
    Eg2IpModel::on_reset(c);
    // top_earlgrey.hjson connects pwrmgr rst_ni to the POR reset tree (eg2_pwrmgr_core.h).
    core_.after_reset(c == Eg2ResetCause::kPor, c == Eg2ResetCause::kLowPowerExit, fault);
  }

  // A wake-up request from source src (index in the top's wakeups list), a level.
  void wakeup(unsigned src) { core_.wakeup(src); }
  // Wake-up source src no longer requests.
  void wakeup_clear(unsigned src) { core_.wakeup_clear(src); }
  // A peripheral reset request from source src (index in the top's peripheral reset requests).
  void reset_request(unsigned src) { core_.reset_request(src); }

  bool low_power() const { return core_.low_power(); }
  // CONTROL.USB_CLK_EN_ACTIVE as last synchronised to the slow domain (CFG_CDC_SYNC): 0 turns
  // the USB clock off in the active state (pwrmgr.hjson; clkmgr then freezes accesses to usbdev).
  bool usb_clk_active() const { return core_.usb_clk_active(); }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == wake_en_) core_.reg_written(Eg2PwrReg::kWakeEn);
    if (r == cdc_) core_.reg_written(Eg2PwrReg::kCdcSync);
  }

 private:
  uint16_t id(Eg2PwrReg r) const {
    switch (r) {
      case Eg2PwrReg::kControl: return control_;
      case Eg2PwrReg::kCdcSync: return cdc_;
      case Eg2PwrReg::kWakeEn: return wake_en_;
      case Eg2PwrReg::kWakeStatus: return wake_status_;
      case Eg2PwrReg::kResetEn: return reset_en_;
      case Eg2PwrReg::kResetStatus: return reset_status_;
      case Eg2PwrReg::kWakeInfo: return wake_info_;
      case Eg2PwrReg::kCaptureDis: return capture_dis_;
      case Eg2PwrReg::kEscStatus: return esc_status_;
      case Eg2PwrReg::kFaultStatus: return fault_status_;
    }
    return control_;
  }
  // Eg2PwrmgrPort.
  uint32_t rd(Eg2PwrReg r) override { return value(id(r)); }
  void wr(Eg2PwrReg r, uint32_t v) override { set_value(id(r), v); }
  void set_wakeup_intr() override { set_intr("wakeup"); }
  void request_reset(Eg2PwrReset why, const std::string &what, int idx, bool lp) override {
    Eg2ResetCause c = why == Eg2PwrReset::kLowPowerExit ? Eg2ResetCause::kLowPowerExit
                    : why == Eg2PwrReset::kEscalation   ? Eg2ResetCause::kEscalation
                    : why == Eg2PwrReset::kSwRequest    ? Eg2ResetCause::kSoftware
                                                        : Eg2ResetCause::kHwRequest;
    sys_.request_reset(c, what, idx, lp);
  }
  void lowpower_event() override { sys_.chip_event("lowpower"); }
  bool reset_pending() override { return sys_.reset_pending(); }
  void note(const std::string &s) override { sys_.note(s); }

  Eg2PwrmgrCore core_;
  uint16_t control_, cdc_, wake_en_, wake_status_, reset_en_, reset_status_, wake_info_,
      capture_dis_, esc_status_, fault_status_;
};

// aon_timer (hw/ip/aon_timer: README.md, doc/theory_of_operation.md), on the always-on clock
// (200 kHz in sim_dv). The wake-up timer counts while WKUP_CTRL.enable, once every
// WKUP_CTRL.prescaler + 1 cycles; at the threshold (count >= WKUP_THOLD) it sets
// wkup_timer_expired, WKUP_CAUSE and requests a wake-up from pwrmgr. The watchdog counts every
// cycle while WDOG_CTRL.enable (not in low power with pause_in_sleep); at the bark threshold it sets
// wdog_timer_bark, which is also rv_core_ibex's watchdog NMI; at the bite threshold it requests a
// reset from pwrmgr. Software writes reach the counters at once (the CDC delay is not modelled).
// Both timers halt while lc_ctrl's lc_escalate_en is on (README.md).
class Eg2AonTimer : public Eg2IpModel {
 public:
  Eg2AonTimer(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    wctrl_ = reg("WKUP_CTRL");
    wth_hi_ = reg("WKUP_THOLD_HI");
    wth_lo_ = reg("WKUP_THOLD_LO");
    wc_hi_ = reg("WKUP_COUNT_HI");
    wc_lo_ = reg("WKUP_COUNT_LO");
    dctrl_ = reg("WDOG_CTRL");
    bark_ = reg("WDOG_BARK_THOLD");
    bite_ = reg("WDOG_BITE_THOLD");
    dc_ = reg("WDOG_COUNT");
    cause_ = reg("WKUP_CAUSE");
    for (uint16_t r : {wc_hi_, wc_lo_, dc_, cause_}) set_modelled(r);
    // "Both timers pause if the lc_escalate_en_i port (an lc_tx_t) is On" (README.md): lc_ctrl
    // asserts it on escalation severity 1 or 2 (lc_ctrl theory_of_operation.md "Security
    // Escalation"). That document says decoded outputs stay disabled until the next power cycle,
    // while this model clears escalation on every chip reset; this conflict is a specification gap.
    for (unsigned sev : {1u, 2u}) {
      sys_.esc_receiver(sev, [this](bool level) {
        if (!level || escalated_) return;
        settle(true);
        check_escalation_counts();
        escalated_ = true;
        rebase();
        evaluate();
      });
    }
    rebase();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    escalated_ = false;  // an alert escalation clears; SCRAP/INVALID still come from lc_ctrl
    rebase();
    evaluate();
  }
  void on_time(uint64_t) override { evaluate(); }
  void on_clock_change() override { evaluate(); }

 protected:
  void reg_before_read(uint16_t r) override {
    if (r == wc_hi_ || r == wc_lo_ || r == dc_ || int(r) == intr_state_ || r == cause_) evaluate();
  }
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    // Clearing WKUP_CAUSE withdraws the timer's wake-up request (aon_timer: the request follows
    // the cause).
    if (r == cause_ && !(value(cause_) & 1)) {
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup_clear(kEg2WakeAonTimer);
    }
    if (r == wctrl_ || r == dctrl_ || r == wc_hi_ || r == wc_lo_ || r == dc_) {
      // Counts written by software, or settings changed: count on from the current values.
      if (r != wc_hi_ && r != wc_lo_ && r != dc_) settle();
      rebase();
    }
    evaluate();
  }

 private:
  uint64_t wcount() const { return (uint64_t(value(wc_hi_)) << 32) | value(wc_lo_); }
  uint64_t wthold() const { return (uint64_t(value(wth_hi_)) << 32) | value(wth_lo_); }
  bool wdog_running() const {
    auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"));
    bool paused = (value(dctrl_) & 2) && pm && pm->low_power();
    return (value(dctrl_) & 1) && !paused && !lc_escalated();
  }
  bool lc_escalated() const {
    // lc_ctrl_function_signals_table.md sets ESCALATE_EN in SCRAP and INVALID as well as the
    // temporary ESCALATE state. lc_ctrl publishes that decoded documented signal to the system.
    return escalated_ || (sys_.lc_escalate_en && sys_.lc_escalate_en());
  }

  // The counters at the current time, from their values at the base time.
  void settle(bool at_escalation = false) {
    if (lc_escalated() && !at_escalation) return;  // halted: the counters keep their values
    uint64_t now = sys_.now_ps();
    uint64_t c = sys_.clock_cycles(Eg2System::Clk::kAon, now) - base_cycle_;
    if ((value(wctrl_) & 1) && (at_escalation || !lc_escalated())) {
      uint64_t pre = ((value(wctrl_) >> 1) & 0xfff) + 1;
      uint64_t w = base_wcount_ + c / pre;
      set_value(wc_lo_, uint32_t(w));
      set_value(wc_hi_, uint32_t(w >> 32));
    }
    auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"));
    bool paused = (value(dctrl_) & 2) && pm && pm->low_power();
    bool wdog = at_escalation ? ((value(dctrl_) & 1) && !paused) : wdog_running();
    if (wdog) set_value(dc_, uint32_t(base_dcount_ + c));
  }
  void check_escalation_counts() {
    // README.md says both timers pause when lc_escalate_en_i is On. Check the frozen value against
    // the independently elapsed AON-clock cycles before rebasing it, so receiver callback order
    // cannot silently leave the last software-access value behind.
    uint64_t now = sys_.clock_cycles(Eg2System::Clk::kAon, sys_.now_ps());
    uint64_t elapsed = now - base_cycle_;
    if (value(wctrl_) & 1) {
      uint64_t pre = ((value(wctrl_) >> 1) & 0xfff) + 1;
      uint64_t want = base_wcount_ + elapsed / pre;
      uint64_t got = value(wc_lo_) | (uint64_t(value(wc_hi_)) << 32);
      if (got != want) {
        sys_.finish(false, "aon_timer wake-up count was not settled at escalation");
        return;
      }
      std::printf("eg2sim: check aon_timer wake-up count at escalation = %llu: ok\n",
                  static_cast<unsigned long long>(got));
    }
    auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"));
    bool paused = (value(dctrl_) & 2) && pm && pm->low_power();
    if ((value(dctrl_) & 1) && !paused) {
      uint32_t want = uint32_t(base_dcount_ + elapsed), got = value(dc_);
      if (got != want) {
        sys_.finish(false, "aon_timer watchdog count was not settled at escalation");
        return;
      }
      std::printf("eg2sim: check aon_timer watchdog count at escalation = %u: ok\n", got);
    }
  }
  void rebase() {
    base_cycle_ = sys_.clock_cycles(Eg2System::Clk::kAon, sys_.now_ps());
    base_wcount_ = wcount();
    base_dcount_ = value(dc_);
  }

  void evaluate() {
    settle();
    if ((value(wctrl_) & 1) && wcount() >= wthold()) {
      set_intr("wkup_timer_expired");
      set_value(cause_, 1);
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup(kEg2WakeAonTimer);
    }
    if (value(dctrl_) & 1) {
      if (value(dc_) >= value(bark_)) {
        set_intr("wdog_timer_bark");
        if (auto *c = dynamic_cast<Eg2RvCoreIbexCfg *>(sys_.model("rv_core_ibex.cfg"))) c->nmi(1);
        // README.md "Watchdog bark": "When there is a "bark" event, two outputs become high:
        // wkup_req_o ... and intr_wdog_timer_bark_o"; the "level wakeup signal (wkup_req_o) stays
        // high until software explicitly acknowledges it by software writing 0 to WKUP_CAUSE"
        // (aon_timer.hjson AON_TIMER.WATCHDOG.BARK: "A bark triggers an interrupt and a wakeup").
        set_value(cause_, 1);
        if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup(kEg2WakeAonTimer);
      }
      if (value(dc_) >= value(bite_)) {
        if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->reset_request(1);
      }
    }
    // The next threshold either counter will reach.
    uint64_t next = Eg2System::kNever;
    uint64_t now_c = sys_.clock_cycles(Eg2System::Clk::kAon, sys_.now_ps());
    // Thresholds beyond 2^40 cycles away are never reached within a run (and would overflow).
    if (!lc_escalated() && (value(wctrl_) & 1) && wcount() < wthold() &&
        wthold() - base_wcount_ < (1ull << 40)) {
      uint64_t pre = ((value(wctrl_) >> 1) & 0xfff) + 1;
      next = std::min(next, base_cycle_ + (wthold() - base_wcount_) * pre);
    }
    if (wdog_running()) {
      for (uint32_t th : {value(bark_), value(bite_)}) {
        if (value(dc_) < th) next = std::min(next, base_cycle_ + (th - base_dcount_));
      }
    }
    if (next != Eg2System::kNever && next > now_c) {
      sys_.schedule(this, sys_.clock_time_of_cycle(Eg2System::Clk::kAon, next));
    } else {
      sys_.unschedule(this);
    }
  }

  uint16_t wctrl_, wth_hi_, wth_lo_, wc_hi_, wc_lo_, dctrl_, bark_, bite_, dc_, cause_;
  uint64_t base_cycle_ = 0, base_wcount_ = 0;
  bool escalated_ = false;  // lc_escalate_en on: both timers halted
  uint32_t base_dcount_ = 0;
};

// sensor_ctrl's STATUS: the AST reports its initialisation done and every IO rail's power good
// from power-on (the AST's power-up sequence takes no modelled time).
//
// Wake-up (theory_of_operation.md, "Wakeup Requests"): "incoming events can also be aggregated
// into a wakeup request to the system": the request (pwrmgr wake-up 5, wkup_req) stands while an
// AST event is active (from ALERT_TRIG or from the AST itself) or recorded (update_wakeup). EG2
// has sensor_ctrl in the always-on domain, so it also wakes the chip from deep sleep.
//
// What the AST drives, as the chip bench does (eg2sim --stim, usually ...@log:<message>):
//   sensor_ctrl.ast_event=<n>       AST event n fires once (chip_if trigger_sensor_ctrl_wkup:
//                                   alert 0 until its acknowledgement): recorded as ALERT_TRIG's
//                                   events are, and a wake-up request while it lasts
//   sensor_ctrl.io_pok_toggle=<n>   IO rail n's power good flips (chip_sw_sensor_ctrl_status_intr):
//                                   STATUS.io_pok changes and io_status_change is raised
//                                   ("io power status has changed", sensor_ctrl.hjson)
class Eg2SensorCtrl : public Eg2IpModel {
 public:
  Eg2SensorCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    alert_trig_ = reg("ALERT_TRIG");
    fatal_en_ = reg("FATAL_ALERT_EN");
    recov_alert_ = reg("RECOV_ALERT");
    fatal_alert_ = reg("FATAL_ALERT");
    for (unsigned i = 0; i < kAlertEvents; ++i) {
      alert_en_[i] = reg(("ALERT_EN_" + std::to_string(i)).c_str());
    }
    status_ = reg("STATUS");
    set_modelled(recov_alert_);
    set_modelled(fatal_alert_);
    set_modelled(status_);
    // STATUS: ast_init_done, then io_pok (sensor_ctrl.hjson field order; the table has no names).
    const eg2_regs::Reg &sr = desc().regs[status_];
    if (sr.nfields >= 2) {
      const eg2_regs::Field &f = desc().fields[sr.field0 + 1];
      io_pok_lsb_ = f.lsb;
      io_rails_ = f.width;
    }
    power_up();
    sys_.stim_handlers["sensor_ctrl.ast_event"] = [this](const std::string &v) {
      unsigned i = unsigned(std::stoul(v, nullptr, 0));
      if (i >= kAlertEvents) return false;
      ast_events_ |= 1u << i;
      record_events();
      update_wakeup();
      // The event lasts until sensor_ctrl acknowledges it, which a recoverable event gets at once
      // (theory_of_operation.md): the wake-up request stood while it lasted.
      ast_events_ &= ~(1u << i);
      update_wakeup();
      return true;
    };
    sys_.stim_handlers["sensor_ctrl.io_pok_toggle"] = [this](const std::string &v) {
      unsigned i = unsigned(std::stoul(v, nullptr, 0));
      if (i >= io_rails_) return false;
      set_value(status_, value(status_) ^ (1u << (io_pok_lsb_ + i)));
      set_intr("io_status_change");
      return true;
    };
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    ast_events_ = 0;
    power_up();
    update_wakeup();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    // ALERT_TRIG "1 Continuously trigger alert until disabled" (sensor_ctrl.hjson): an event stays
    // active while its trigger is set, so it is checked again after every write that can change
    // the outcome: the trigger, an enable, FATAL_ALERT_EN (moves it to FATAL_ALERT) and a clear
    // of RECOV_ALERT (rw1c: sets the bit again at once). FATAL_ALERT is read-only.
    bool relevant = r == alert_trig_ || r == fatal_en_ || r == recov_alert_;
    for (unsigned i = 0; i < kAlertEvents; ++i) relevant |= r == alert_en_[i];
    if (!relevant) return;
    record_events();
    update_wakeup();
  }

 private:
  // The AST events active now: ALERT_TRIG's and the AST's own.
  uint32_t active_events() const { return value(alert_trig_) | ast_events_; }
  // theory_of_operation.md: enabled AST events are recorded in RECOV_ALERT or FATAL_ALERT as
  // selected by FATAL_ALERT_EN, then sent to the alert handler.
  void record_events() {
    bool held = false;
    for (unsigned i = 0; i < kAlertEvents; ++i) {
      if (!((active_events() >> i) & 1) || (value(alert_en_[i]) & 0xf) != kMuBi4True) continue;
      held |= (value(alert_trig_) >> i) & 1;
      if ((value(fatal_en_) >> i) & 1) {
        set_value(fatal_alert_, value(fatal_alert_) | (1u << i));
        raise_alert("fatal_alert");
      } else {
        set_value(recov_alert_, value(recov_alert_) | (1u << i));
        raise_alert("recov_alert");
      }
    }
    // ALERT_TRIG "1 Continuously trigger alert until disabled" (sensor_ctrl.hjson): while a trigger
    // holds an enabled event, sensor_ctrl keeps sending its alert, so an alert handler that is
    // reset and configured again meanwhile (sensor_ctrl is always-on, the alert handler is not:
    // rstmgr "Summary", a low-power exit resets the non-aon domains) sees it again. Sent again
    // every 4 always-on cycles (the documents give no interval).
    uint64_t now = sys_.now_ps();
    if (held) {
      sys_.schedule(this, sys_.clock_time_of_cycle(
                              Eg2System::Clk::kAon,
                              sys_.clock_cycles(Eg2System::Clk::kAon, now) + 4));
    } else {
      sys_.unschedule(this);
    }
  }
  void on_time(uint64_t) override { record_events(); }
  // The wake-up request (pwrmgr wake-up 5) stands while an AST event is active or recorded in
  // RECOV_ALERT / FATAL_ALERT, until software clears it. The documents say only that events are
  // "aggregated into a wakeup request"; how long it stands is from OpenTitan's chip test
  // pwrmgr_sensor_ctrl_deep_sleep_wake_up (chip_testplan.hjson chip_sw_pwrmgr_sleep_sensor_ctrl_
  // alert_wakeup), which expects WAKE_STATUS set after the wake-up and clear only once the event is
  // cleared. EG2 spec GAP-RTLREQ-11.
  void update_wakeup() {
    uint32_t mask = (1u << kAlertEvents) - 1;
    bool want = ((active_events() | value(recov_alert_) | value(fatal_alert_)) & mask) != 0;
    if (want == wkup_req_) return;
    wkup_req_ = want;
    if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) {
      if (want) pm->wakeup(kWakeupSrc);
      else pm->wakeup_clear(kWakeupSrc);
    }
  }
  static constexpr unsigned kWakeupSrc = kEg2WakeSensorCtrl;  // top_earlgrey.gen.hjson wakeups: sensor_ctrl wkup_req
  uint32_t ast_events_ = 0;
  bool wkup_req_ = false;
  unsigned io_pok_lsb_ = 1, io_rails_ = 0;
  void power_up() {
    const eg2_regs::Reg &r = desc().regs[status_];
    uint32_t all = 0;
    for (uint16_t k = 0; k < r.nfields; ++k) {
      const eg2_regs::Field &f = desc().fields[r.field0 + k];
      all |= (f.width >= 32 ? 0xffffffffu : ((1u << f.width) - 1)) << f.lsb;
    }
    set_value(status_, all);
  }
  static constexpr unsigned kAlertEvents = 11;
  uint16_t alert_trig_, alert_en_[kAlertEvents], fatal_en_, recov_alert_, fatal_alert_, status_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_AON_H_
