// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_ADC_CTRL_H_
#define EG2_MODEL_IP_EG2_ADC_CTRL_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include "../eg2_ip_model.h"
#include "eg2_aon.h"
#include "eg2_periph_stim.h"
#include "eg2_periph_wakeup.h"

namespace eg2_model {

// adc_ctrl (hw/ip/adc_ctrl: doc/theory_of_operation.md, data/adc_ctrl.hjson). Channel values (10
// bits) from --stim adc_ctrl.chan0=<v> / chan1=<v>, default 0, and while the model runs from
// adc_ctrl.chan0=<v>@<point> (eg2sim --stim), as the chip bench forces the AST's ADC output, or as
// a sequence of sample pairs, one per conversion (adc_ctrl.pairs=<c0>/<c1>;...@<point>). While
// ADC_EN_CTL.adc_enable:
//   One-shot mode: one sample of both channels after ADC_PD_CTL.pwrup_time cycles sets ADC_CHN_VAL
//   and ONESHOT in ADC_INTR_STATUS (if ADC_INTR_CTL enables it).
//   Scanning ("Filters and debounce"): each pair of samples is evaluated against the enabled filter
//   pairs (filter i hits when both channels' filter i are enabled and each value is inside
//   [min_v, max_v], cond 0, or outside, cond 1). A counter counts evaluations hitting the same set
//   of filters (a different set or none resets it). With ADC_PD_CTL.lp_mode the controller scans
//   periodically: the ADC powers up (pwrup_time cycles) every wakeup_time cycles for one pair, and
//   when lp_sample_cnt evaluations have hit (the counter "reaches its threshold", "Filters and
//   debounce"; "Scan type check" says "more than") it moves to continuous scanning:
//   FILTER_STATUS.TRANS ("transition occurred"), ADC_INTR_STATUS.TRANS if ADC_INTR_CTL enables it.
//   Continuous scanning (lp_mode 0, or after that transition) takes a pair every cycle once powered
//   up; when np_sample_cnt evaluations have hit, FILTER_STATUS and ADC_INTR_STATUS (as ADC_INTR_CTL
//   enables) get the hit filters and ADC_CHN_VAL's intr copies the values; when a pair hits nothing
//   after the transition, the controller goes back to periodic scanning ("If a match cannot be
//   confirmed in normal operation mode, the ADC controller switches back to low power mode").
// A pair takes "two samples (450us) plus internal processing time (4 clock cycles)" from the ADC
// controller (theory_of_operation.md "ADC Controller"). A result that cannot
// change (the same inputs and settings, the counter at its threshold or no filter hit) is not
// sampled again until an input or a setting changes.
// The wake-up request (pwrmgr wake-up 1) stands while a FILTER_STATUS bit is set that
// ADC_WAKEUP_CTL enables (match bits, and TRANS with its trans_en): "If a filter is enabled for
// wakeup in adc_wakeup_ctl and filter_status indicates a match, a wakeup is generated".
// match_pending (status) follows ADC_INTR_STATUS.
// Items in adc_ctrl.model.log, the chip bench's format: "lines %08h (intr wkup = bits 0..1)" when the
// interrupt line (match_pending enabled) or the wake-up request changes, and "< <register> %08h"
// for the reads the bench checks.
// Debug lines: enable and mode, power-up and sample counts, each filter as configured, each
// evaluation with its values, the low-power to continuous transition, filter matches, the wake-up
// request, interrupts raised.
class Eg2AdcCtrl : public Eg2IpModel {
 public:
  static constexpr unsigned kNumFilter = 8;

  Eg2AdcCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), log_(sys_, inst(), "lines", "(intr wkup = bits 0..1)") {
    en_ = reg("adc_en_ctl");
    pd_ = reg("adc_pd_ctl");
    lp_sample_ = reg("adc_lp_sample_ctl");
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
    log_.reads({{istat_, "ADC_INTR_STATUS", 0x3ff},
                {fstat_, "FILTER_STATUS", 0x1ff},
                {val_[0], "ADC_CHN_VAL_0", 0x0ffc0ffc},
                {val_[1], "ADC_CHN_VAL_1", 0x0ffc0ffc},
                {uint16_t(intr_state_), "INTR_STATE", 0x1}});
    debug_irqs();
    for (unsigned c = 0; c < 2; ++c) {
      sys_.stim_handlers["adc_ctrl.chan" + std::to_string(c)] = [this, c](const std::string &v) {
        uint32_t x = uint32_t(std::stoul(v, nullptr, 0)) & 0x3ff;
        if (x != chan_[c]) {
          chan_[c] = x;
          dbgf(1, "channel %u input %u", c, x);
          if (value(en_) & 1) resume();
        }
        return true;
      };
    }
    // A sequence of sample pairs the AST returns in turn, as the bench forces the ADC's output at
    // each conversion (adc_ctrl.pairs=<c0>/<c1>;<c0>/<c1>;...@<point>): each pair the controller
    // takes uses the next. Assumption: after the last pair the channels keep their values; the IP
    // documents do not define a bench-forced sequence, and the sequence calls 1023 "unforced".
    sys_.stim_handlers["adc_ctrl.pairs"] = [this](const std::string &v) {
      pairs_.clear();
      for (size_t s = 0; s < v.size();) {
        size_t e = v.find(';', s);
        unsigned a = 0, b = 0;
        if (std::sscanf(v.substr(s, e == std::string::npos ? e : e - s).c_str(), "%u/%u", &a, &b) !=
            2) {
          return false;
        }
        pairs_.push_back({a & 0x3ff, b & 0x3ff});
        s = e == std::string::npos ? v.size() : e + 1;
      }
      dbgf(1, "%zu sample pairs queued", pairs_.size());
      if (value(en_) & 1) resume();
      return true;
    };
    update();
  }
  void on_reset(Eg2ResetCause c) override {
    bool kept = keeps_state(c);
    Eg2IpModel::on_reset(c);
    if (!kept) {
      phase_ = kOff;
      count_ = 0;
      last_hits_ = 0;
      update();
    }
  }
  void on_time(uint64_t now) override {
    if (!(value(en_) & 1) || phase_ == kOff) return;
    if (phase_ == kSleep) {  // the periodic wake: power up for the next pair
      phase_ = kLp;
      sys_.schedule(this, sample_after(aon_after(sys_, now, pwrup())));
      return;
    }
    if (!pairs_.empty()) {  // the next pair of a sequence the bench forces
      chan_[0] = pairs_.front().first;
      chan_[1] = pairs_.front().second;
      pairs_.pop_front();
    }
    for (unsigned c = 0; c < 2; ++c) {
      uint32_t v = chan_[c];
      set_value(val_[c], (value(val_[c]) & ~0xffcu) | (v << 2));
    }
    if (value(en_) & 2) {  // one-shot
      dbgf(1, "one-shot sample: channel 0 = %u, channel 1 = %u", chan_[0], chan_[1]);
      if ((value(ictl_) >> 9) & 1) set_value(istat_, value(istat_) | (1u << 9));
      phase_ = kOff;
      update();
      return;
    }
    uint32_t hits = 0;
    for (unsigned i = 0; i < kNumFilter; ++i) {
      if (matches(value(f0_[i]), chan_[0]) && matches(value(f1_[i]), chan_[1])) hits |= 1u << i;
    }
    // The counter: evaluations hitting the same set of filters.
    if (!hits) count_ = 0;
    else if (!last_hits_ || hits == last_hits_) ++count_;
    else count_ = 0;
    last_hits_ = hits;
    if (dbg_count("adc evaluations", 32)) {
      dbgf(1, "%s pair: channel 0 = %u, channel 1 = %u, filters hit 0x%02x, count %u",
           phase_ == kLp ? "periodic" : "continuous", chan_[0], chan_[1], hits, count_);
    }
    uint64_t next = Eg2System::kNever;
    if (phase_ == kLp) {
      if (count_ >= std::max<uint32_t>(1, value(lp_sample_) & 0xff)) {
        // The move to continuous scanning.
        set_value(fstat_, value(fstat_) | (1u << 8));
        if ((value(ictl_) >> 8) & 1) set_value(istat_, value(istat_) | (1u << 8));
        dbg(1, "periodic scanning hit lp_sample_cnt times: continuous scanning "
               "(FILTER_STATUS.TRANS)");
        phase_ = kNp;
        continuous_ = true;
        count_ = 0;
        last_hits_ = 0;
        next = sample_after(now);
      } else if (hits) {
        // A hit not yet counted out: the next periodic pair.
        phase_ = kSleep;
        next = aon_after(sys_, now, wakeup_time());
      } else {
        // Nothing hits: nothing changes until an input or a setting does, or a queued pair comes.
        phase_ = pairs_.empty() ? kIdle : kSleep;
        if (phase_ == kSleep) next = aon_after(sys_, now, wakeup_time());
      }
    } else {
      uint32_t np = std::max<uint32_t>(1, value(sample_) & 0xffff);
      if (hits && count_ >= np) {
        if (!(value(fstat_) & hits & 0xff)) dbgf(1, "filters 0x%02x matched", hits);
        if (count_ == np) {
          // The values of the first debounced hit.
          for (unsigned c = 0; c < 2; ++c) {
            set_value(val_[c], (value(val_[c]) & ~0x0ffc0000u) | (chan_[c] << 18));
          }
        }
        set_value(fstat_, value(fstat_) | hits);
        set_value(istat_, value(istat_) | (hits & value(ictl_) & 0xff));
        count_ = np;
        if (pairs_.empty()) phase_ = kIdle;
        else next = sample_after(now);
      } else if (hits) {
        next = sample_after(now);
      } else if (value(pd_) & 1) {
        phase_ = kSleep;  // back to periodic scanning
        continuous_ = false;
        next = aon_after(sys_, now, wakeup_time());
      } else {
        if (pairs_.empty()) phase_ = kIdle;
        else next = sample_after(now);
      }
    }
    if (next != Eg2System::kNever) sys_.schedule(this, next);
    update();
  }

 protected:
  void reg_before_read(uint16_t r) override { log_.read(r, value(r)); }
  void reg_write(uint16_t r, uint32_t old, uint32_t, uint32_t) override {
    if (r == en_ || r == pd_ || r == sample_ || r == lp_sample_) {
      if (value(r) != old) {
        dbgf(1, "adc %s, %s mode: power-up %llu cycles, %s", (value(en_) & 1) ? "enabled" : "disabled",
             (value(en_) & 2) ? "one-shot" : (value(pd_) & 1) ? "low-power scan" : "normal scan",
             static_cast<unsigned long long>(pwrup()),
             (value(pd_) & 1) ? "periodic pairs every wakeup_time cycles" : "continuous");
      }
      if (!(value(en_) & 1)) {
        phase_ = kOff;
        sys_.unschedule(this);
      } else if (r == en_ && !(old & 1)) {
        start();
      } else {
        resume();
      }
    } else if (value(r) != old) {
      for (unsigned i = 0; i < kNumFilter; ++i) {
        if (r != f0_[i] && r != f1_[i]) continue;
        uint32_t f = value(r);
        dbgf(1, "filter %u, channel %u: %s, values %s [%u, %u]", i, r == f0_[i] ? 0 : 1,
             (f >> 31) ? "enabled" : "disabled", ((f >> 12) & 1) ? "outside" : "inside",
             (f >> 2) & 0x3ff, (f >> 18) & 0x3ff);
        if (value(en_) & 1) resume();
      }
      if (r == ictl_ || r == wake_) dbgf(1, "%s = 0x%03x", desc().regs[r].name, value(r));
    }
    update();
  }

 private:
  enum Phase { kOff, kLp, kSleep, kNp, kIdle };
  uint64_t pwrup() const { return (value(pd_) >> 4) & 0xf; }
  uint64_t wakeup_time() const { return std::max<uint64_t>(1, value(pd_) >> 8); }
  uint64_t sample_after(uint64_t now) const {
    // 450 us for two ADC samples, then four cycles of the always-on ADC controller clock.
    return aon_after(sys_, now + 450000000ull, 4);
  }
  static bool matches(uint32_t f, uint32_t v) {
    if (!((f >> 31) & 1)) return false;
    uint32_t lo = (f >> 2) & 0x3ff, hi = (f >> 18) & 0x3ff;
    bool inside = v >= lo && v <= hi;
    return ((f >> 12) & 1) ? !inside : inside;
  }
  // Enabled: power up, then the first pair (periodic with lp_mode).
  void start() {
    count_ = 0;
    last_hits_ = 0;
    continuous_ = !(value(pd_) & 1);
    phase_ = (value(en_) & 2) || continuous_ ? kNp : kLp;
    sys_.schedule(this, sample_after(aon_after(sys_, sys_.now_ps(), pwrup())));
  }
  // An input or a setting changed while enabled: a controller that was waiting on a result that
  // could not change takes the next pair (periodic scanning at its next wake).
  void resume() {
    if (phase_ == kOff) return start();
    if (phase_ != kIdle) return;
    phase_ = continuous_ ? kNp : kSleep;
    sys_.schedule(this, sample_after(sys_.now_ps()));
  }
  void update() {
    set_intr_level("match_pending", value(istat_) != 0);
    bool intr = intr_state_ >= 0 && intr_enable_ >= 0 &&
                (value(uint16_t(intr_state_)) & value(uint16_t(intr_enable_)) & 1);
    bool wkup = (value(fstat_) & value(wake_) & 0x1ff) != 0;
    if (wkup != wkup_) {
      wkup_ = wkup;
      dbgf(1, "wake-up request %s", wkup ? "raised" : "withdrawn");
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) {
        if (wkup) pm->wakeup(kEg2WakeAdcCtrl);
        else pm->wakeup_clear(kEg2WakeAdcCtrl);
      }
    }
    log_.state(uint32_t(intr) | (uint32_t(wkup) << 1));
  }

  uint16_t en_, pd_, lp_sample_, sample_, f0_[kNumFilter], f1_[kNumFilter], val_[2], wake_, fstat_,
      ictl_, istat_;
  uint32_t chan_[2] = {0, 0};
  std::deque<std::pair<uint32_t, uint32_t>> pairs_;  // the sample pairs still to come
  Phase phase_ = kOff;
  bool continuous_ = false;  // continuous scanning (not periodic)
  uint32_t count_ = 0, last_hits_ = 0;
  bool wkup_ = false;
  Eg2SysmonLog log_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_ADC_CTRL_H_
