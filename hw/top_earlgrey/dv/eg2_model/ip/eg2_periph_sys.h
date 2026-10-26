// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// sysrst_ctrl, adc_ctrl and pinmux's wake-up detectors of the EG2 SystemC model, the always-on
// peripherals that wake the chip (hw/ip/sysrst_ctrl, hw/ip/adc_ctrl, hw/top_earlgrey/ip_autogen/
// pinmux: data/*.hjson, doc/theory_of_operation.md). Their timers count the always-on clock, 200 kHz
// in sim_dv. Wake-ups go to pwrmgr by the top's index (sysrst_ctrl 0, adc_ctrl 1, pinmux 2, see
// ip/eg2_aon.h). Their inputs are static in a standalone run (--stim, ip/eg2_periph_stim.h): an
// event the chip bench makes by changing an input over time (an edge, a key press while the test
// waits) is not modelled; one a static input already satisfies (a level, a combination held, a
// value inside a filter) is, after its timer.

#ifndef EG2_MODEL_IP_EG2_PERIPH_SYS_H_
#define EG2_MODEL_IP_EG2_PERIPH_SYS_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_aon.h"
#include "eg2_periph_stim.h"

namespace eg2_model {

// The time n cycles of the always-on clock after now (Eg2System's aon clock; a debounce timer
// started before a frequency change keeps the time it was given).
inline uint64_t aon_after(const Eg2System &sys, uint64_t now, uint64_t n) {
  return sys.clock_time_of_cycle(Eg2System::Clk::kAon,
                                 sys.clock_cycles(Eg2System::Clk::kAon, now) + n);
}

inline void eg2_request_wakeup(Eg2System &sys, unsigned src) {
  if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys.model("pwrmgr"))) pm->wakeup(src);
}

// sysrst_ctrl. Inputs (PIN_IN_VALUE order: pwrb_in, key0_in, key1_in, key2_in, lid_open,
// ac_present, ec_rst_l, flash_wp_l) from --stim sysrst_ctrl.in=<hex>, default 0xcf (buttons
// released, lid closed, no AC, EC and write-protect not asserted); PIN_IN_VALUE reads them raw.
// KEY_INVERT_CTL inverts the key, power-button, AC and lid inputs before use and the key and
// power-button outputs after. Outputs (PIN_OUT_CTL order: bat_disable, ec_rst_l, pwrb_out,
// key0_out, key1_out, key2_out, z3_wakeup, flash_wp_l): pwrb_out and keyN_out pass their inputs
// through, ec_rst_l follows its input except while a reset pulse drives it low, flash_wp_l follows
// its input, bat_disable and z3_wakeup are 0 unless set by an action; "the override is enabled by
// PIN_OUT_CTL and allowed by PIN_ALLOWED_CTL" for the value PIN_OUT_VALUE gives (allowed_0 bit i
// for 0, allowed_1 bit i + 8 for 1). Each output change is an output item "out <PIN_OUT_CTL-order
// bits, hex>".
// Combinations (COM_SEL_CTL i non-zero): when every selected input is pressed (low after inversion)
// and the precondition holds (COM_PRE_SEL_CTL's inputs released, taken as held from reset), the
// combination is detected after COM_DET_CTL cycles; COM_OUT_CTL's actions follow: bat_disable,
// interrupt (COMBO_INTR_STATUS), ec_rst (ec_rst_l low for EC_RST_CTL cycles), rst_req (a reset
// request to pwrmgr, peripheral request 0). The ultra-low-power wake-up (ULP_CTL.ulp_enable): the
// power button pressed, the lid open or AC present for its debounce time sets ULP_STATUS and
// z3_wakeup and wakes the chip. event_detected (status) follows COMBO_INTR_STATUS | KEY_INTR_STATUS;
// any event also sets WKUP_STATUS and requests a wake-up. Key edge interrupts need a changing input
// and are not raised.
class Eg2SysrstCtrl : public Eg2IpModel {
 public:
  static constexpr unsigned kNumCombo = 4;

  Eg2SysrstCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    in_val_ = reg("PIN_IN_VALUE");
    invert_ = reg("KEY_INVERT_CTL");
    allowed_ = reg("PIN_ALLOWED_CTL");
    out_ctl_ = reg("PIN_OUT_CTL");
    out_val_ = reg("PIN_OUT_VALUE");
    ec_rst_ctl_ = reg("EC_RST_CTL");
    ulp_ctl_ = reg("ULP_CTL");
    ulp_status_ = reg("ULP_STATUS");
    ulp_pwrb_ = reg("ULP_PWRB_DEBOUNCE_CTL");
    wkup_status_ = reg("WKUP_STATUS");
    combo_status_ = reg("COMBO_INTR_STATUS");
    key_status_ = reg("KEY_INTR_STATUS");
    for (unsigned i = 0; i < kNumCombo; ++i) {
      std::string s = "_" + std::to_string(i);
      sel_[i] = reg(("COM_SEL_CTL" + s).c_str());
      pre_sel_[i] = reg(("COM_PRE_SEL_CTL" + s).c_str());
      det_[i] = reg(("COM_DET_CTL" + s).c_str());
      com_out_[i] = reg(("COM_OUT_CTL" + s).c_str());
    }
    set_modelled(in_val_);
    raw_ = Eg2PeriphStim::u32("sysrst_ctrl.in", 0xcf) & 0xff;
    update();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    for (auto &t : combo_at_) t = Eg2System::kNever;
    ulp_at_ = ec_low_until_ = Eg2System::kNever;
    bat_disable_ = z3_ = false;
    update();
  }
  void on_time(uint64_t now) override {
    for (unsigned i = 0; i < kNumCombo; ++i) {
      if (now >= combo_at_[i]) {
        combo_at_[i] = Eg2System::kNever;
        uint32_t act = value(com_out_[i]);
        if (act & 1) bat_disable_ = true;
        if (act & 2) set_value(combo_status_, value(combo_status_) | (1u << i));
        if (act & 4) ec_low_until_ = aon_after(sys_, now, uint64_t(value(ec_rst_ctl_) & 0xffff));
        if (act & 8) sys_.request_reset(Eg2ResetCause::kHwRequest, "sysrst_ctrl combination " +
                                        std::to_string(i), 0);
        event();
      }
    }
    if (now >= ulp_at_) {
      ulp_at_ = Eg2System::kNever;
      set_value(ulp_status_, 1);
      z3_ = true;
      event();
    }
    if (now >= ec_low_until_) ec_low_until_ = Eg2System::kNever;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    uint64_t now = sys_.now_ps();
    for (unsigned i = 0; i < kNumCombo; ++i) {
      if (r == sel_[i] || r == det_[i] || r == pre_sel_[i] || r == com_out_[i]) {
        combo_at_[i] = Eg2System::kNever;
        uint32_t sel = value(sel_[i]) & 0x1f;
        if (sel && (pressed() & sel) == sel) {
          combo_at_[i] = aon_after(sys_, now, uint64_t(value(det_[i])));
        }
      }
    }
    if (r == ulp_ctl_) {
      ulp_at_ = Eg2System::kNever;
      bool wake = !(eff() & 1) || ((eff() >> 4) & 1) || ((eff() >> 5) & 1);
      if ((value(ulp_ctl_) & 1) && wake) {
        ulp_at_ = aon_after(sys_, now, uint64_t(value(ulp_pwrb_) & 0xffff));
      }
    }
    update();
  }

 private:
  // The inputs after KEY_INVERT_CTL (pwrb, key0-2, lid_open, ac_present inverted by their _in bit).
  uint32_t eff() const {
    uint32_t inv = value(invert_), v = raw_;
    static const unsigned kInBit[6] = {6, 0, 2, 4, 10, 8};  // pwrb, key0..2, lid, ac -> KEY_INVERT bit
    for (unsigned i = 0; i < 6; ++i) v ^= ((inv >> kInBit[i]) & 1) << i;
    return v;
  }
  // COM_SEL order (key0, key1, key2, pwrb, ac_present): pressed = low, ac_present = high.
  uint32_t pressed() const {
    uint32_t e = eff();
    return ((~e >> 1) & 1) | (((~e >> 2) & 1) << 1) | (((~e >> 3) & 1) << 2) | ((~e & 1) << 3) |
           (((e >> 5) & 1) << 4);
  }
  void event() {
    set_value(wkup_status_, 1);
    eg2_request_wakeup(sys_, 0);
  }
  void update() {
    set_value(in_val_, raw_);
    uint32_t e = eff(), inv = value(invert_);
    uint32_t def = (uint32_t(bat_disable_) << 0) |
                   (uint32_t(((raw_ >> 6) & 1) && ec_low_until_ == Eg2System::kNever) << 1) |
                   (((e & 1) ^ ((inv >> 7) & 1)) << 2) | ((((e >> 1) & 1) ^ ((inv >> 1) & 1)) << 3) |
                   ((((e >> 2) & 1) ^ ((inv >> 3) & 1)) << 4) | ((((e >> 3) & 1) ^ ((inv >> 5) & 1)) << 5) |
                   (uint32_t(z3_) << 6) | (((raw_ >> 7) & 1) << 7);
    uint32_t ctl = value(out_ctl_), ov = value(out_val_), al = value(allowed_), out = def;
    for (unsigned i = 0; i < 8; ++i) {
      unsigned v = (ov >> i) & 1;
      bool ok = (al >> (i + (v ? 8 : 0))) & 1;
      if (((ctl >> i) & 1) && ok) out = (out & ~(1u << i)) | (v << i);
    }
    if (out != outputs_) {
      char item[16];
      std::snprintf(item, sizeof item, "out %02x", out);
      sys_.emit(desc().window, item);
      outputs_ = out;
    }
    set_intr_level("event_detected", (value(combo_status_) | value(key_status_)) != 0);
    uint64_t t = ec_low_until_;
    for (uint64_t c : combo_at_) t = std::min(t, c);
    t = std::min(t, ulp_at_);
    if (t == Eg2System::kNever) sys_.unschedule(this);
    else sys_.schedule(this, t);
  }

  uint16_t in_val_, invert_, allowed_, out_ctl_, out_val_, ec_rst_ctl_, ulp_ctl_, ulp_status_,
      ulp_pwrb_, wkup_status_, combo_status_, key_status_;
  uint16_t sel_[kNumCombo], pre_sel_[kNumCombo], det_[kNumCombo], com_out_[kNumCombo];
  uint32_t raw_ = 0xcf, outputs_ = 0xffffffffu;
  uint64_t combo_at_[kNumCombo] = {Eg2System::kNever, Eg2System::kNever, Eg2System::kNever,
                                   Eg2System::kNever};
  uint64_t ulp_at_ = Eg2System::kNever, ec_low_until_ = Eg2System::kNever;
  bool bat_disable_ = false, z3_ = false;
};

// adc_ctrl. Channel values (10 bits) from --stim adc_ctrl.chan0=<v> / chan1=<v>, default 0. While
// ADC_EN_CTL.adc_enable: in one-shot mode one sample of both channels after ADC_PD_CTL.pwrup_time
// cycles sets ADC_CHN_VAL (value and the intr copy) and ONESHOT in ADC_INTR_STATUS; in normal mode
// the filters are evaluated after pwrup_time + np_sample_cnt cycles: filter i matches when both
// channels' filter i are enabled and each value is inside [min_v, max_v] (cond 0) or outside (cond
// 1), setting FILTER_STATUS.MATCH[i], ADC_INTR_STATUS.MATCH[i] if ADC_INTR_CTL enables it, and a
// wake-up if ADC_WAKEUP_CTL enables it. Low-power mode's slower sampling is taken as the same
// timing. match_pending (status) follows ADC_INTR_STATUS. Transitions (TRANS) need a changing value
// and are not raised.
class Eg2AdcCtrl : public Eg2IpModel {
 public:
  static constexpr unsigned kNumFilter = 8;

  Eg2AdcCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    en_ = reg("adc_en_ctl");
    pd_ = reg("adc_pd_ctl");
    sample_ = reg("adc_sample_ctl");
    for (unsigned i = 0; i < kNumFilter; ++i) {
      f0_[i] = reg(("adc_chn0_filter_ctl_" + std::to_string(i)).c_str());
      f1_[i] = reg(("adc_chn1_filter_ctl_" + std::to_string(i)).c_str());
    }
    val_[0] = reg("adc_chn_val_0");
    val_[1] = reg("adc_chn_val_1");
    wake_ = reg("adc_wakeup_ctl");
    fstat_ = reg("filter_status");
    ictl_ = reg("adc_intr_ctl");
    istat_ = reg("adc_intr_status");
    chan_[0] = Eg2PeriphStim::u32("adc_ctrl.chan0", 0) & 0x3ff;
    chan_[1] = Eg2PeriphStim::u32("adc_ctrl.chan1", 0) & 0x3ff;
    set_modelled(val_[0]);
    set_modelled(val_[1]);
  }
  void on_time(uint64_t) override {
    if (!(value(en_) & 1)) return;
    for (unsigned c = 0; c < 2; ++c) {
      uint32_t v = chan_[c];
      set_value(val_[c], (v << 2) | (v << 18));
    }
    if (value(en_) & 2) {  // one-shot
      if ((value(ictl_) >> 9) & 1) set_value(istat_, value(istat_) | (1u << 9));
    } else {
      for (unsigned i = 0; i < kNumFilter; ++i) {
        if (matches(value(f0_[i]), chan_[0]) && matches(value(f1_[i]), chan_[1])) {
          set_value(fstat_, value(fstat_) | (1u << i));
          if ((value(ictl_) >> i) & 1) set_value(istat_, value(istat_) | (1u << i));
          if ((value(wake_) >> i) & 1) eg2_request_wakeup(sys_, 1);
        }
      }
    }
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == en_ || r == pd_ || r == sample_) {
      sys_.unschedule(this);
      if (value(en_) & 1) {
        uint64_t cyc = ((value(pd_) >> 4) & 0xf) + ((value(en_) & 2) ? 0 : (value(sample_) & 0xffff));
        sys_.schedule(this, aon_after(sys_, sys_.now_ps(), (cyc + 1)));
      }
    }
    update();
  }

 private:
  static bool matches(uint32_t f, uint32_t v) {
    if (!((f >> 31) & 1)) return false;
    uint32_t lo = (f >> 2) & 0x3ff, hi = (f >> 18) & 0x3ff;
    bool inside = v >= lo && v <= hi;
    return ((f >> 12) & 1) ? !inside : inside;
  }
  void update() { set_intr_level("match_pending", value(istat_) != 0); }

  uint16_t en_, pd_, sample_, f0_[kNumFilter], f1_[kNumFilter], val_[2], wake_, fstat_, ictl_,
      istat_;
  uint32_t chan_[2] = {0, 0};
};

// pinmux: the wake-up detectors (WKUP_DETECTOR*, WKUP_CAUSE); the pad and select registers stay
// registers (the model does not route pads). Detector i, enabled, watches the pad WKUP_DETECTOR_
// PADSEL selects, whose level is static: bit SEL of --stim pinmux.mio=<hex> (MIODIO 0) or
// pinmux.dio=<hex> (MIODIO 1), default 0. Modes (MODE): 0 rising edge, 1 falling edge, 2 any edge
// (need a changing level, not raised), 3 timed high, 4 timed low: the level held for
// WKUP_DETECTOR_CNT_TH + 1 cycles sets bit i of WKUP_CAUSE and requests wake-up 2.
class Eg2Pinmux : public Eg2IpModel {
 public:
  static constexpr unsigned kNumDet = 8;

  Eg2Pinmux(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    for (unsigned i = 0; i < kNumDet; ++i) {
      std::string s = "_" + std::to_string(i);
      en_[i] = reg(("WKUP_DETECTOR_EN" + s).c_str());
      det_[i] = reg(("WKUP_DETECTOR" + s).c_str());
      th_[i] = reg(("WKUP_DETECTOR_CNT_TH" + s).c_str());
      sel_[i] = reg(("WKUP_DETECTOR_PADSEL" + s).c_str());
      at_[i] = Eg2System::kNever;
    }
    cause_ = reg("WKUP_CAUSE");  // one bit per detector
    mio_ = Eg2PeriphStim::u32("pinmux.mio", 0);
    dio_ = Eg2PeriphStim::u32("pinmux.dio", 0);
  }
  void on_time(uint64_t now) override {
    for (unsigned i = 0; i < kNumDet; ++i) {
      if (now >= at_[i]) {
        at_[i] = Eg2System::kNever;
        set_value(cause_, value(cause_) | (1u << i));
        eg2_request_wakeup(sys_, 2);
      }
    }
    plan();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    for (unsigned i = 0; i < kNumDet; ++i) {
      if (r != en_[i] && r != det_[i] && r != th_[i] && r != sel_[i]) continue;
      at_[i] = Eg2System::kNever;
      uint32_t mode = value(det_[i]) & 7, sel = value(sel_[i]) & 0x3f;
      bool level = (((value(det_[i]) >> 4) & 1) ? dio_ : mio_) >> sel & 1;
      if ((value(en_[i]) & 1) && ((mode == 3 && level) || (mode == 4 && !level))) {
        at_[i] = aon_after(sys_, sys_.now_ps(), (uint64_t(value(th_[i])) + 1));
      }
    }
    plan();
  }

 private:
  void plan() {
    uint64_t t = Eg2System::kNever;
    for (uint64_t a : at_) t = std::min(t, a);
    if (t == Eg2System::kNever) sys_.unschedule(this);
    else sys_.schedule(this, t);
  }
  uint16_t en_[kNumDet], det_[kNumDet], th_[kNumDet], sel_[kNumDet], cause_;
  uint64_t at_[kNumDet];
  uint32_t mio_ = 0, dio_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_SYS_H_
