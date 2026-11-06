// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// clkmgr of the EG2 SystemC model.
//
// Hints: the transactional clocks' hints (clkmgr.hjson CLK_HINTS, CLK_HINTS_STATUS). "When software
// provides a disable hint, the clock manager checks to see if the associated hardware block is
// idle. If the hardware block is idle, then the clock is disabled. If the hardware block is not
// idle, the clock is kept on. For the enable case, the software hint is immediately honored and the
// clock turned on." CLK_HINTS_STATUS "provides read feedback for the current clock state": bit i is
// 1 while the hint enables the clock or the block (AES, HMAC, KMAC, OTBN, in that order) is busy
// (Eg2IpModel::idle). A block whose clock is off freezes an access to it (clock_off below): the
// core stalls until a reset.
//
// The clock tree (the frequencies the model's clocks run at, Eg2System::clock_hz):
//  - internal clocks (AST doc/interfaces.md "Clock domains"): sys (main) up to 100 MHz, io 96 MHz,
//    usb 48 MHz, aon 200 kHz; io_div2 and io_div4 are io divided by 2 and 4 (clkmgr
//    theory_of_operation.md "Clock Frequency Summary": 96, 48, 24 MHz). AST README "Clock Outputs":
//    once configured, each is within +/-3% of its target; the model runs them at the target.
//  - software requested external clocks (theory_of_operation.md "Software Requested External
//    Clocks", "Clock Frequency Summary"): EXTCLK_CTRL.SEL True, and only while life-cycle hardware
//    debug is allowed, switches all clock sources to the external clock. AST doc/interfaces.md
//    all_clk_byp_req_i: "clk_ast_ext_i serves as the sys_clk, io_clk, usb_clk, and aon_clk clock
//    root. The external clock is conditionally divided by 2 to form a 48 MHz usb_clk. Further,
//    usb_clk is divided by 240 to form a 200 KHz aon_clk." The external clock is 96 MHz with
//    HI_SPEED_SEL True and 48 MHz otherwise (the summary table's assumption, and AST's
//    ext_freq_is_96m_i); at low speed "internal dividers are also stepped down": io (div 1) 48 MHz,
//    div 2 48 MHz, div 4 24 MHz. So: high speed main 96, io 96, io_div4 24, usb 48 MHz; low speed
//    main 48, io 48, io_div4 24, usb 48 MHz; aon 200 kHz in both. The switch completes at once
//    (EXTCLK_STATUS.ACK).
//
// Frequency measurements (theory_of_operation.md "Clock Frequency / Time-out Measurements"): while
// <CLK>_MEAS_CTRL_EN.EN is True, "clock manager then counts the number of relevant root clock cycles
// in each always-on clock period. If the resulting count differs from the programmed thresholds, a
// recoverable error is registered": "Clock too fast is registered when the clock cycle count is
// greater than the software programmed max threshold. Clock too slow is registered when the clock
// cycle count is less than the software programmed min threshold." The measured clocks are io,
// io_div4, main and usb (the hjson's measurement registers); an error sets RECOV_ERR_CODE's
// <CLK>_MEASURE_ERR and raises the recov_fault alert ("triggered when there are measurement
// errors"). Each full always-on period after the enable is one measurement; the model evaluates the
// periods that have passed whenever software touches the block.
//
// Reset (theory_of_operation.md "Reset Domains"): the dividers are on the por domain; "the other
// functions inside the clock manager operate on the life cycle reset domain", so "a user or
// escalation initiated reset still restores the clock manager to a default clean slate"
// (Eg2IpModel::keeps_state): the measurement controls, the error codes and the external clock
// selection reset, and the clocks return to their internal sources.
//
// Not modelled: the jittery main clock's frequency spread (JITTER_ENABLE): no document gives it
// (status/dv_tickets.md "Clock jitter: how far the jittery main clock deviates"); clock time-out
// errors (no clock the model runs ever stops while it is measured); the AST oscillators'
// calibration and their +/-3% deviation.

#ifndef EG2_MODEL_IP_EG2_CLKMGR_H_
#define EG2_MODEL_IP_EG2_CLKMGR_H_

#include <cstdint>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_aon.h"
#include "eg2_mem_otp.h"

namespace eg2_model {

class Eg2Clkmgr : public Eg2IpModel {
 public:
  Eg2Clkmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    hints_ = reg("CLK_HINTS");
    enables_ = reg("CLK_ENABLES");
    extclk_ctrl_ = reg("EXTCLK_CTRL");
    extclk_status_ = reg("EXTCLK_STATUS");
    recov_ = reg("RECOV_ERR_CODE");
    sys_.clock_off = [this](const std::string &b) { return clock_off(b); };
    status_ = reg("CLK_HINTS_STATUS");
    set_modelled(status_);
    set_modelled(extclk_status_);
    set_modelled(recov_);
    static const char *const kNames[kMeas] = {"IO", "IO_DIV4", "MAIN", "USB"};
    for (unsigned i = 0; i < kMeas; ++i) {
      meas_[i].en = reg((std::string(kNames[i]) + "_MEAS_CTRL_EN").c_str());
      meas_[i].ctrl = reg((std::string(kNames[i]) + "_MEAS_CTRL_SHADOWED").c_str());
    }
    // A chip clock's frequency in Hz from now on (eg2sim --stim clk.<name>=<hz>[@<point>]), the
    // input the chip bench gives the model through the DPI (dpi/eg2_sysmodel.cc): an AST clock off
    // its target, as AST README "Clock Outputs" allows (+/-3% once configured).
    static const struct { const char *key; Eg2System::Clk c; } kClkIn[] = {
        {"clk.main", Eg2System::Clk::kMain},
        {"clk.io_div4", Eg2System::Clk::kIoDiv4},
        {"clk.aon", Eg2System::Clk::kAon}};
    for (const auto &k : kClkIn) {
      Eg2System::Clk c = k.c;
      std::string key = k.key;
      sys_.stim_handlers[key] = [this, c, key](const std::string &v) {
        measure();
        uint64_t hz = std::stoull(v, nullptr, 0);
        if (sys_.set_clock_hz(c, hz, sys_.now_ps())) sys_.note(key + " = " + v + " Hz");
        if (c == Eg2System::Clk::kMain && !ext_) main_internal_hz_ = hz;
        return true;
      };
    }
  }

  // The frequency of a measured clock now (for other models and the log).
  uint64_t io_hz() const { return ext_ && ext_low_ ? 48000000ull : sys_.clock_hz(Eg2System::Clk::kIo); }

  void on_reset(Eg2ResetCause cause) override {
    if (!keeps_state(cause)) {
      if (ext_) internal_clocks();
      for (Meas &m : meas_) m.on = false;
    }
    Eg2IpModel::on_reset(cause);
  }

 protected:
  void reg_before_read(uint16_t r) override {
    if (r == recov_) {
      measure();
      return;
    }
    if (r != status_) return;
    static const char *const kBlocks[] = {"aes", "hmac", "kmac", "otbn"};
    uint32_t hints = value(hints_), st = 0;
    for (unsigned i = 0; i < 4; ++i) {
      bool busy = false;
      for (Eg2IpModel *m : sys_.models()) {
        if (std::string(m->module()) == kBlocks[i] && !m->idle()) busy = true;
      }
      if (((hints >> i) & 1) || busy) st |= 1u << i;
    }
    set_value(status_, st);
  }
  bool reg_before_write(uint16_t r, uint32_t, uint32_t) override {
    // The periods before a change are measured with the setting they ran under.
    if (r == extclk_ctrl_ || r == recov_ || is_meas_reg(r)) measure();
    return true;
  }
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    for (Meas &m : meas_) {
      if (r != m.en && r != m.ctrl) continue;
      bool on = (value(m.en) & 0xf) == kMuBi4True;
      if (on && (!m.on || r == m.ctrl)) {
        // Measuring starts with the next full always-on period.
        m.next = sys_.clock_cycles(Eg2System::Clk::kAon, sys_.now_ps()) + 1;
      }
      m.on = on;
      return;
    }
    if (r != extclk_ctrl_) return;
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    bool selected = (value(extclk_ctrl_) & 0xf) == kMuBi4True;
    bool hi = ((value(extclk_ctrl_) >> 4) & 0xf) == kMuBi4True;
    bool on = selected && lc && lc->hw_debug_en();
    set_value(extclk_status_, on ? kMuBi4True : kMuBi4False);
    if (on) {
      external_clocks(!hi);
    } else if (ext_) {
      internal_clocks();
    }
  }

 private:
  static constexpr unsigned kMeas = 4;  // io, io_div4, main, usb (RECOV_ERR_CODE bits 1-4)
  static constexpr uint32_t kMuBi4True = 0x6, kMuBi4False = 0x9;
  struct Meas {
    uint16_t en = 0, ctrl = 0;
    bool on = false;
    uint64_t next = 0;  // the next always-on period (its starting cycle) to measure
  };

  bool is_meas_reg(uint16_t r) const {
    for (const Meas &m : meas_) {
      if (r == m.en || r == m.ctrl) return true;
    }
    return false;
  }

  // Cycles of measured clock i completed by t_ps.
  uint64_t meas_cycles(unsigned i, uint64_t t_ps) const {
    switch (i) {
      case 0:  // io: io_div4's source, 4x; at low-speed external, 2x (dividers stepped down)
        return ext_ && ext_low_ ? 2 * sys_.clock_cycles(Eg2System::Clk::kIoDiv4, t_ps)
                                : sys_.clock_cycles(Eg2System::Clk::kIo, t_ps);
      case 1:
        return sys_.clock_cycles(Eg2System::Clk::kIoDiv4, t_ps);
      case 2:
        return sys_.clock_cycles(Eg2System::Clk::kMain, t_ps);
      default:  // usb: 48 MHz from AST, and from the external clock (divided by 2 at 96 MHz)
        return Eg2System::cycles(t_ps, 48000000ull);
    }
  }

  // Every full always-on period that has passed since the last evaluation, per enabled clock.
  void measure() {
    const uint64_t now = sys_.now_ps();
    const uint64_t done = sys_.clock_cycles(Eg2System::Clk::kAon, now);
    uint32_t errs = 0;
    for (unsigned i = 0; i < kMeas; ++i) {
      Meas &m = meas_[i];
      if (!m.on) continue;
      uint32_t ctrl = value(m.ctrl), hi = ctrl & 0x3ff, lo = (ctrl >> 10) & 0x3ff;
      // At most a second of always-on periods per evaluation: the clocks change only at events.
      if (done > m.next + 200000) m.next = done - 200000;
      for (uint64_t n = m.next; n < done; ++n) {
        uint64_t t0 = sys_.clock_time_of_cycle(Eg2System::Clk::kAon, n);
        uint64_t t1 = sys_.clock_time_of_cycle(Eg2System::Clk::kAon, n + 1);
        uint64_t count = meas_cycles(i, t1) - meas_cycles(i, t0);
        if (count > hi || count < lo) {
          if (!(errs & (2u << i))) {
            dbgf(1, "%s measurement %llu outside [%u, %u]", kMeasName[i],
                 (unsigned long long)count, lo, hi);
          }
          errs |= 2u << i;
        }
      }
      if (done > m.next) m.next = done;
    }
    if (errs) {
      set_value(recov_, value(recov_) | errs);
      raise_alert("recov_fault");
    }
  }

  void external_clocks(bool low) {
    if (!ext_) main_internal_hz_ = sys_.clock_hz(Eg2System::Clk::kMain);
    ext_ = true;
    ext_low_ = low;
    sys_.set_clock_hz(Eg2System::Clk::kMain, low ? 48000000ull : 96000000ull, sys_.now_ps());
    sys_.note(std::string("clkmgr: external clock, ") + (low ? "low speed (48 MHz)" : "high speed (96 MHz)"));
  }
  void internal_clocks() {
    ext_ = false;
    ext_low_ = false;
    sys_.set_clock_hz(Eg2System::Clk::kMain, main_internal_hz_, sys_.now_ps());
    sys_.note("clkmgr: internal clocks");
  }

  // Whether the block's clock is off: a transactional block (AES, HMAC, KMAC, OTBN) whose hint is
  // clear while it is idle; a peripheral whose CLK_ENABLES bit is clear. The peripherals: those
  // clkmgr_off_peri_test names (bit 0 clk_io_div4_peri uart0, 2 clk_io_peri spi_host0,
  // 3 clk_usb_peri usbdev, 4 clk_io_spi_host1 spi_host1); the rest of each clock group is not
  // gated in the model.
  bool clock_off(const std::string &b) {
    static const char *const kHinted[] = {"aes", "hmac", "kmac", "otbn"};
    for (unsigned i = 0; i < 4; ++i) {
      if (b != kHinted[i] || ((value(hints_) >> i) & 1)) continue;
      for (Eg2IpModel *m : sys_.models()) {
        if (std::string(m->module()) == kHinted[i] && !m->idle()) return false;
      }
      return true;
    }
    static const struct { const char *block; unsigned bit; } kPeri[] = {
        {"uart0", 0}, {"spi_host0", 2}, {"usbdev", 3}, {"spi_host1", 4}};
    for (const auto &p : kPeri) {
      if (b != p.block) continue;
      if (!((value(enables_) >> p.bit) & 1)) return true;
      // The USB clock itself is off in the active state when pwrmgr's CONTROL.USB_CLK_EN_ACTIVE
      // is 0 (pwrmgr.hjson: "USB clock disabled during active power state").
      if (b == "usbdev") {
        auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"));
        return pm && !pm->usb_clk_active();
      }
      return false;
    }
    return false;
  }

  static constexpr const char *kMeasName[kMeas] = {"io", "io_div4", "main", "usb"};
  uint16_t hints_, status_, enables_ = 0, extclk_ctrl_, extclk_status_, recov_;
  Meas meas_[kMeas];
  bool ext_ = false, ext_low_ = false;
  uint64_t main_internal_hz_ = 100000000ull;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CLKMGR_H_
