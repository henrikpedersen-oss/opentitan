// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// usbdev of the EG2 SystemC model (hw/ip/usbdev: data/usbdev.hjson, doc/theory_of_operation.md):
// the bus-side levels and the always-on wake-up, without a USB host (no packets). The packet buffer
// window and the endpoint registers are plain registers and memory.
//
// VBUS: "in simulation the DPI model connects VBUS shortly after reset" (usbdev_vbus_test.c): the
// sense input rises --stim usbdev.vbus_us=<microseconds> after the start (default 100), raising the
// powered interrupt; usbstat.sense and phy_pins_sense.pwr_sense read it.
// Line levels (phy_pins_sense.rx_dp_i, rx_dn_i): no host drives the bus, so a line is high exactly
// when its pull-up is on: with phy_pins_drive.en the override's dp_pullup_en_o and dn_pullup_en_o,
// otherwise the D+ pull-up while usbctrl.enable (D- with phy_config.pinflip), and only with VBUS.
// rx_d_i follows rx_dp_i.
// Wake-up: a write of wake_control.suspend_req starts the always-on wake module
// (wake_events.module_active), which holds the pull-ups as they were ("the AON module can hold" the
// line, usbdev_aon_pullup_test.c): while it is active the lines keep those levels whatever the
// interface does. It reports a wake-up when the bus leaves the idle state it saw; with
// no host, the bus changes only when the chip's own drive goes away at low-power entry: if the
// override drives a line (phy_pins_drive.en), bus_not_idle is set and wake-up 3 (pinmux's USB
// wake-up) is requested; pwrmgr acts on it at the low-power entry. wake_ack stops the module.
// Not modelled: packets, link states, frames, the counters, the data toggles, disconnection.

#ifndef EG2_MODEL_IP_EG2_PERIPH_USB_H_
#define EG2_MODEL_IP_EG2_PERIPH_USB_H_

#include <cstdint>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_periph_stim.h"
#include "eg2_periph_sys.h"

namespace eg2_model {

class Eg2Usbdev : public Eg2IpModel {
 public:
  Eg2Usbdev(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    usbctrl_ = reg("usbctrl");
    usbstat_ = reg("usbstat");
    sense_ = reg("phy_pins_sense");
    drive_ = reg("phy_pins_drive");
    config_ = reg("phy_config");
    wake_ctl_ = reg("wake_control");
    wake_ev_ = reg("wake_events");
    set_modelled(usbstat_);
    set_modelled(sense_);
    set_modelled(wake_ev_);
    vbus_at_ = uint64_t(Eg2PeriphStim::u32("usbdev.vbus_us", 100)) * 1000000ull;
    sys_.schedule(this, vbus_at_);
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    if (c == Eg2ResetCause::kPor) active_ = false;
    if (sys_.now_ps() < vbus_at_) sys_.schedule(this, vbus_at_);
  }
  void on_time(uint64_t now) override {
    if (now >= vbus_at_) set_intr("powered");
  }

 protected:
  void reg_before_read(uint16_t r) override {
    bool vbus = sys_.now_ps() >= vbus_at_;
    if (r == usbstat_) {
      set_value(usbstat_, (value(usbstat_) & ~(1u << 15)) | (uint32_t(vbus) << 15));
    } else if (r == sense_) {
      bool dp, dn;
      pullups(dp, dn);
      dp = dp && vbus;
      dn = dn && vbus;
      set_value(sense_, uint32_t(dp) | (uint32_t(dn) << 1) | (uint32_t(dp) << 2) |
                            (uint32_t(vbus) << 16));
    }
  }
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r != wake_ctl_) return;
    uint32_t w = wdata & wmask;
    if (w & 1) {  // suspend_req: the wake module starts, holding the pull-ups
      pullups(held_dp_, held_dn_);
      active_ = true;
      uint32_t ev = 1;
      if ((value(drive_) >> 16) & 1) {
        ev |= 1u << 10;  // bus_not_idle once the chip's drive goes at low-power entry
        eg2_request_wakeup(sys_, 3);
      }
      set_value(wake_ev_, ev);
    }
    if (w & 2) {  // wake_ack
      active_ = false;
      set_value(wake_ev_, 0);
    }
  }

 private:
  // The pull-ups now: the wake module's held ones, the override's, or the interface's.
  void pullups(bool &dp, bool &dn) const {
    uint32_t dr = value(drive_);
    if (active_) {
      dp = held_dp_;
      dn = held_dn_;
    } else if ((dr >> 16) & 1) {
      dp = (dr >> 6) & 1;
      dn = (dr >> 7) & 1;
    } else {
      bool on = value(usbctrl_) & 1, flip = (value(config_) >> 5) & 1;
      dp = on && !flip;
      dn = on && flip;
    }
  }
  uint16_t usbctrl_, usbstat_, sense_, drive_, config_, wake_ctl_, wake_ev_;
  bool active_ = false, held_dp_ = false, held_dn_ = false;
  uint64_t vbus_at_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_USB_H_
