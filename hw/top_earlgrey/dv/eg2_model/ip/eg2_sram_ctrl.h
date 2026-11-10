// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_SRAM_CTRL_H_
#define EG2_MODEL_IP_EG2_SRAM_CTRL_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_core_system.h"
#include "eg2_mem_otp.h"

namespace eg2_model {

// sram_ctrl (hw/ip/sram_ctrl: doc/theory_of_operation.md, programmer's guide, data/sram_ctrl.hjson).
// CTRL.RENEW_SCR_KEY obtains a new key from OTP at once: STATUS.SCR_KEY_VALID, SCR_KEY_SEED_VALID
// while OTP's SECRET1 is locked ("if the key seeds have not yet been provisioned in OTP, the keys
// are derived from all-zero constants, and seed_valid is 0"), and SCR_KEY_ROTATED becomes
// MuBi4True (software clears it to MuBi4False by writing MuBi4True). CTRL.INIT overwrites the
// whole RAM ("LFSR Initialization Feature") and sets STATUS.INIT_DONE: with zeros where the instance
// is built with SecZeroInit = 1 (only sram_ctrl_meta in top_earlgrey.gen.hjson, so CHERIoT mode
// starts with no tag), with pseudo-random data otherwise. A key renewal without INIT turns the old
// contents into garbage ("a key renewal makes old contents unreadable"). Code execution from the RAM
// (exec_enabled(), for whoever needs it; the RAM itself is a DMI memory): only on the instances built
// with InstrExec = 1 (main and sec), then as the doc's table: EXEC.EN == MuBi4True if OTP's
// EN_SRAM_IFETCH is MuBi8True, the life cycle's HW_DEBUG_EN otherwise. Not modelled: scrambling
// itself, the integrity errors garbage data gives, readback checks, escalation.
class Eg2SramCtrl : public Eg2IpModel {
 public:
  Eg2SramCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    status_ = reg("STATUS");
    exec_ = reg("EXEC");
    rotated_ = reg("SCR_KEY_ROTATED");
    set_modelled(status_);
    std::string w = d.window;
    inst_ = w.substr(0, w.find('.'));
    ram_ = inst_ + ".ram";
    zero_init_ = inst_ == "sram_ctrl_meta";
    instr_exec_ = inst_ == "sram_ctrl_main" || inst_ == "sram_ctrl_sec";
    // Fetches only while execution is enabled (theory_of_operation.md "Code Execution from SRAM",
    // the table). The doc does not say how a refused fetch answers; the RTL's TL-UL adapter answers
    // it with an error (sram_ctrl.sv, en_ifetch), an instruction access fault on the core.
    sys_.add_exec_region(ram_, [this] { return exec_enabled(); });
  }

  bool exec_enabled() const {
    if (!instr_exec_) return false;
    auto *o = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    if (o && o->en_sram_ifetch()) return (value(exec_) & 0xf) == kMuBi4True;
    auto *l = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    return l && l->hw_debug_en();
  }

  // Out of reset the controller uses its predefined key again, so data written under a renewed key
  // reads as garbage. The retention SRAM's controller keeps its key through a low-power exit
  // (sram_ctrl_sleep_sram_ret_contents_scramble_test: preserved after deep sleep, garbage after a
  // watchdog reset); its registers follow the model's always-on rule (Eg2IpModel::keeps_state:
  // kept through a low-power exit only).
  void on_reset(Eg2ResetCause c) override {
    bool ret = inst_ == "sram_ctrl_ret";
    if (keyed_ && !(ret && c == Eg2ResetCause::kLowPowerExit)) {
      garble();
      keyed_ = false;
      if (ret) {
        set_value(status_, value(status_) & ~((1u << 3) | (1u << 4)));
        set_value(rotated_, kMuBi4False);
      }
    }
    Eg2IpModel::on_reset(c);
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == rotated_) {
      // rw1c of a MuBi4: writing MuBi4True clears it to MuBi4False.
      if ((w & 0xf) == kMuBi4True) set_value(rotated_, kMuBi4False);
      return;
    }
    if (r != ctrl_) return;
    if (w & 1) {
      auto *o = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
      bool seed = o && o->sram_key_seed_valid();
      set_value(status_, (value(status_) & ~(1u << 4)) | (1u << 3) | (seed ? (1u << 4) : 0));
      set_value(rotated_, kMuBi4True);
      // "A key renewal makes old contents unreadable": the old data reads back as garbage (the
      // chip's integrity errors on such reads are not modelled).
      if (!(w & 2)) garble();
      keyed_ = true;
    }
    if (w & 2) {
      auto it = sys_.memories.find(ram_);
      if (it != sys_.memories.end()) {
        for (uint8_t &b : it->second->data()) {
          b = zero_init_ ? 0 : next_byte();
        }
      }
      set_value(status_, value(status_) | (1u << 5));
    }
  }

 private:
  void garble() {
    auto it = sys_.memories.find(ram_);
    if (it == sys_.memories.end()) return;
    for (uint8_t &b : it->second->data()) b ^= next_byte();
  }
  uint8_t next_byte() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    return uint8_t(lfsr_ | 1);  // never 0: every byte changes
  }

  uint16_t ctrl_, status_, exec_, rotated_;
  std::string inst_, ram_;
  bool zero_init_, instr_exec_, keyed_ = false;
  uint32_t lfsr_ = 0x1234567;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_SRAM_CTRL_H_
