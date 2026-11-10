// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_SYSRST_CTRL_H_
#define EG2_MODEL_IP_EG2_SYSRST_CTRL_H_

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

// sysrst_ctrl (hw/ip/sysrst_ctrl: doc/theory_of_operation.md, data/sysrst_ctrl.hjson). Inputs
// (PIN_IN_VALUE order: pwrb_in, key0_in, key1_in, key2_in, lid_open, ac_present, ec_rst_l,
// flash_wp_l) from --stim sysrst_ctrl.in=<hex>, default 0xcf (buttons released, lid closed, no AC,
// EC and write-protect not asserted), and while running from sysrst_ctrl.in=<hex>@<point> (eg2sim
// --stim), as the chip bench drives the pads; PIN_IN_VALUE reads them raw. KEY_INVERT_CTL inverts
// the key, power-button, AC and lid inputs before use ("Input signals will be inverted before the
// combo detection logic"; the document does not say whether the key-interrupt and ULP logic see
// them inverted too: here every feature does) and the key and power-button outputs after.
//
// Every timer counts the always-on clock. The debounce "samples once when the timer starts and
// then again when the timer ends" (theory_of_operation.md): an input change starts it, changes
// while it runs are not seen, and the event is detected if the input is still at its new level at
// the end. The features, each started by an input change:
//   Key interrupts ("Keyboard and input triggered interrupt"): an H->L or L->H transition of pwrb,
//   key0..2, ac_present, ec_rst_l or flash_wp_l enabled in KEY_INTR_CTL and still there after
//   KEY_INTR_DEBOUNCE_CTL cycles sets its KEY_INTR_STATUS bit.
//   Combos ("Combo detection", COM_SEL_CTL): the selected keys' active-low pressed indications
//   OR'd (the combined trigger is low only while every selected key is low, ac_present included:
//   "HW will detect H2L transition in the combo use case"); its negative edge, while the
//   pre-condition (COM_PRE_SEL_CTL's keys low, none selected: always true) has held for
//   COM_PRE_DET_CTL cycles, is debounced by KEY_INTR_DEBOUNCE_CTL and must then stay low for
//   COM_DET_CTL cycles; a rise or a released pre-condition cancels it. Then COM_OUT_CTL's actions:
//   bat_disable (high until the next reset), interrupt (COMBO_INTR_STATUS), ec_rst (ec_rst_l low
//   for EC_RST_CTL cycles), rst_req (pwrmgr's peripheral reset request 0, "asserted until the next
//   reset").
//   ULP wake-up (ULP_CTL.ulp_enable): ac_present high, a pwrb H->L or a lid_open L->H, held through
//   its ULP_*_DEBOUNCE_CTL, sets ULP_STATUS and z3_wakeup; sticky until ULP_CTL is cleared.
//   EC reset stretching: an H->L of ec_rst_l_i drives ec_rst_l_o low for EC_RST_CTL cycles.
// A key interrupt, a combo's interrupt and the ULP wake-up also set WKUP_STATUS, and the wake-up
// request to pwrmgr (wake-up 0) stands while WKUP_STATUS is set ("an interrupt will also issue a
// wakeup request ... Software should therefore read and clear WKUP_STATUS as well"). event_detected
// (status) follows COMBO_INTR_STATUS | KEY_INTR_STATUS ("triggered by combo or keyboard events",
// the hjson; theory_of_operation.md also names an interrupt for the ULP wake-up, which the hjson's
// interrupt does not list: not raised here). Auto-block is not modelled.
//
// Outputs (PIN_OUT_CTL order: bat_disable, ec_rst_l, pwrb_out, key0_out, key1_out, key2_out,
// z3_wakeup, flash_wp_l): pwrb_out and keyN_out pass their inputs through. flash_wp_l has "no
// bypass path" and "defaults to asserted low" (theory_of_operation.md "Flash Write Protect
// Output"). bat_disable and z3_wakeup are 0 unless set by an action. Assumption: ec_rst_l follows
// its input except while a reset pulse drives it low; the documents describe the bidirectional pin
// and pulse stretching but do not specify this bypass. "Output signals will be inverted after the
// output signal override logic" (theory_of_operation.md "Pin output and keyboard inversion
// control"). The override is enabled by PIN_OUT_CTL and allowed by PIN_ALLOWED_CTL for the value
// PIN_OUT_VALUE gives (allowed_0 bit i for 0, allowed_1 bit i + 8 for 1). Each output change is an
// item in sysrst_ctrl.model.log, the chip bench's format:
// "outputs %08h (bat_disable ec_rst_l pwrb_out key0..2_out z3_wakeup flash_wp_l rst_req = bits
// 0..8)"; reads of the status registers the bench checks are "< <register> %08h" lines.
// Debug lines: the inputs at the start and each change, the key inversion, the pin overrides, each
// combination and the ULP wake-up as configured and when detected, the EC reset pulse, the reset
// request, the outputs as they change, interrupts raised.
class Eg2SysrstCtrl : public Eg2IpModel {
 public:
  static constexpr unsigned kNumCombo = 4;

  Eg2SysrstCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base),
        log_(sys_, inst(), "outputs",
             "(bat_disable ec_rst_l pwrb_out key0..2_out z3_wakeup flash_wp_l rst_req = bits "
             "0..8)") {
    in_val_ = reg("PIN_IN_VALUE");
    invert_ = reg("KEY_INVERT_CTL");
    allowed_ = reg("PIN_ALLOWED_CTL");
    out_ctl_ = reg("PIN_OUT_CTL");
    out_val_ = reg("PIN_OUT_VALUE");
    ec_rst_ctl_ = reg("EC_RST_CTL");
    ulp_ctl_ = reg("ULP_CTL");
    ulp_status_ = reg("ULP_STATUS");
    ulp_deb_[0] = reg("ULP_AC_DEBOUNCE_CTL");
    ulp_deb_[1] = reg("ULP_PWRB_DEBOUNCE_CTL");
    ulp_deb_[2] = reg("ULP_LID_DEBOUNCE_CTL");
    wkup_status_ = reg("WKUP_STATUS");
    combo_status_ = reg("COMBO_INTR_STATUS");
    key_status_ = reg("KEY_INTR_STATUS");
    key_ctl_ = reg("KEY_INTR_CTL");
    key_deb_ = reg("KEY_INTR_DEBOUNCE_CTL");
    for (unsigned i = 0; i < kNumCombo; ++i) {
      std::string s = "_" + std::to_string(i);
      sel_[i] = reg(("COM_SEL_CTL" + s).c_str());
      pre_sel_[i] = reg(("COM_PRE_SEL_CTL" + s).c_str());
      pre_det_[i] = reg(("COM_PRE_DET_CTL" + s).c_str());
      det_[i] = reg(("COM_DET_CTL" + s).c_str());
      com_out_[i] = reg(("COM_OUT_CTL" + s).c_str());
    }
    set_modelled(in_val_);
    raw_ = Eg2PeriphStim::u32("sysrst_ctrl.in", 0xcf) & 0xff;
    log_.reads({{key_status_, "KEY_INTR_STATUS", 0x3fff},
                {combo_status_, "COMBO_INTR_STATUS", 0xf},
                {ulp_status_, "ULP_STATUS", 0x1},
                {wkup_status_, "WKUP_STATUS", 0x1},
                {in_val_, "PIN_IN_VALUE", 0xff}});
    debug_irqs();
    dbgf(1, "inputs 0x%02x (--stim sysrst_ctrl.in; PIN_IN_VALUE order pwrb_in key0_in key1_in "
            "key2_in lid_open ac_present ec_rst_l flash_wp_l = bits 0..7)", raw_);
    // The pads as the chip bench drives them while the test runs.
    sys_.stim_handlers["sysrst_ctrl.in"] = [this](const std::string &v) {
      set_inputs(uint32_t(std::stoul(v, nullptr, 0)) & 0xff);
      return true;
    };
    // chip_sw_sysrst_ctrl_inputs_vseq's pad order is key0, key1, key2, pwrb, ac, lid, ec, wp.
    // Route its six MIO pads through the selectors software programmed. pinmux.hjson says
    // MIO_PERIPH_INSEL "selects the MIO pad driving the peripheral input" and values 0 and 1 are
    // constants while "2 to NMioPads+1 select the corresponding MIO pad".
    sys_.stim_handlers["sysrst_ctrl.pads"] = [this](const std::string &v) {
      set_sequence_pads(uint32_t(std::stoul(v, nullptr, 0)) & 0xff);
      return true;
    };
    // The output pad order used by chip_sw_sysrst_ctrl_outputs_vseq is key0, key1, key2, pwrb,
    // bat_disable, z3_wakeup, ec_rst_l, flash_wp_l. A mismatch is a bench-check failure.
    sys_.stim_handlers["sysrst_ctrl.outputs_expect"] = [this](const std::string &v) {
      size_t slash = v.find('/');
      uint32_t want = uint32_t(std::stoul(v.substr(0, slash), nullptr, 0)) & 0xff;
      uint32_t mask = slash == std::string::npos
                          ? 0xff
                          : uint32_t(std::stoul(v.substr(slash + 1), nullptr, 0)) & 0xff;
      uint32_t got = output_pads();
      char why[160];
      std::snprintf(why, sizeof why,
                    "sysrst_ctrl output pads = 0x%02x, expected 0x%02x under mask 0x%02x", got,
                    want, mask);
      if ((got & mask) != (want & mask)) sys_.finish(false, why);
      else std::printf("eg2sim: check %s: ok\n", why);
      return true;
    };
    // The output sequence first loops the four keyboard inputs back to four output pads for all
    // 16 patterns. Exercise that phase explicitly and restore the sequence's input afterwards.
    sys_.stim_handlers["sysrst_ctrl.loopback_check"] = [this](const std::string &v) {
      if (v != "1") return false;
      if (loopback_checked_) return true;
      loopback_checked_ = true;
      uint32_t saved = raw_;
      for (unsigned pattern = 0; pattern < 16; ++pattern) {
        set_sequence_pads(pattern);
        uint32_t got = output_pads() & 0xf;
        char why[144];
        std::snprintf(why, sizeof why,
                      "sysrst_ctrl loopback output pads = 0x%x for input pattern 0x%x", got,
                      pattern);
        if (got != pattern) {
          sys_.finish(false, why);
          break;
        }
        std::printf("eg2sim: check %s: ok\n", why);
      }
      set_inputs(saved);
      return true;
    };
    restart();
  }
  void on_reset(Eg2ResetCause c) override {
    bool kept = keeps_state(c);
    Eg2IpModel::on_reset(c);
    if (kept) return;
    restart();
  }
  void on_time(uint64_t now) override {
    std::vector<Check> due;
    for (auto it = checks_.begin(); it != checks_.end();) {
      if (it->at <= now) {
        due.push_back(*it);
        it = checks_.erase(it);
      } else {
        ++it;
      }
    }
    for (const Check &c : due) check_due(c, now);
    if (now >= ec_low_until_) {
      ec_low_until_ = Eg2System::kNever;
      dbg(1, "ec_rst_l pulse ended");
    }
    update();
  }

 protected:
  void reg_before_read(uint16_t r) override { log_.read(r, value(r)); }
  void reg_write(uint16_t r, uint32_t old, uint32_t, uint32_t) override {
    for (unsigned i = 0; i < kNumCombo; ++i) {
      if (r == sel_[i] || r == det_[i] || r == pre_sel_[i] || r == pre_det_[i] ||
          r == com_out_[i]) {
        // A new configuration starts from the inputs as they are: a combination held now needs a
        // new negative edge.
        cancel(Check::kCombo, i);
        pre_since_[i] = pre_held(i) ? sys_.now_ps() : Eg2System::kNever;
        if (value(r) != old && dbg_count("combination configuration writes", 16))
          dbgf(1, "combination %u: inputs 0x%02x (key0 key1 key2 pwrb ac_present = bits 0..4) "
                  "pressed %u cycles after the debounce, pre-condition 0x%02x for %u cycles, "
                  "actions %s", i, value(sel_[i]) & 0x1f, value(det_[i]),
               value(pre_sel_[i]) & 0x1f, value(pre_det_[i]), actions(value(com_out_[i])).c_str());
      }
    }
    if (r == ulp_ctl_) {
      if (!(value(ulp_ctl_) & 1)) {
        // "disable ULP wakeup feature and reset the ULP FSM".
        for (unsigned k = 0; k < 3; ++k) cancel(Check::kUlp, k);
        z3_ = false;
      } else if (!(old & 1)) {
        // A high level of ac_present is a condition of its own (not an edge).
        if ((eff() >> 5) & 1) arm(Check::kUlp, 0, value(ulp_deb_[0]) & 0xffff);
      }
      dbgf(1, "ULP_CTL.ulp_enable %u", value(ulp_ctl_) & 1);
    } else if (r == wkup_status_ && !(value(wkup_status_) & 1) && (old & 1)) {
      // Cleared by software: the wake-up request is withdrawn.
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup_clear(kEg2WakeSysrstCtrl);
    } else if ((r == invert_ || r == out_ctl_ || r == out_val_ || r == allowed_ ||
                r == ec_rst_ctl_ || r == key_ctl_ || r == key_deb_) &&
               value(r) != old) {
      dbgf(1, "%s = 0x%08x", desc().regs[r].name, value(r));
    }
    update();
  }

 private:
  uint32_t pinmux_value(const std::string &name) const {
    auto *pinmux = sys_.model("pinmux");
    int r = pinmux ? pinmux->reg_index(name) : -1;
    return r < 0 ? 0 : pinmux->value(uint16_t(r));
  }
  unsigned mio_input(unsigned peripheral, uint64_t mio) const {
    unsigned sel = pinmux_value("MIO_PERIPH_INSEL_" + std::to_string(peripheral)) & 0x3f;
    return sel < 2 ? sel : unsigned(mio >> (sel - 2)) & 1;
  }
  void set_sequence_pads(uint32_t p) {
    // Native MIO pad indices used by chip_sw_sysrst_ctrl_{inputs,outputs}_vseq.
    static const unsigned kInputMio[6] = {12, 15, 17, 46, 29, 31};
    uint64_t mio = 0;
    for (unsigned i = 0; i < 6; ++i) mio |= uint64_t((p >> i) & 1) << kInputMio[i];
    // sysrst_ctrl peripheral input indices: ac, key0, key1, key2, pwrb, lid = 54..59.
    uint32_t raw = (mio_input(58, mio) << 0) | (mio_input(55, mio) << 1) |
                   (mio_input(56, mio) << 2) | (mio_input(57, mio) << 3) |
                   (mio_input(59, mio) << 4) | (mio_input(54, mio) << 5) | (p & 0xc0);
    set_inputs(raw);
  }
  unsigned mio_output(unsigned pad) const {
    // pinmux.hjson says MIO_OUTSEL values 0, 1, and 2 select constant zero, constant one, and
    // high-Z; "3 to NOutputs+2 select the corresponding peripheral output".
    unsigned sel = pinmux_value("MIO_OUTSEL_" + std::to_string(pad)) & 0x7f;
    if (sel < 3) return sel == 1;
    unsigned peripheral = sel - 3;
    // Native sysrst_ctrl outputs 64..69 are bat, key0, key1, key2, pwrb, z3.
    static const unsigned kOutputBit[6] = {0, 3, 4, 5, 2, 6};
    return peripheral >= 64 && peripheral < 70
               ? unsigned(outputs_ >> kOutputBit[peripheral - 64]) & 1
               : 0;
  }
  uint32_t output_pads() const {
    // Physical MIO output pads used by chip_sw_sysrst_ctrl_outputs_vseq, followed by the two DIOs.
    static const unsigned kOutputMio[6] = {18, 40, 41, 29, 31, 16};
    uint32_t p = 0;
    for (unsigned i = 0; i < 6; ++i) p |= mio_output(kOutputMio[i]) << i;
    return p | (((outputs_ >> 1) & 1) << 6) | (((outputs_ >> 7) & 1) << 7);
  }

  // A debounce or detection timer: its kind, its index (KEY_INTR bit, combo, ULP input), its end.
  struct Check {
    enum Kind { kKey, kCombo, kUlp } kind;
    unsigned idx;
    uint64_t at;
  };
  // COM_OUT_CTL's actions in words.
  static std::string actions(uint32_t act) {
    std::string s;
    if (act & 1) s += " bat_disable";
    if (act & 2) s += " interrupt";
    if (act & 4) s += " ec_rst";
    if (act & 8) s += " rst_req (chip reset)";
    return s.empty() ? "none" : s.substr(1);
  }
  // The inputs after KEY_INVERT_CTL (pwrb, key0-2, lid_open, ac_present inverted by their _in bit).
  uint32_t eff() const { return eff_of(raw_); }
  uint32_t eff_of(uint32_t raw) const {
    uint32_t inv = value(invert_), v = raw;
    static const unsigned kInBit[6] = {6, 0, 2, 4, 10, 8};  // pwrb, key0..2, lid, ac -> KEY_INVERT bit
    for (unsigned i = 0; i < 6; ++i) v ^= ((inv >> kInBit[i]) & 1) << i;
    return v;
  }
  // The level of KEY_INTR_CTL's input i (pwrb, key0, key1, key2, ac_present, ec_rst_l, flash_wp_l).
  static unsigned key_level(uint32_t e, unsigned i) {
    static const unsigned kPinBit[7] = {0, 1, 2, 3, 5, 6, 7};
    return (e >> kPinBit[i]) & 1;
  }
  // COM_SEL order (key0, key1, key2, pwrb, ac_present): pressed is low.
  static uint32_t pressed_of(uint32_t e) {
    return ((~e >> 1) & 1) | (((~e >> 2) & 1) << 1) | (((~e >> 3) & 1) << 2) | ((~e & 1) << 3) |
           (((~e >> 5) & 1) << 4);
  }
  bool trigger_low(unsigned i, uint32_t e) const {
    uint32_t sel = value(sel_[i]) & 0x1f;
    return sel && (pressed_of(e) & sel) == sel;
  }
  bool pre_held(unsigned i) const {
    uint32_t pre = value(pre_sel_[i]) & 0x1f;
    return (pressed_of(eff()) & pre) == pre;
  }
  bool pre_ready(unsigned i, uint64_t now) const {
    if (!(value(pre_sel_[i]) & 0x1f)) return true;
    return pre_since_[i] != Eg2System::kNever &&
           now >= aon_after(sys_, pre_since_[i], uint64_t(value(pre_det_[i])));
  }

  bool armed(Check::Kind k, unsigned idx) const {
    for (const Check &c : checks_) {
      if (c.kind == k && c.idx == idx) return true;
    }
    return false;
  }
  void arm(Check::Kind k, unsigned idx, uint64_t cycles) {
    if (armed(k, idx)) return;
    checks_.push_back({k, idx, aon_after(sys_, sys_.now_ps(), cycles)});
  }
  void cancel(Check::Kind k, unsigned idx) {
    for (auto it = checks_.begin(); it != checks_.end();) {
      it = it->kind == k && it->idx == idx ? checks_.erase(it) : it + 1;
    }
  }

  // The pads change: start each feature's timer on the change it watches.
  void set_inputs(uint32_t raw) {
    uint32_t old_e = eff(), old_raw = raw_;
    raw_ = raw;
    uint32_t e = eff();
    if (raw != old_raw) dbgf(1, "inputs 0x%02x (were 0x%02x)", raw, old_raw);
    uint32_t kctl = value(key_ctl_);
    for (unsigned i = 0; i < 7; ++i) {
      unsigned was = key_level(old_e, i), is = key_level(e, i);
      if (was == is) continue;
      unsigned bit = is ? i + 7 : i;  // L2H bits follow the seven H2L bits
      if ((kctl >> bit) & 1) arm(Check::kKey, bit, value(key_deb_) & 0xffff);
    }
    for (unsigned i = 0; i < kNumCombo; ++i) {
      bool held = pre_held(i);
      if (!held) {
        pre_since_[i] = Eg2System::kNever;
        cancel(Check::kCombo, i);
      } else if (pre_since_[i] == Eg2System::kNever) {
        pre_since_[i] = sys_.now_ps();
      }
      bool was = trigger_low(i, old_e), is = trigger_low(i, e);
      if (was && !is) cancel(Check::kCombo, i);
      if (!was && is && pre_ready(i, sys_.now_ps())) {
        arm(Check::kCombo, i,
            uint64_t(value(key_deb_) & 0xffff) + uint64_t(value(det_[i])));
      }
    }
    if (value(ulp_ctl_) & 1) {
      if (!((old_e >> 5) & 1) && ((e >> 5) & 1)) arm(Check::kUlp, 0, value(ulp_deb_[0]) & 0xffff);
      if ((old_e & 1) && !(e & 1)) arm(Check::kUlp, 1, value(ulp_deb_[1]) & 0xffff);
      if (!((old_e >> 4) & 1) && ((e >> 4) & 1)) arm(Check::kUlp, 2, value(ulp_deb_[2]) & 0xffff);
    }
    // The hardwired reset stretching: ec_rst_l_i asserted (high to low).
    if (((old_raw >> 6) & 1) && !((raw >> 6) & 1)) ec_pulse();
    update();
  }

  void check_due(const Check &c, uint64_t now) {
    uint32_t e = eff();
    if (c.kind == Check::kKey) {
      unsigned i = c.idx % 7, want = c.idx >= 7;
      if (key_level(e, i) != want || !((value(key_ctl_) >> c.idx) & 1)) return;
      set_value(key_status_, value(key_status_) | (1u << c.idx));
      dbgf(1, "input change detected: KEY_INTR_STATUS bit %u", c.idx);
      event();
    } else if (c.kind == Check::kCombo) {
      unsigned i = c.idx;
      if (!trigger_low(i, e) || !pre_held(i)) return;
      uint32_t act = value(com_out_[i]);
      dbgf(1, "combination %u detected (COM_SEL_CTL_%u 0x%02x): actions %s", i, i,
           value(sel_[i]) & 0x1f, actions(act).c_str());
      if (act & 1) bat_disable_ = true;
      if (act & 4) ec_pulse();
      if (act & 2) {
        set_value(combo_status_, value(combo_status_) | (1u << i));
        event();
      }
      if ((act & 8) && !rst_req_) {
        rst_req_ = true;
        dbg(1, "reset request to pwrmgr (peripheral reset request 0)");
        if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->reset_request(0);
      }
    } else {
      static const unsigned kBit[3] = {5, 0, 4};  // ac_present, pwrb, lid_open
      static const unsigned kWant[3] = {1, 0, 1};
      if (!(value(ulp_ctl_) & 1) || ((e >> kBit[c.idx]) & 1) != kWant[c.idx]) return;
      set_value(ulp_status_, 1);
      z3_ = true;
      dbg(1, "ultra-low-power wake-up detected: ULP_STATUS set, z3_wakeup high");
      event();
    }
    (void)now;
  }
  void ec_pulse() {
    ec_low_until_ = aon_after(sys_, sys_.now_ps(), uint64_t(value(ec_rst_ctl_) & 0xffff));
    dbgf(1, "ec_rst_l driven low for %u always-on cycles (EC_RST_CTL)",
         value(ec_rst_ctl_) & 0xffff);
  }
  void event() {
    set_value(wkup_status_, 1);
    eg2_request_wakeup(sys_, kEg2WakeSysrstCtrl);
  }
  // After a reset of the block: no timer runs, the actions are released, the pre-conditions are
  // timed from now.
  void restart() {
    checks_.clear();
    ec_low_until_ = Eg2System::kNever;
    bat_disable_ = z3_ = rst_req_ = false;
    for (unsigned i = 0; i < kNumCombo; ++i) {
      pre_since_[i] = pre_held(i) ? sys_.now_ps() : Eg2System::kNever;
    }
    if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup_clear(kEg2WakeSysrstCtrl);
    update();
  }
  void update() {
    set_value(in_val_, raw_);
    uint32_t e = eff(), inv = value(invert_);
    uint32_t def = (uint32_t(bat_disable_) << 0) |
                   (uint32_t(((raw_ >> 6) & 1) && ec_low_until_ == Eg2System::kNever) << 1) |
                   ((e & 1) << 2) | (((e >> 1) & 1) << 3) | (((e >> 2) & 1) << 4) |
                   (((e >> 3) & 1) << 5) | (uint32_t(z3_) << 6);
    uint32_t ctl = value(out_ctl_), ov = value(out_val_), al = value(allowed_), out = def;
    for (unsigned i = 0; i < 8; ++i) {
      unsigned v = (ov >> i) & 1;
      bool ok = (al >> (i + (v ? 8 : 0))) & 1;
      if (((ctl >> i) & 1) && ok) out = (out & ~(1u << i)) | (v << i);
    }
    // KEY_INVERT_CTL output bits in PIN_OUT_CTL order are 9, none, 7, 1, 3, 5, 11, none.
    // The KEY_INVERT_CTL descriptions include bat_disable and z3_wakeup; ec_rst_l and flash_wp_l
    // have no inversion fields (sysrst_ctrl.hjson KEY_INVERT_CTL).
    static const int kOutInvBit[8] = {9, -1, 7, 1, 3, 5, 11, -1};
    for (unsigned i = 0; i < 8; ++i) {
      if (kOutInvBit[i] >= 0) out ^= ((inv >> kOutInvBit[i]) & 1) << i;
    }
    out |= uint32_t(rst_req_) << 8;
    if (out != outputs_) {
      dbgf(1, "outputs 0x%03x (bat_disable %u, ec_rst_l %u, pwrb_out %u, key0..2_out %u%u%u, "
              "z3_wakeup %u, flash_wp_l %u, rst_req %u)", out, out & 1, (out >> 1) & 1,
           (out >> 2) & 1, (out >> 3) & 1, (out >> 4) & 1, (out >> 5) & 1, (out >> 6) & 1,
           (out >> 7) & 1, (out >> 8) & 1);
      outputs_ = out;
    }
    log_.state(out);
    set_intr_level("event_detected", (value(combo_status_) | value(key_status_)) != 0);
    uint64_t t = ec_low_until_;
    for (const Check &c : checks_) t = std::min(t, c.at);
    if (t == Eg2System::kNever) sys_.unschedule(this);
    else sys_.schedule(this, t);
  }

  uint16_t in_val_, invert_, allowed_, out_ctl_, out_val_, ec_rst_ctl_, ulp_ctl_, ulp_status_,
      ulp_deb_[3], wkup_status_, combo_status_, key_status_, key_ctl_, key_deb_;
  uint16_t sel_[kNumCombo], pre_sel_[kNumCombo], pre_det_[kNumCombo], det_[kNumCombo],
      com_out_[kNumCombo];
  uint32_t raw_ = 0xcf, outputs_ = 0xffffffffu;
  std::vector<Check> checks_;
  uint64_t pre_since_[kNumCombo] = {Eg2System::kNever, Eg2System::kNever, Eg2System::kNever,
                                    Eg2System::kNever};
  uint64_t ec_low_until_ = Eg2System::kNever;
  bool bat_disable_ = false, z3_ = false, rst_req_ = false;
  bool loopback_checked_ = false;
  Eg2SysmonLog log_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_SYSRST_CTRL_H_
