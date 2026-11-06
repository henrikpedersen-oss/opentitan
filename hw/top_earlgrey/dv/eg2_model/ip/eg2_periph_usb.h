// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// usbdev of the EG2 SystemC model (hw/ip/usbdev: data/usbdev.hjson, doc/theory_of_operation.md):
// the bus-side levels, the always-on wake-up and a small USB host. The host can send SETUP or OUT
// transactions to address 0, endpoint 0; there is no enumeration or transmission model.
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
// wake-up) is requested; pwrmgr acts on it at the low-power entry. wake_ack stops the module: it
// "return[s] to an inactive state" (usbdev.hjson wake_control.wake_ack), and its wake-up request
// (wake_detect_active, theory_of_operation.md "Optional external AON/Wake functionality") ends.
// Items in usbdev.model.log, in the format of the chip bench's usbdev.chip.log: "(in) UsbConnect"
// when VBUS rises, and the token, data and handshake packets for each host transaction.
// Debug lines: usbctrl (connect, device address), the endpoint enables, phy_config, the pin
// override (phy_pins_drive), each IN packet software makes ready (configin: endpoint, buffer,
// size; no host collects it), buffers given to the receive engine, VBUS, the pull-ups as the bus
// sees them, the wake module, interrupts raised.
// Not modelled: enumeration, link states, frames, the counters, data-toggle evolution,
// disconnection or host collection of IN packets.

#ifndef EG2_MODEL_IP_EG2_PERIPH_USB_H_
#define EG2_MODEL_IP_EG2_PERIPH_USB_H_

#include <cctype>
#include <cstdint>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

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
    rxfifo_ = reg("rxfifo");
    rxsetup_ = reg("rxenable_setup");
    rxout_ = reg("rxenable_out");
    set_nak_out_ = reg("set_nak_out");
    out_stall_ = reg("out_stall");
    in_stall_ = reg("in_stall");
    fifo_ctrl_ = reg("fifo_ctrl");
    set_modelled(usbstat_);
    set_modelled(sense_);
    set_modelled(wake_ev_);
    set_modelled(rxfifo_);
    vbus_at_ = uint64_t(Eg2PeriphStim::u32("usbdev.vbus_us", 100)) * 1000000ull;
    for (unsigned i = 0; i < kEps; ++i) configin_[i] = reg(("configin_" + std::to_string(i)).c_str());
    avout_ = reg("avoutbuffer");
    avsetup_ = reg("avsetupbuffer");
    ep_out_en_ = reg("ep_out_enable");
    ep_in_en_ = reg("ep_in_enable");
    sys_.stim_handlers["usbdev.host_setup"] = [this](const std::string &v) {
      return host_packet(true, v);
    };
    sys_.stim_handlers["usbdev.host_out"] = [this](const std::string &v) {
      return host_packet(false, v);
    };
    sys_.schedule(this, vbus_at_);
    debug_irqs();
    update_fifos();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    if (c == Eg2ResetCause::kPor) active_ = false;
    avout_fifo_.clear();
    avsetup_fifo_.clear();
    rx_fifo_.clear();
    setup_seen_ = 0;
    update_fifos();
    if (sys_.now_ps() < vbus_at_) sys_.schedule(this, vbus_at_);
    debug_lines();
  }
  void on_time(uint64_t now) override {
    if (now >= vbus_at_) {
      if (!vbus_seen_) {
        vbus_seen_ = true;
        sys_.emit_in(desc().window, "(in) UsbConnect");
        dbg(1, "VBUS present (the host's supply): powered interrupt");
      }
      set_intr("powered");
      debug_lines();
    }
  }

 protected:
  void reg_before_read(uint16_t r) override {
    bool vbus = sys_.now_ps() >= vbus_at_;
    if (r == usbstat_) {
      update_fifos();
    } else if (r == rxfifo_) {
      set_value(rxfifo_, rx_fifo_.empty() ? 0 : rx_fifo_.front());
    } else if (r == sense_) {
      bool dp, dn;
      pullups(dp, dn);
      dp = dp && vbus;
      dn = dn && vbus;
      set_value(sense_, uint32_t(dp) | (uint32_t(dn) << 1) | (uint32_t(dp) << 2) |
                            (uint32_t(vbus) << 16));
    }
  }
  void reg_after_read(uint16_t r) override {
    if (r != rxfifo_ || rx_fifo_.empty()) return;
    uint32_t e = rx_fifo_.front();
    rx_fifo_.pop_front();
    sys_.emit_in(desc().window,
                 "< RXFIFO ep " + std::to_string((e >> 20) & 0xf) +
                     ((e >> 19) & 1 ? " SETUP size " : " OUT size ") +
                     std::to_string((e >> 8) & 0x7f) + " buffer " +
                     std::to_string(e & 0x1f));
    update_fifos();
  }
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    debug_write(r, old, wdata & wmask);
    uint32_t w = wdata & wmask;
    if (r == avout_ || r == avsetup_) {
      // usbdev.hjson av*out/setup*buffer: "If the Available ... Buffer FIFO is full, any write
      // operations are discarded." The interrupt description says a full-FIFO write raises
      // av_overflow.
      auto &q = r == avout_ ? avout_fifo_ : avsetup_fifo_;
      size_t limit = r == avout_ ? kAvOutDepth : kAvSetupDepth;
      if (q.size() < limit) q.push_back(uint8_t(w & 0x1f));
      else set_intr("av_overflow");
      update_fifos();
      debug_lines();
      return;
    }
    if (r == fifo_ctrl_) {
      // usbdev.hjson fifo_ctrl: each bit is the "Software reset" of its named FIFO.
      if (w & 1) avout_fifo_.clear();
      if (w & 2) avsetup_fifo_.clear();
      if (w & 4) rx_fifo_.clear();
      update_fifos();
      debug_lines();
      return;
    }
    if (r != wake_ctl_) {
      update_fifos();
      debug_lines();
      return;
    }
    if (w & 1) {  // suspend_req: the wake module starts, holding the pull-ups
      pullups(held_dp_, held_dn_);
      active_ = true;
      uint32_t ev = 1;
      if ((value(drive_) >> 16) & 1) {
        ev |= 1u << 10;  // bus_not_idle once the chip's drive goes at low-power entry
        eg2_request_wakeup(sys_, 3);
      }
      set_value(wake_ev_, ev);
      dbgf(1, "wake_control.suspend_req: the always-on wake module holds the pull-ups (D+ %u, "
              "D- %u)%s", held_dp_, held_dn_,
           (ev >> 10) & 1 ? "; the pin override drives the bus, so it will see it leave idle at "
                            "low-power entry: bus_not_idle, wake-up requested"
                          : "");
    }
    if (w & 2) {  // wake_ack
      active_ = false;
      set_value(wake_ev_, 0);
      dbg(1, "wake_control.wake_ack: the wake module stops, its wake-up request ends");
      if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))) pm->wakeup_clear(3);
    }
    debug_lines();
  }

 private:
  static constexpr unsigned kEps = 12;
  static constexpr size_t kAvOutDepth = 8, kAvSetupDepth = 4, kRxDepth = 8;

  static bool parse_bytes(const std::string &text, std::vector<uint8_t> *bytes) {
    std::string hex;
    for (char c : text) {
      if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
      else if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    if (hex.size() & 1) return false;
    bytes->clear();
    for (size_t i = 0; i < hex.size(); i += 2)
      bytes->push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes->size() <= 64;
  }

  bool connected() const {
    bool dp, dn;
    pullups(dp, dn);
    // Programmer's Guide "Initialization": setting usbctrl.enable "causes the USB device to
    // assert the pullup"; control transfers initially use device ID 0. A transaction therefore
    // reaches this host model only with the interface enabled, a pull-up visible and VBUS present.
    return (value(usbctrl_) & 1) && (dp || dn) && sys_.now_ps() >= vbus_at_;
  }

  bool host_packet(bool setup, const std::string &text) {
    std::vector<uint8_t> data;
    if (!parse_bytes(text, &data)) return false;
    constexpr unsigned address = 0, endpoint = 0;
    log_host_packet(setup, address, endpoint, data);

    // Programmer's Guide "Initialization": initial control transfers "will be sent to device ID
    // 0". usbctrl.device_address is the address accepted after enumeration.
    if (!connected() || address != ((value(usbctrl_) >> 16) & 0x7f)) {
      dbg(1, "host transaction ignored: the device is not connected at address 0");
      return true;
    }
    // usbdev.hjson ep_out_enable: "If the bit is clear, any SETUP or OUT packets sent to the
    // endpoint will be ignored."
    if (!(value(ep_out_en_) & (1u << endpoint))) {
      dbg(1, "host transaction ignored: OUT endpoint 0 is disabled");
      return true;
    }

    if (setup) {
      // usbdev.hjson rxenable_setup: "If the bit is clear then a SETUP packet will be ignored."
      if (!(value(rxsetup_) & (1u << endpoint))) return true;
      // theory_of_operation.md "Reception": with the appropriate Available Buffer FIFO empty or
      // the Received Buffer FIFO full, "SETUP transactions are ignored."
      if (avsetup_fifo_.empty() || rx_fifo_.size() >= kRxDepth) return true;
      receive(true, endpoint, data, avsetup_fifo_);
      setup_seen_ |= 1u << endpoint;

      // Programmer's Guide "Stalling": "Receiving the SETUP token clears the in_stall and
      // out_stall registers for that endpoint."
      set_value(out_stall_, value(out_stall_) & ~(1u << endpoint));
      set_value(in_stall_, value(in_stall_) & ~(1u << endpoint));
      // Programmer's Guide "Transmission": an accepted SETUP clears rdy and sets pend for a
      // pending transmission on that endpoint.
      uint32_t ci = value(configin_[endpoint]);
      if (ci & (1u << 31))
        set_value(configin_[endpoint],
                  (ci & ~((1u << 29) | (1u << 31))) | (1u << 30));
      // Programmer's Guide "Stalling": set_nak_out or a cleared rxenable_out makes following
      // IN/OUT requests receive NAKs until software decides how to handle the new SETUP request.
      // usbdev.hjson out_data_toggle only says that reading returns the current flags and writing
      // sets masked flags. The IP documents do not say that SETUP changes a toggle, so it does not
      // change one here; the documentation gap is recorded in the report.
      return true;
    }

    // theory_of_operation.md "Reception": clearing rxenable_out NAKs the endpoint. The
    // Programmer's Guide adds that set_nak_out NAKs requests following a new SETUP.
    if (!(value(rxout_) & (1u << endpoint)) ||
        ((setup_seen_ & (1u << endpoint)) && (value(set_nak_out_) & (1u << endpoint)))) {
      sys_.emit(desc().window, "packet chip->host NAK");
      return true;
    }
    // theory_of_operation.md "Reception": no OUT buffer or no Received Buffer FIFO room NAKs
    // OUT transactions.
    if (avout_fifo_.empty() || rx_fifo_.size() + 1 >= kRxDepth) {
      sys_.emit(desc().window, "packet chip->host NAK");
      return true;
    }
    receive(false, endpoint, data, avout_fifo_);
    // theory_of_operation.md "Reception": set_nak_out "will clear its corresponding bit in the
    // rxenable_out register" after an OUT transaction, forcing later OUT requests to be NAKed.
    if (value(set_nak_out_) & (1u << endpoint))
      set_value(rxout_, value(rxout_) & ~(1u << endpoint));
    return true;
  }

  void receive(bool setup, unsigned endpoint, const std::vector<uint8_t> &data,
               std::deque<uint8_t> &available) {
    // theory_of_operation.md "Reception": "the next buffer ID is pulled from the appropriate
    // Available Buffer FIFO" and packet data is written to that buffer in the 2 kB SRAM.
    uint8_t buffer = available.front();
    available.pop_front();
    Eg2RegBlock::win_write("buffer", 64u * buffer, data.data(), data.size());
    // The same paragraph says buffer ID, packet size, OUT/SETUP and endpoint are passed through
    // the Received Buffer FIFO and pkt_received is raised.
    rx_fifo_.push_back(buffer | (uint32_t(data.size()) << 8) | (uint32_t(setup) << 19) |
                       (endpoint << 20));
    sys_.emit(desc().window, "packet chip->host ACK");
    update_fifos();
  }

  void log_host_packet(bool setup, unsigned address, unsigned endpoint,
                       const std::vector<uint8_t> &data) {
    std::ostringstream token, payload;
    token << "packet host->chip " << (setup ? "SETUP" : "OUT") << " addr " << address << " ep "
          << endpoint;
    payload << "packet host->chip DATA0 " << data.size() << " bytes";
    static constexpr char kHex[] = "0123456789abcdef";
    for (uint8_t b : data) payload << ' ' << kHex[b >> 4] << kHex[b & 15];
    sys_.emit_in(desc().window, token.str());
    sys_.emit_in(desc().window, payload.str());
  }

  void update_fifos() {
    bool vbus = sys_.now_ps() >= vbus_at_;
    uint32_t st = value(usbstat_) & 0x0000ffffu;
    st = (st & ~(1u << 15)) | (uint32_t(vbus) << 15);
    st |= uint32_t(avout_fifo_.size()) << 16;
    st |= uint32_t(avsetup_fifo_.size()) << 20;
    if (avout_fifo_.size() == kAvOutDepth) st |= 1u << 23;
    st |= uint32_t(rx_fifo_.size()) << 24;
    if (avsetup_fifo_.size() == kAvSetupDepth) st |= 1u << 30;
    if (rx_fifo_.empty()) st |= 1u << 31;
    set_value(usbstat_, st);
    // usbdev.hjson: these status interrupts are directly tied to their FIFO conditions.
    bool enabled = value(usbctrl_) & 1;
    set_intr_level("pkt_received", !rx_fifo_.empty());
    set_intr_level("av_out_empty", enabled && avout_fifo_.empty());
    set_intr_level("av_setup_empty", enabled && avsetup_fifo_.empty());
    set_intr_level("rx_full", enabled && rx_fifo_.size() == kRxDepth);
  }
  void debug_write(uint16_t r, uint32_t old, uint32_t w) {
    if (!dbg_on(1)) return;
    uint32_t v = value(r);
    if (r == usbctrl_) {
      if ((v ^ old) & 1) dbgf(1, "usbctrl.enable %u: %s", v & 1,
                              (v & 1) ? "connect (pull-up on)" : "disconnect (pull-up off)");
      if ((v ^ old) & (0x7fu << 16)) dbgf(1, "usbctrl.device_address %u", (v >> 16) & 0x7f);
      if (w & 2) dbg(1, "usbctrl.resume_link_active (no link state modelled)");
    } else if ((r == ep_out_en_ || r == ep_in_en_) && v != old) {
      dbgf(1, "%s: endpoints 0x%03x", r == ep_out_en_ ? "ep_out_enable" : "ep_in_enable", v);
    } else if (r == config_ && v != old) {
      dbgf(1, "phy_config 0x%08x (pinflip %u)", v, (v >> 5) & 1);
    } else if (r == drive_ && v != old) {
      dbgf(1, "phy_pins_drive: %s (D+ pull-up %u, D- pull-up %u, oe %u)",
           (v >> 16) & 1 ? "software drives the pins" : "the hardware drives the pins",
           (v >> 6) & 1, (v >> 7) & 1, (v >> 4) & 1);
    } else if (r == avout_ || r == avsetup_) {
      if (dbg_count("buffers given to the receive engine", 16))
        dbgf(1, "%s: buffer %u given to the receive engine",
             r == avout_ ? "avoutbuffer" : "avsetupbuffer", w & 0x1f);
    } else {
      for (unsigned i = 0; i < kEps; ++i) {
        if (r == configin_[i] && ((v >> 31) & 1) && !((old >> 31) & 1)) {
          dbgf(1, "configin_%u: IN packet of %u bytes ready on endpoint %u in buffer %u; no USB "
                  "host in eg2sim collects it", i, (v >> 8) & 0x7f, i, v & 0x1f);
        }
      }
    }
  }
  // The levels the bus sees on D+ and D- (the pull-ups with VBUS), as a debug line when they change.
  void debug_lines() {
    bool dp, dn, vbus = sys_.now_ps() >= vbus_at_;
    pullups(dp, dn);
    unsigned l = unsigned(dp && vbus) | (unsigned(dn && vbus) << 1);
    if (l == lines_) return;
    lines_ = l;
    dbgf(1, "bus: D+ %s, D- %s%s", (l & 1) ? "pulled up" : "low", (l & 2) ? "pulled up" : "low",
         l ? " (a host would see the device connect)" : "");
  }
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
  uint16_t usbctrl_, usbstat_, sense_, drive_, config_, wake_ctl_, wake_ev_, rxfifo_, rxsetup_,
      rxout_, set_nak_out_, out_stall_, in_stall_, fifo_ctrl_;
  uint16_t configin_[kEps], avout_, avsetup_, ep_out_en_, ep_in_en_;
  std::deque<uint8_t> avout_fifo_, avsetup_fifo_;
  std::deque<uint32_t> rx_fifo_;
  uint16_t setup_seen_ = 0;
  bool active_ = false, held_dp_ = false, held_dn_ = false, vbus_seen_ = false;
  uint64_t vbus_at_ = 0;
  unsigned lines_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_USB_H_
