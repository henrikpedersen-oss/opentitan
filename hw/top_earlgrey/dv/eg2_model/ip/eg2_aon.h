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
// it; a power-on reset sets POR alone. A reset requested while the chip is in low power records
// LOW_POWER_EXIT with its own cause (theory_of_operation.md "Reset Information"). The per-IP
// software resets (SW_RST_CTRL_N) reset their block's model when written 0.
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
// unless an enabled interrupt is already pending (the fall through, WAKE_INFO.FALL_THROUGH). In
// low power an enabled wake-up (WAKEUP_EN) records its reason in WAKE_INFO, clears the hint, raises
// the wakeup interrupt and wakes the chip through a reset recorded as a low-power exit: deep sleep
// restarts in ROM (programmers_guide.md "Exiting from Deep Sleep"); the model resets for normal
// sleep too, as the RTL does (pwrmgr_fsm), the guide only describes software's side. An enabled
// peripheral reset request (RESET_EN) resets the chip in any state. Aborts (a non-volatile
// operation in flight) are not modelled.
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
      bool asleep = low_power_;
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
    set_modelled(cdc_);
    set_modelled(control_);
    set_modelled(wake_status_);
    set_modelled(reset_status_);
    set_modelled(wake_info_);
    sys_.low_power_requested = [this] { return enter_low_power(); };
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    low_power_ = false;
    set_value(esc_status_, 0);
  }

  // A wake-up request from source src (index in the top's wakeups list).
  void wakeup(unsigned src) {
    set_value(wake_status_, value(wake_status_) | (1u << src));
    if (!low_power_ || !((value(wake_en_) >> src) & 1)) return;
    if (!(value(capture_dis_) & 1)) set_value(wake_info_, value(wake_info_) | (1u << src));
    exit_low_power();
    sys_.request_reset(Eg2ResetCause::kLowPowerExit,
                       "low-power exit, wake-up " + std::to_string(src));
  }

  // A peripheral reset request from source src (index in the top's peripheral reset requests).
  void reset_request(unsigned src) {
    if (!((value(reset_en_) >> src) & 1)) return;
    set_value(reset_status_, value(reset_status_) | (1u << src));
    bool asleep = low_power_;
    exit_low_power();
    sys_.request_reset(Eg2ResetCause::kHwRequest,
                       "peripheral reset request " + std::to_string(src), int(src), asleep);
  }

  bool low_power() const { return low_power_; }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == cdc_) set_value(cdc_, 0);
  }

 private:
  bool enter_low_power() {
    if (!(value(control_) & 1)) return false;
    low_power_ = true;
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
  void exit_low_power() {
    if (!low_power_) return;
    low_power_ = false;
    set_value(control_, value(control_) & ~1u);
    set_intr("wakeup");
  }

  uint16_t control_, cdc_, wake_en_, wake_status_, reset_en_, reset_status_, wake_info_,
      capture_dis_, esc_status_;
  bool low_power_ = false;
};

// aon_timer (hw/ip/aon_timer: README.md, doc/theory_of_operation.md), on the always-on clock
// (200 kHz in sim_dv). The wake-up timer counts while WKUP_CTRL.enable, once every
// WKUP_CTRL.prescaler + 1 cycles; at the threshold (count >= WKUP_THOLD) it sets
// wkup_timer_expired, WKUP_CAUSE and requests a wake-up from pwrmgr. The watchdog counts every
// cycle while WDOG_CTRL.enable (not in low power with pause_in_sleep); at the bark threshold it sets
// wdog_timer_bark, which is also rv_core_ibex's watchdog NMI; at the bite threshold it requests a
// reset from pwrmgr. Software writes reach the counters at once (the CDC delay is not modelled).
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
    rebase();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
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
    return (value(dctrl_) & 1) && !paused;
  }

  // The counters at the current time, from their values at the base time.
  void settle() {
    uint64_t now = sys_.now_ps();
    uint64_t c = sys_.clock_cycles(Eg2System::Clk::kAon, now) - base_cycle_;
    if (value(wctrl_) & 1) {
      uint64_t pre = ((value(wctrl_) >> 1) & 0xfff) + 1;
      uint64_t w = base_wcount_ + c / pre;
      set_value(wc_lo_, uint32_t(w));
      set_value(wc_hi_, uint32_t(w >> 32));
    }
    if (wdog_running()) set_value(dc_, uint32_t(base_dcount_ + c));
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
    if ((value(wctrl_) & 1) && wcount() < wthold() && wthold() - base_wcount_ < (1ull << 40)) {
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
  uint32_t base_dcount_ = 0;
};

// sensor_ctrl's STATUS: the AST reports its initialisation done and every IO rail's power good
// from power-on (the AST's power-up sequence takes no modelled time).
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
    power_up();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    power_up();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r != alert_trig_) {
      bool enable_write = false;
      for (unsigned i = 0; i < kAlertEvents; ++i) enable_write |= r == alert_en_[i];
      if (!enable_write) return;
    }
    // theory_of_operation.md: enabled AST events are recorded in RECOV_ALERT or FATAL_ALERT as
    // selected by FATAL_ALERT_EN, then sent to the alert handler.
    for (unsigned i = 0; i < kAlertEvents; ++i) {
      if (!((value(alert_trig_) >> i) & 1) || (value(alert_en_[i]) & 0xf) != kMuBi4True) continue;
      if ((value(fatal_en_) >> i) & 1) {
        set_value(fatal_alert_, value(fatal_alert_) | (1u << i));
        raise_alert("fatal_alert");
      } else {
        set_value(recov_alert_, value(recov_alert_) | (1u << i));
        raise_alert("recov_alert");
      }
    }
  }

 private:
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
