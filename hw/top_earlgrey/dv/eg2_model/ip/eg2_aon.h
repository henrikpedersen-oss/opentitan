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

namespace eg2_model {


// rstmgr: RESET_REQ written with MuBi4True is a software reset. RESET_INFO records every reset's
// cause (POR, LOW_POWER_EXIT, SW_RESET, HW_REQ) and keeps it through resets until software clears
// it; a power-on reset sets POR alone. A reset requested after a deep-sleep entry records
// LOW_POWER_EXIT with its own cause (theory_of_operation.md "Reset Information"; Eg2Pwrmgr). The
// per-IP software resets (SW_RST_CTRL_N) reset their block's model when written 0.
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
class Eg2Rstmgr : public Eg2IpModel {
 public:
  Eg2Rstmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
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
    }
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

  void before_reset(Eg2ResetCause c) override {
    if (c != Eg2ResetCause::kPor && (value(cpu_ctrl_) & 1) && sys_.cpu_crashdump) {
      cpu_dump_ = sys_.cpu_crashdump();
      sys_.note("rstmgr: CPU crash dump captured");
    }
    if (c == Eg2ResetCause::kPor || !(value(alert_ctrl_) & 1) || !sys_.alert_crashdump) return;
    alert_dump_ = sys_.alert_crashdump();
    char buf[64];
    std::snprintf(buf, sizeof buf, "rstmgr: alert crash dump captured (%zu words)",
                  alert_dump_.size());
    sys_.note(buf);
  }

  void on_reset(Eg2ResetCause c) override {
    if (c == Eg2ResetCause::kPor) {
      Eg2IpModel::on_reset(c);
      alert_dump_.clear();
      cpu_dump_.clear();
      set_value(info_, 1);
      return;
    }
    set_value(alert_ctrl_, value(alert_ctrl_) & ~1u);
    set_value(cpu_ctrl_, value(cpu_ctrl_) & ~1u);
    uint32_t bit = 0;
    switch (c) {
      case Eg2ResetCause::kLowPowerExit: bit = 1u << 1; break;
      case Eg2ResetCause::kSoftware: bit = 1u << 2; break;
      case Eg2ResetCause::kHwRequest:
        bit = sys_.reset_hw_req() >= 0 ? 1u << (3 + sys_.reset_hw_req()) : 0;
        break;
      case Eg2ResetCause::kEscalation: bit = 1u << (3 + 3); break;
      case Eg2ResetCause::kNdm: bit = 1u << (3 + 4); break;
      default: break;
    }
    if (c != Eg2ResetCause::kLowPowerExit && sys_.reset_in_low_power()) bit |= 1u << 1;
    set_value(info_, value(info_) | bit);
    set_value(req_, kMuBi4False);
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == req_ && (value(req_) & 0xf) == kMuBi4True) {
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
      uint32_t idx = (value(alert_ctrl_) >> 4) & 0xf;
      set_value(alert_info_, idx < alert_dump_.size() ? alert_dump_[idx] : 0);
    } else if (r == cpu_attr_) {
      set_value(cpu_attr_, sys_.cpu_crashdump ? 8 : 0);
    } else if (r == cpu_info_) {
      uint32_t idx = (value(cpu_ctrl_) >> 4) & 0xf;
      set_value(cpu_info_, idx < cpu_dump_.size() ? cpu_dump_[idx] : 0);
    }
  }

 private:
  uint16_t req_, info_, alert_ctrl_, alert_attr_, alert_info_, cpu_ctrl_, cpu_attr_, cpu_info_;
  std::vector<uint32_t> cpu_dump_;  // the last capture
  // SW_RST_CTRL_N_<i>: the blocks in the order of rstmgr.hjson's SW_RST countermeasures (usbdev_aon
  // has no model of its own).
  static constexpr unsigned kSwResets = 10;
  static constexpr const char *kSwResetBlocks[kSwResets] = {
      "spi_device", "spi_host0", "spi_host1", "usbdev", "", "i2c0", "i2c1", "i2c2", "i3c0", "i3c1"};
  uint16_t sw_rst_[kSwResets];
  std::vector<uint32_t> alert_dump_;  // the last capture
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
class Eg2Pwrmgr : public Eg2IpModel {
 public:
  Eg2Pwrmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    esc_status_ = reg("ESCALATE_RESET_STATUS");
    set_modelled(esc_status_);
    sys_.esc_receiver(3, [this](bool level) {
      if (!level) return;
      set_value(esc_status_, 1);
      bool asleep = low_power_reset();
      exit_low_power();
      sys_.note("pwrmgr: escalation reset request (esc_rst)");
      sys_.request_reset(Eg2ResetCause::kEscalation, "alert escalation reset (pwrmgr esc_rst)", -1,
                         asleep);
    });
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
    sys_.low_power_requested = [this] { return enter_low_power(); };
    sys_.in_low_power = [this] { return low_power_; };
    sys_.cpu_interrupt_wakeup = [this] { interrupt_wakeup(); };
    // The AST's main power-ok dips for a moment (the chip bench's ast_supply_if
    // glitch_vcmain_pok): eg2sim --stim pwrmgr.main_pok_glitch=1@<point>.
    sys_.stim_handlers["pwrmgr.main_pok_glitch"] = [this](const std::string &) {
      main_pok_glitch();
      return true;
    };
  }

  void on_reset(Eg2ResetCause c) override {
    uint32_t fault = value(fault_status_);
    Eg2IpModel::on_reset(c);
    // top_earlgrey.hjson connects pwrmgr rst_ni to the POR reset tree; FAULT_STATUS therefore
    // survives every internally requested reset and clears only when POR reaches pwrmgr.
    if (c != Eg2ResetCause::kPor) set_value(fault_status_, fault);
    low_power_ = false;
    set_value(esc_status_, 0);
    // A reset other than a low-power exit resets the wake-up sources too (Eg2IpModel::keeps_state):
    // their requests drop with them.
    if (c != Eg2ResetCause::kLowPowerExit) {
      wake_req_ = 0;
      show_wake_status();
    }
    // The synchronised copy follows CONTROL as the reset leaves it (resval 1; an always-on
    // register keeps its value over a reset that does not reach the always-on domain).
    usb_clk_active_ = (value(control_) >> 7) & 1;
  }

  // A wake-up request from source src (index in the top's wakeups list).
  // A request is a level: it stays until its source withdraws it (wakeup_clear), and wakes the
  // chip from low power whenever its WAKEUP_EN bit is set, also if it came before the enable.
  // WAKE_STATUS: "all current wake requests post enable mask" (pwrmgr.hjson).
  void wakeup(unsigned src) {
    wake_req_ |= 1u << src;
    show_wake_status();
    if (!((value(wake_en_) >> src) & 1)) return;
    if (!low_power_) return;
    if (!(value(capture_dis_) & 1)) set_value(wake_info_, value(wake_info_) | (1u << src));
    bool deep = deep_sleep_;
    exit_low_power();
    if (!deep) {
      sys_.note("pwrmgr: wake-up " + std::to_string(src) + " from normal sleep, no reset");
      return;
    }
    sys_.request_reset(Eg2ResetCause::kLowPowerExit,
                       "low-power exit, wake-up " + std::to_string(src));
  }

  // Wake-up source src no longer requests (WAKE_STATUS shows the requests as they are now).
  void wakeup_clear(unsigned src) {
    wake_req_ &= ~(1u << src);
    show_wake_status();
  }

  // A peripheral reset request from source src (index in the top's peripheral reset requests).
  void reset_request(unsigned src) {
    if (!((value(reset_en_) >> src) & 1)) return;
    set_value(reset_status_, value(reset_status_) | (1u << src));
    bool asleep = low_power_reset();
    exit_low_power();
    sys_.request_reset(Eg2ResetCause::kHwRequest,
                       "peripheral reset request " + std::to_string(src), int(src), asleep);
  }

  bool low_power() const { return low_power_; }
  // CONTROL.USB_CLK_EN_ACTIVE as last synchronised to the slow domain (CFG_CDC_SYNC): 0 turns
  // the USB clock off in the active state (pwrmgr.hjson; clkmgr then freezes accesses to usbdev).
  bool usb_clk_active() const { return usb_clk_active_; }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == wake_en_) show_wake_status();
    if (r == cdc_) {
      usb_clk_active_ = (value(control_) >> 7) & 1;
      set_value(cdc_, 0);
    }
  }

 private:
  // Whether pwrmgr has made a low-power entry reset (a deep-sleep entry): rstmgr then records a
  // reset requested now as a low-power exit too (the class comment).
  bool low_power_reset() const { return low_power_ && deep_sleep_; }
  // pwrmgr theory_of_operation.md "Fall Through Handling": right after WFI, an interrupt can make
  // entry invalid and return the system to active. CONTROL.CORE_CLK_EN says whether the core clock
  // remains on in low power; pwrmgr.hjson WAKE_INFO.FALL_THROUGH records the cancelled entry.
  void interrupt_wakeup() {
    if (!low_power_ || deep_sleep_ || !(value(control_) & (1u << 4))) return;
    if (!(value(capture_dis_) & 1)) set_value(wake_info_, value(wake_info_) | (1u << 6));
    sys_.note("pwrmgr: processor interrupt cancelled normal-sleep entry (fall through)");
    exit_low_power(false);
  }
  // theory_of_operation.md "Main Power Unstable Reset Requests": "If the main power ever becomes
  // unstable (the power okay indication is low even though it is powered on), the power manager
  // requests an internal reset. This reset behaves similarly to the escalation reset"; "the main
  // power unstable checks are valid only during states that power should be on", so not once a
  // deep-sleep entry has turned the main domain off (during the entry it still is on). rstmgr
  // records it as HW_REQ's main power glitch (rstmgr.hjson RESET_INFO b5: request index 2).
  // FAULT_STATUS.MAIN_PD_GLITCH says "When 1, unexpected power glitch was observed on main PD"
  // (pwrmgr.hjson); pwrmgr is on the POR reset tree, so the sticky bit is held until POR.
  void main_pok_glitch() {
    if (low_power_ && deep_sleep_ && !entering_) {
      sys_.note("pwrmgr: main power-ok low while the main domain is powered down: no glitch reset");
      return;
    }
    set_value(fault_status_, value(fault_status_) | (1u << 2));
    // Assumption: a glitch at the lowpower event counts as a triggered low-power-entry reset, so
    // rstmgr also records LOW_POWER_EXIT. The documents do not define that point in the entry;
    // chip_sw_deep_power_glitch_vseq applies the glitch there and the software expects both bits.
    bool lp_reset = low_power_reset();
    exit_low_power();
    sys_.note("pwrmgr: main power glitch: internal reset request");
    sys_.request_reset(Eg2ResetCause::kHwRequest, "main power glitch (pwrmgr internal reset)", 2,
                       lp_reset);
  }
  bool enter_low_power() {
    if (!(value(control_) & 1)) return false;
    low_power_ = true;
    deep_sleep_ = !((value(control_) >> 8) & 1);  // CONTROL.MAIN_PD_N 0: main powered down
    // The entry as the chip bench sees it (pwrmgr_low_power_if): what is applied now comes while
    // the entry is under way, before the main domain is off.
    entering_ = true;
    sys_.chip_event("lowpower");
    entering_ = false;
    if (sys_.reset_pending()) return true;
    // A wake-up already requested and enabled wakes the chip at once.
    uint32_t w = value(wake_status_) & value(wake_en_);
    for (unsigned s = 0; s < 6; ++s) {
      if ((w >> s) & 1) {
        wakeup(s);
        break;
      }
    }
    return true;
  }
  void exit_low_power(bool raise_wakeup_interrupt = true) {
    if (!low_power_) return;
    low_power_ = false;
    set_value(control_, value(control_) & ~1u);
    if (raise_wakeup_interrupt) set_intr("wakeup");
  }

  bool usb_clk_active_ = true;
  uint16_t control_, cdc_, wake_en_, wake_status_, reset_en_, reset_status_, wake_info_,
      capture_dis_, esc_status_, fault_status_;
  void show_wake_status() { set_value(wake_status_, wake_req_ & value(wake_en_)); }

  bool low_power_ = false;
  bool deep_sleep_ = false;
  bool entering_ = false;  // during the low-power entry's event (chip_event "lowpower")
  uint32_t wake_req_ = 0;  // the sources' wake-up requests, before the enable mask
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
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup_clear(4);
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
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup(4);
    }
    if (value(dctrl_) & 1) {
      if (value(dc_) >= value(bark_)) {
        set_intr("wdog_timer_bark");
        if (auto *c = dynamic_cast<Eg2RvCoreIbexCfg *>(sys_.model("rv_core_ibex.cfg"))) c->nmi(1);
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
    for (unsigned i = 0; i < kAlertEvents; ++i) {
      if (!((active_events() >> i) & 1) || (value(alert_en_[i]) & 0xf) != kMuBi4True) continue;
      if ((value(fatal_en_) >> i) & 1) {
        set_value(fatal_alert_, value(fatal_alert_) | (1u << i));
        raise_alert("fatal_alert");
      } else {
        set_value(recov_alert_, value(recov_alert_) | (1u << i));
        raise_alert("recov_alert");
      }
    }
  }
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
  static constexpr unsigned kWakeupSrc = 5;  // top_earlgrey.gen.hjson wakeups: sensor_ctrl wkup_req
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
