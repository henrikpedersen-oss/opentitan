// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_PINMUX_H_
#define EG2_MODEL_IP_EG2_PINMUX_H_

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

// pinmux: the wake-up detectors (WKUP_DETECTOR*, WKUP_CAUSE); the pad and select registers stay
// registers (the model does not route pads). The pads' input levels come from --stim
// pinmux.mio=<hex> (MIO pad n is bit n) and pinmux.dio=<hex> (DIO pad n), default 0, and change
// while the model runs as the chip bench drives them: pinmux.mio=<hex>@<point>, pinmux.dio=...,
// or one pad, pinmux.pad=<0 MIO | 1 DIO>:<pad>:<level>@<point> (eg2sim --stim).
// Detector i, enabled, watches the pad WKUP_DETECTOR_PADSEL selects: for MIO "the pad select index
// is the same as used for MIO_PERIPH_INSEL, meaning that index 0 and 1 just select constants 0 and
// 1, and the MIO pads live at indices >= 2"; for DIO it "corresponds 1:1 to the DIO pad"
// (pinmux registers.md WKUP_DETECTOR_PADSEL). "The input signal is sampled with the AON clock"
// and "needs to remain stable for at least one AON clock cycle after a level change for the
// detector to recognize the event"; with WKUP_DETECTOR.FILTER "the signal must be stable for 4
// always-on clock cycles" (theory_of_operation.md "Wakeup Detectors", registers.md). A level the
// detector has recognised is an edge for modes 0 (rising), 1 (falling) and 2 (any); for modes 3
// (timed high) and 4 (timed low) the level held for WKUP_DETECTOR_CNT_TH + 1 cycles. A detection
// sets bit i of WKUP_CAUSE and requests wake-up 2. The documents do not say how long the request
// stands: here while WKUP_CAUSE is not 0 (software clears it, programmers_guide.md step 6).
// Assumption: enabling a detector starts it from the pad's level then (no edge); the documents do
// not define whether an already-active edge or level is observed at enable.
// Items in pinmux.model.log, the chip bench's format (its wake-up monitor): "pin_wkup_req %08h"
// when the wake-up request (WKUP_CAUSE not 0) changes, "< WKUP_CAUSE %08h" for each read of it.
// Debug lines: each detector as configured (mode, pad, its level, threshold), the pads as they
// change, when a detector fires, WKUP_CAUSE cleared; the pad routing and attribute writes
// (MIO_OUTSEL, MIO_PERIPH_INSEL, pad attributes and sleep modes) by register and value, the first
// 64 of them at level 1.
class Eg2Pinmux : public Eg2IpModel {
 public:
  static constexpr unsigned kNumDet = 8;
  static constexpr unsigned kNumMio = 47;
  static constexpr unsigned kNumDio = 16;

  Eg2Pinmux(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), log_(sys_, inst(), "pin_wkup_req", "") {
    for (unsigned i = 0; i < kNumDet; ++i) {
      std::string s = "_" + std::to_string(i);
      en_[i] = reg(("WKUP_DETECTOR_EN" + s).c_str());
      det_[i] = reg(("WKUP_DETECTOR" + s).c_str());
      th_[i] = reg(("WKUP_DETECTOR_CNT_TH" + s).c_str());
      sel_[i] = reg(("WKUP_DETECTOR_PADSEL" + s).c_str());
    }
    for (unsigned i = 0; i < kNumMio; ++i) {
      std::string s = "_" + std::to_string(i);
      mio_sleep_en_[i] = reg(("MIO_PAD_SLEEP_EN" + s).c_str());
      mio_sleep_mode_[i] = reg(("MIO_PAD_SLEEP_MODE" + s).c_str());
    }
    for (unsigned i = 0; i < kNumDio; ++i) {
      std::string s = "_" + std::to_string(i);
      dio_sleep_en_[i] = reg(("DIO_PAD_SLEEP_EN" + s).c_str());
      dio_sleep_mode_[i] = reg(("DIO_PAD_SLEEP_MODE" + s).c_str());
    }
    cause_ = reg("WKUP_CAUSE");  // one bit per detector
    auto start = [](const char *key) {
      auto it = Eg2PeriphStim::all().find(key);
      return it == Eg2PeriphStim::all().end() ? 0ull : std::stoull(it->second, nullptr, 0);
    };
    mio_ = start("pinmux.mio");
    dio_ = start("pinmux.dio");
    log_.reads({{cause_, "WKUP_CAUSE", 0xff}});
    log_.state(0);
    sys_.stim_handlers["pinmux.mio"] = [this](const std::string &v) {
      set_pads(std::stoull(v, nullptr, 0), dio_);
      return true;
    };
    sys_.stim_handlers["pinmux.dio"] = [this](const std::string &v) {
      set_pads(mio_, std::stoull(v, nullptr, 0));
      return true;
    };
    sys_.stim_handlers["pinmux.pad"] = [this](const std::string &v) {
      unsigned type = 0, pad = 0, level = 0;
      if (std::sscanf(v.c_str(), "%u:%u:%u", &type, &pad, &level) != 3 || type > 1 || pad > 63) {
        return false;
      }
      uint64_t m = type ? dio_ : mio_, bit = 1ull << pad;
      m = level ? (m | bit) : (m & ~bit);
      set_pads(type ? mio_ : m, type ? m : dio_);
      return true;
    };
    retention_check_setup();
    for (unsigned i = 0; i < kNumDet; ++i) restart(i);
  }
  void on_reset(Eg2ResetCause c) override {
    bool kept = keeps_state(c);
    Eg2IpModel::on_reset(c);
    if (!kept) {
      for (unsigned i = 0; i < kNumDet; ++i) restart(i);
      update_request();
    }
    log_.state(value(cause_) != 0);
  }
  void on_time(uint64_t now) override {
    for (unsigned i = 0; i < kNumDet; ++i) {
      if (now >= sample_at_[i]) {
        // The new level has been stable long enough to be seen.
        sample_at_[i] = Eg2System::kNever;
        unsigned lv = level(i);
        if (lv != seen_[i]) {
          seen_[i] = lv;
          recognise(i, now);
        }
      }
      if (now >= fire_at_[i]) {
        fire_at_[i] = Eg2System::kNever;
        fire(i);
      }
    }
    log_.state(value(cause_) != 0);
    plan();
  }

 protected:
  void reg_before_read(uint16_t r) override { log_.read(r, value(r)); }
  void reg_write(uint16_t r, uint32_t old, uint32_t, uint32_t) override {
    bool detector = false;
    for (unsigned i = 0; i < kNumDet; ++i) {
      if (r != en_[i] && r != det_[i] && r != th_[i] && r != sel_[i]) continue;
      detector = true;
      restart(i);
      if (value(r) != old && (value(en_[i]) & 1)) {
        static const char *kMode[8] = {"rising edge", "falling edge", "any edge", "timed high",
                                       "timed low", "mode 5", "mode 6", "mode 7"};
        bool dio = (value(det_[i]) >> 4) & 1;
        dbgf(1, "wake-up detector %u: %s on %s pad select %u (level %u), threshold %u cycles%s", i,
             kMode[value(det_[i]) & 7], dio ? "DIO" : "MIO", value(sel_[i]) & 0x3f, level(i),
             value(th_[i]), ((value(det_[i]) >> 3) & 1) ? ", filtered" : "");
      } else if (r == en_[i] && value(r) != old) {
        dbgf(1, "wake-up detector %u disabled", i);
      }
    }
    if (r == cause_ && value(r) != old) {
      dbgf(1, "WKUP_CAUSE 0x%02x (was 0x%02x)", value(r), old);
      update_request();
    } else if (!detector && value(r) != old && dbg_count("pinmux configuration writes", 64)) {
      dbgf(1, "%s = 0x%08x", desc().regs[r].name, value(r));
    }
    log_.state(value(cause_) != 0);
    plan();
  }

 private:
  bool enabled(unsigned i) const { return value(en_[i]) & 1; }
  unsigned mode(unsigned i) const {
    unsigned m = value(det_[i]) & 7;
    return m > 4 ? 0 : m;  // "Out of range values default to Posedge"
  }
  // The level of the pad detector i selects.
  unsigned level(unsigned i) const {
    unsigned sel = value(sel_[i]) & 0x3f;
    if ((value(det_[i]) >> 4) & 1) return unsigned(dio_ >> sel) & 1;
    if (sel < 2) return sel;
    return unsigned(mio_ >> (sel - 2)) & 1;
  }
  // Assumption above: start over from the pad's level, with no edge and a timed level from now.
  void restart(unsigned i) {
    seen_[i] = level(i);
    sample_at_[i] = fire_at_[i] = Eg2System::kNever;
    if (enabled(i)) recognise(i, sys_.now_ps(), /*edges=*/false);
  }
  // Detector i has seen its pad change to seen_[i].
  void recognise(unsigned i, uint64_t now, bool edges = true) {
    if (!enabled(i)) return;
    unsigned m = mode(i), lv = seen_[i];
    if (m <= 2) {
      if (edges && (m == 2 || (m == 0) == (lv == 1))) fire(i);
      return;
    }
    fire_at_[i] = Eg2System::kNever;
    if ((m == 3) == (lv == 1)) fire_at_[i] = aon_after(sys_, now, uint64_t(value(th_[i])) + 1);
  }
  void fire(unsigned i) {
    set_value(cause_, value(cause_) | (1u << i));
    dbgf(1, "wake-up detector %u fired: WKUP_CAUSE bit %u set, wake-up requested", i, i);
    update_request();
  }
  // The pads change: each enabled detector whose pad changed samples it after the stable time.
  void set_pads(uint64_t mio, uint64_t dio) {
    if (mio != mio_ || dio != dio_) {
      dbgf(1, "pads MIO 0x%012llx DIO 0x%06llx", (unsigned long long)mio, (unsigned long long)dio);
    }
    mio_ = mio;
    dio_ = dio;
    for (unsigned i = 0; i < kNumDet; ++i) {
      if (!enabled(i)) continue;
      if (level(i) == seen_[i]) {
        sample_at_[i] = Eg2System::kNever;  // back before it was seen: a glitch
        continue;
      }
      // Seen at the next always-on clock edge it held through (the filter: 4 cycles).
      uint64_t stable = ((value(det_[i]) >> 3) & 1) ? 4 : 1;
      if (sample_at_[i] == Eg2System::kNever) sample_at_[i] = aon_after(sys_, sys_.now_ps(), stable);
    }
    plan();
  }
  void update_request() {
    bool want = (value(cause_) & 0xff) != 0;
    if (want == req_) return;
    req_ = want;
    if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) {
      if (want) pm->wakeup(kEg2WakePinmuxPin);
      else pm->wakeup_clear(kEg2WakePinmuxPin);
    }
  }
  void plan() {
    uint64_t t = Eg2System::kNever;
    for (unsigned i = 0; i < kNumDet; ++i) t = std::min({t, sample_at_[i], fire_at_[i]});
    if (t == Eg2System::kNever) sys_.unschedule(this);
    else sys_.schedule(this, t);
  }
  // ---- The pads in sleep (chip_sw_sleep_pin_mio_dio_val_vseq's checks, made by the model) ----
  // Retention logic (theory_of_operation.md "Retention Logic"): "all muxing matrix and DIO outputs
  // are routed through the retention logic", which "can be configured to be automatically
  // activated upon sleep entry in order to either drive the output low, high, high-Z or to the last
  // seen value (keep)"; "the sleep behavior of all outputs is activated in parallel via a trigger
  // signal asserted by the power manager". Per pad, MIO_PAD_SLEEP_EN / DIO_PAD_SLEEP_EN.EN and
  // MIO_PAD_SLEEP_MODE / DIO_PAD_SLEEP_MODE.OUT (registers.md): 0 Tie-Low "driven actively to
  // zero", 1 Tie-High "driven actively to one", 2 High-Z "left undriven ... the actual driving
  // behavior during deep sleep will then depend on the pull-up/-down configuration in
  // MIO_PAD_ATTR" (pull_en bit 2, pull_select bit 3: 0 pull-down, 1 pull-up), 3 Keep "last driven
  // value". An external pull on an undriven pad decides its level (the bench's pins_pd/pins_pu).
  // The software's choice per pad comes from its log ("BEGIN Chosen Retention Types", "DIO [<i>]:
  // <v>", "MIO [<i>]: <v>", "END Chosen Retention Types"), as the vseq reads it. --stim
  // pinmux.sleep_pad_check=1@<point> (in low power) compares every chosen pad's level with the
  // choice three times, with no external pull, all pulled down and all pulled up (the vseq's
  // three passes); Keep pads are not checked (the vseq skips them). One bench check per pass, then
  // the bench's verdict ends the run, as the vseq's override_test_status_and_finish. With --stim
  // bench.inject_fail=1 the first chosen pad's expectation is inverted (the check must go red).
  void retention_check_setup() {
    for (unsigned i = 0; i < kNumMio; ++i) mio_attr_[i] = reg(("MIO_PAD_ATTR_" + std::to_string(i)).c_str());
    for (unsigned i = 0; i < kNumDio; ++i) dio_attr_[i] = reg(("DIO_PAD_ATTR_" + std::to_string(i)).c_str());
    for (int &c : mio_chosen_) c = -1;
    for (int &c : dio_chosen_) c = -1;
    sys_.sw_log_watchers.push_back([this](const std::string &m) {
      if (m == "BEGIN Chosen Retention Types") {
        listing_ = true;
        for (int &c : mio_chosen_) c = -1;
        for (int &c : dio_chosen_) c = -1;
      } else if (m == "END Chosen Retention Types") {
        listing_ = false;
      } else if (listing_) {
        unsigned i = 0, v = 0;
        if (std::sscanf(m.c_str(), "DIO [%u]: %x", &i, &v) == 2 && i < kNumDio) dio_chosen_[i] = int(v);
        if (std::sscanf(m.c_str(), "MIO [%u]: %x", &i, &v) == 2 && i < kNumMio) mio_chosen_[i] = int(v);
      }
    });
    sys_.stim_handlers["pinmux.sleep_pad_check"] = [this](const std::string &) {
      sleep_pad_check();
      return true;
    };
  }
  // The pad's level in sleep: 0, 1, 'z' (undriven, no pull), 'k' (Keep: not checked), 'o' (no
  // sleep behaviour enabled: the peripheral's output, which the model does not drive).
  char sleep_level(bool dio, unsigned i, int ext_pull) const {
    uint32_t en = value(dio ? dio_sleep_en_[i] : mio_sleep_en_[i]) & 1;
    uint32_t mode = value(dio ? dio_sleep_mode_[i] : mio_sleep_mode_[i]) & 3;
    uint32_t attr = value(dio ? dio_attr_[i] : mio_attr_[i]);
    if (!en) return 'o';
    if (mode == 0) return '0';
    if (mode == 1) return '1';
    if (mode == 3) return 'k';
    if (ext_pull >= 0) return char('0' + ext_pull);
    if ((attr >> 2) & 1) return char('0' + ((attr >> 3) & 1));
    return 'z';
  }
  void sleep_pad_check() {
    static const char *const kPass[3] = {"no pull", "pull-down", "pull-up"};
    bool inverted = sys_.bench_inject_fail;
    for (int pass = 0; pass < 3; ++pass) {
      int ext = pass == 0 ? -1 : pass == 1 ? 0 : 1;
      unsigned checked = 0, bad = 0;
      std::string first;
      bool flip = inverted;
      for (int d = 0; d < 2; ++d) {
        unsigned n = d ? kNumDio : kNumMio;
        for (unsigned i = 0; i < n; ++i) {
          int c = d ? dio_chosen_[i] : mio_chosen_[i];
          if (c < 0 || c == 3) continue;
          char want = c == 0 ? '0' : c == 1 ? '1' : ext < 0 ? 'z' : char('0' + ext);
          if (flip) {
            want = want == '1' ? '0' : '1';
            flip = false;
          }
          char got = sleep_level(d, i, ext);
          ++checked;
          if (got != want && !bad++) {
            first = std::string(d ? "DIO[" : "MIO[") + std::to_string(i) + "] chosen " +
                    std::to_string(c) + ": expected " + want + ", the pad is " + got;
          }
        }
      }
      if (sys_.bench_check) {
        sys_.bench_check(std::string("pad retention, ") + kPass[pass], checked > 0 && bad == 0,
                         std::to_string(checked) + " pads, " + std::to_string(bad) + " wrong" +
                             (bad ? " (first: " + first + ")" : ""));
      }
    }
    if (sys_.bench_end) sys_.bench_end("the pads in sleep checked");
  }
  uint16_t mio_attr_[kNumMio], dio_attr_[kNumDio];
  int mio_chosen_[kNumMio] = {}, dio_chosen_[kNumDio] = {};
  bool listing_ = false;

  uint16_t en_[kNumDet], det_[kNumDet], th_[kNumDet], sel_[kNumDet], cause_;
  uint16_t mio_sleep_en_[kNumMio], mio_sleep_mode_[kNumMio];
  uint16_t dio_sleep_en_[kNumDio], dio_sleep_mode_[kNumDio];
  uint64_t sample_at_[kNumDet], fire_at_[kNumDet];
  unsigned seen_[kNumDet];
  uint64_t mio_ = 0, dio_ = 0;
  bool req_ = false;
  Eg2SysmonLog log_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PINMUX_H_
