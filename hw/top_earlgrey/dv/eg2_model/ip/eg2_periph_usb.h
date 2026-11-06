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
// With the host (--stim usbdev.host=1, usb-host/): VBUS is the host's; the link states, link
// events and the wake module's bus events follow the line the host leaves (usb_bus).
// Not modelled: the counters.

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
#include "../usb-host/usbdpi.h"

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
    in_sent_ = reg("in_sent");
    out_tog_ = reg("out_data_toggle");
    in_tog_ = reg("in_data_toggle");
    out_iso_ = reg("out_iso");
    in_iso_ = reg("in_iso");
    set_modelled(out_tog_);
    set_modelled(in_tog_);
    if (Eg2PeriphStim::u32("usbdev.host", 0)) host_start();
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
    if (c == Eg2ResetCause::kPor) {
      active_ = false;
      aon_ev_ = 0;
    }
    avout_fifo_.clear();
    avsetup_fifo_.clear();
    rx_fifo_.clear();
    se0_bits_ = bus_idle_ = since_sof_ = 0;
    update_fifos();
    if (sys_.now_ps() < vbus_at_) sys_.schedule(this, vbus_at_);
    debug_lines();
  }
  void on_time(uint64_t now) override {
    // With the host, VBUS is the host's (usb_bus).
    if (!host_ && now >= vbus_at_) {
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
    bool vbus = vbus_on();
    if (r == usbstat_) {
      update_fifos();
    } else if (r == wake_ev_) {
      // The always-on wake module's state: it keeps it through the main domain's resets.
      set_value(wake_ev_, active_ ? aon_ev_ : 0);
    } else if (r == out_tog_ || r == in_tog_) {
      set_value(r, r == out_tog_ ? out_toggle_ : in_toggle_);
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
    if (r == out_tog_ || r == in_tog_) {
      // usbdev.hjson out/in_data_toggle: "Writing sets the Data Toggle flag for each endpoint if
      // the corresponding mask bit in the upper half of this register is set."
      uint16_t &t = r == out_tog_ ? out_toggle_ : in_toggle_;
      uint16_t mask = (w >> 16) & 0xfff;
      t = uint16_t((t & ~mask) | (w & mask));
      set_value(r, t);
      return;
    }
    if (r == in_sent_) {
      update_fifos();
      return;
    }
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
    if (r == usbctrl_ && (w & 2) && link() == kPowered) {
      // usbctrl.resume_link_active: a software-directed jump to resume from a suspended link, for a
      // device powered down while suspended (hjson; doc/wake_resume.md); the end of resume
      // signaling (J) then makes the link active (usb_bus).
      set_link(kResuming);
    }
    if (r != wake_ctl_) {
      update_fifos();
      debug_lines();
      return;
    }
    if (w & 1) {  // suspend_req: the wake module starts, holding the pull-ups
      pullups(held_dp_, held_dn_);
      active_ = true;
      aon_nonj_ = aon_se0_ = 0;
      uint32_t ev = 1;
      if ((value(drive_) >> 16) & 1) {
        ev |= 1u << 10;  // bus_not_idle once the chip's drive goes at low-power entry
        eg2_request_wakeup(sys_, 3);
      }
      aon_ev_ = ev;
      set_value(wake_ev_, ev);
      dbgf(1, "wake_control.suspend_req: the always-on wake module holds the pull-ups (D+ %u, "
              "D- %u)%s", held_dp_, held_dn_,
           (ev >> 10) & 1 ? "; the pin override drives the bus, so it will see it leave idle at "
                            "low-power entry: bus_not_idle, wake-up requested"
                          : "");
    }
    if (w & 2) {  // wake_ack
      active_ = false;
      aon_ev_ = 0;
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
    return (value(usbctrl_) & 1) && (dp || dn) && vbus_on();
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
      if (value(set_nak_out_) & (1u << endpoint))
        set_value(rxout_, value(rxout_) & ~(1u << endpoint));

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
    if (!(value(rxout_) & (1u << endpoint))) {
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
               std::deque<uint8_t> &available, bool log_ack = true) {
    // theory_of_operation.md "Reception": "the next buffer ID is pulled from the appropriate
    // Available Buffer FIFO" and packet data is written to that buffer in the 2 kB SRAM.
    uint8_t buffer = available.front();
    available.pop_front();
    Eg2RegBlock::win_write("buffer", 64u * buffer, data.data(), data.size());
    // The same paragraph says buffer ID, packet size, OUT/SETUP and endpoint are passed through
    // the Received Buffer FIFO and pkt_received is raised.
    rx_fifo_.push_back(buffer | (uint32_t(data.size()) << 8) | (uint32_t(setup) << 19) |
                       (endpoint << 20));
    if (log_ack) sys_.emit(desc().window, "packet chip->host ACK");
    update_fifos();
  }

  void log_host_packet(bool setup, unsigned address, unsigned endpoint,
                       const std::vector<uint8_t> &data, bool data1 = false, bool token_line = true) {
    std::ostringstream token, payload;
    token << "packet host->chip " << (setup ? "SETUP" : "OUT") << " addr " << address << " ep "
          << endpoint;
    payload << "packet host->chip " << (data1 ? "DATA1 " : "DATA0 ") << data.size() << " bytes";
    static constexpr char kHex[] = "0123456789abcdef";
    for (uint8_t b : data) payload << ' ' << kHex[b >> 4] << kHex[b & 15];
    if (token_line) sys_.emit_in(desc().window, token.str());
    sys_.emit_in(desc().window, payload.str());
  }

  void update_fifos() {
    bool vbus = vbus_on();
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
    // usbdev.hjson in_sent: set on the host's ACK "causing a pkt_sent interrupt to be raised"
    // (theory_of_operation.md "Transmission"); it stays while any bit is set.
    set_intr_level("pkt_sent", (value(in_sent_) & 0xfff) != 0);
  }
  void debug_write(uint16_t r, uint32_t old, uint32_t w) {
    if (!dbg_on(1)) return;
    uint32_t v = value(r);
    if (r == usbctrl_) {
      if ((v ^ old) & 1) dbgf(1, "usbctrl.enable %u: %s", v & 1,
                              (v & 1) ? "connect (pull-up on)" : "disconnect (pull-up off)");
      if ((v ^ old) & (0x7fu << 16)) dbgf(1, "usbctrl.device_address %u", (v >> 16) & 0x7f);
      if (w & 2) dbg(1, "usbctrl.resume_link_active");
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
    bool dp, dn, vbus = vbus_on();
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
  // ---- The packet engine: one USB packet from the host (PID, fields, CRC) in, the device's reply
  // (PID and fields, CRC16 for data) out, empty for none. theory_of_operation.md "USB Protocol
  // Engine": IN, OUT and SETUP transactions; where the IP documents say nothing about the data
  // toggles, the USB 2.0 specification (8.5.3, 8.6.4) decides.
  static constexpr uint32_t kRdy = 1u << 31, kPend = 1u << 30, kSending = 1u << 29;
  // usbstat.link_state (theory_of_operation.md "USB Link State").
  enum LinkState : uint32_t {
    kDisconnected = 0,
    kPowered = 1,
    kPoweredSuspended = 2,
    kActive = 3,
    kSuspended = 4,
    kActiveNoSof = 5,
    kResuming = 6
  };
  uint32_t link() const { return (value(usbstat_) >> 12) & 7; }
  bool bit(uint16_t r, unsigned ep) const { return (value(r) >> ep) & 1; }
  // in_iso/out_iso: "if a rxenable_setup is set for this endpoint's number, this bit will not take
  // effect. Control endpoint configuration trumps isochronous endpoint configuration."
  bool iso(uint16_t r, unsigned ep) const { return bit(r, ep) && !bit(rxsetup_, ep); }
  void set_link(uint32_t state) {
    static const char *const kNames[] = {"Disconnected", "Powered", "Powered Suspended", "Active",
                                         "Suspended", "Active No SOF", "Resuming", "?"};
    if (state != link()) dbgf(1, "link state: %s -> %s", kNames[link()], kNames[state & 7]);
    // "Disconnected" (theory_of_operation.md "USB Link State"): "An interrupt is raised on entering
    // this state", whether VBUS went or the interface was disabled. (usbdev.hjson's interrupt
    // describes only the VBUS loss; see status/dv_tickets.md.)
    if (state == kDisconnected && link() != kDisconnected) set_intr("disconnected");
    set_value(usbstat_, (value(usbstat_) & ~(7u << 12)) | (state << 12));
    if (state == kResuming) resume_se0_ = false;
  }
  // VBUS: the host's (usbdpi's P2D_SENSE) when the host runs, otherwise the stimulus time.
  bool vbus_on() const {
    return host_ ? (host_->driving & P2D_SENSE) != 0 : sys_.now_ps() >= vbus_at_;
  }
  // The wake module reports an event (wake_events) and requests the wake-up
  // (wake_detect_active, theory_of_operation.md "Optional external AON/Wake functionality"; pinmux's
  // USB wake-up, source 3).
  void aon_event(unsigned bit, const char *what) {
    if ((aon_ev_ >> bit) & 1) return;
    aon_ev_ |= 1u << bit;
    dbgf(1, "wake module: %s (wake_events bit %u), wake-up requested", what, bit);
    eg2_request_wakeup(sys_, 3);
  }
  // ---- The bus as usbdev's link FSM and the wake module see it, one full-speed bit (83.3 ns) at a
  // time, while the host runs. Rules: theory_of_operation.md "USB Link State" (states and link
  // events), "Optional external AON/Wake functionality" and "AON/Wake Clocking".
  static constexpr uint32_t kResetBits = 36;        // "in SE0 for 3 us"
  static constexpr uint32_t kSuspendBits = 36000;   // "in the J state for more than 3 ms"
  static constexpr uint32_t kHostLostBits = 49152;  // usbstat.host_lost: "for 4.096 ms"
  // The wake module runs at 200 kHz ("AON/Wake Clocking"): it sees a bus state that lasts one of
  // its clocks (5 us), so not the low-speed EOP that ends resume signaling ("will respond to Low
  // Speed EOP events ... as if they are Bus Reset signaling" only at 1 MHz or more).
  static constexpr uint32_t kAonBits = 60;
  void usb_bus() {
    bool vbus = vbus_on();
    if (vbus != vbus_prev_) {
      vbus_prev_ = vbus;
      if (vbus) {
        // usbdev.hjson interrupt powered: "Raised if VBUS is applied."
        sys_.emit_in(desc().window, "(in) UsbConnect");
        dbg(1, "VBUS present (the host's supply): powered interrupt");
        set_intr("powered");
      } else {
        // Link event "Disconnect": "VBUS has been lost"; interrupt disconnected.
        dbg(1, "VBUS lost: disconnected interrupt");
        set_intr("disconnected");
      }
      debug_lines();
    }
    if (!vbus) {
      // The wake module's usb_aon_sense_lost: "the VBUS/SENSE signal became deasserted".
      if (active_) aon_event(8, "VBUS lost (disconnected)");
      // "Disconnected": "VBUS is not driven by the host". Without VBUS there is no signaling.
      set_link(kDisconnected);
      bus_idle_ = se0_bits_ = aon_se0_ = aon_nonj_ = 0;
      return;
    }
    // The line: the host's drive, otherwise the pull-ups. J is the pulled-up line high, the other
    // low (D- with phy_config.pinflip); SE0 is both low.
    bool pdp, pdn;
    pullups(pdp, pdn);
    uint32_t dr = host_->driving;
    bool oe = dr & P2D_OE;
    bool dp = oe ? (dr & P2D_DP) != 0 : pdp, dn = oe ? (dr & P2D_DN) != 0 : pdn;
    bool se0 = !dp && !dn;
    bool j = (pdn && !pdp) ? (!dp && dn) : (dp && !dn);
    if (active_) {
      // "Resume Signaling": usb_aon_bus_not_idle, "switched to a 'non-idle' state"; "Bus Reset":
      // usb_aon_bus_reset (SE0).
      aon_nonj_ = j ? 0 : aon_nonj_ + 1;
      aon_se0_ = se0 ? aon_se0_ + 1 : 0;
      if (aon_nonj_ == kAonBits) aon_event(10, "bus not idle");
      if (aon_se0_ == kAonBits) aon_event(9, "bus reset (SE0)");
    }
    // "Disconnected": also "when the user has not connected the pull-up by enabling the interface"
    // (usbctrl.enable).
    if (!(value(usbctrl_) & 1)) {
      set_link(kDisconnected);
      bus_idle_ = se0_bits_ = 0;
      return;
    }
    // "Powered": VBUS driven and the pull-up connected, not reset yet.
    if (link() == kDisconnected) set_link(kPowered);
    se0_bits_ = se0 ? se0_bits_ + 1 : 0;
    bus_idle_ = j ? bus_idle_ + 1 : 0;
    // "Link Reset": "in the SE0 state for 3 us".
    if (se0_bits_ == kResetBits) {
      usb_link_reset();
      return;
    }
    uint32_t ls = link();
    if (!j) {
      // "Link Resume": "driven to a non-J state after being in Suspend"; from Powered Suspended
      // the link returns to Powered with the resume interrupt; from Suspended it goes to Resuming,
      // "awaiting the end of resume signaling".
      if (ls == kSuspended) {
        set_link(kResuming);
      } else if (ls == kPoweredSuspended) {
        set_link(kPowered);
        set_intr("link_resume");
      }
      if (link() == kResuming && se0) resume_se0_ = true;
    } else if (ls == kResuming && resume_se0_) {
      // USB 2.0 7.1.7.7: resume signaling ends with a low-speed EOP (SE0, then J); "For the case
      // of resuming to active link states, the end of resume signaling has occurred": the resume
      // interrupt, and "Resuming" goes to "Active No SOF".
      set_link(kActiveNoSof);
      set_intr("link_resume");
    }
    // "Link Suspend": "in the J state for more than 3 ms": Powered Suspended before a reset,
    // Suspended after it, with the suspend interrupt.
    if (bus_idle_ == kSuspendBits) {
      if (ls == kPowered) {
        set_link(kPoweredSuspended);
        set_intr("link_suspend");
      } else if (ls == kActive || ls == kActiveNoSof) {
        set_link(kSuspended);
        set_intr("link_suspend");
      }
    }
    // "Host Lost": "the link is active but a start of frame (SOF) packet has not been received
    // from the host in 4 frames" (usbstat.host_lost, interrupt host_lost).
    if (link() == kActive && ++since_sof_ == kHostLostBits) {
      set_value(usbstat_, value(usbstat_) | (1u << 11));
      set_intr("host_lost");
    }
  }

 public:
  std::vector<uint8_t> usb_packet(const uint8_t *p, size_t n) {
    bus_idle_ = 0;  // a packet: the bus is not idle (usb_bus)
    if (n == 0 || !connected()) return {};
    // A packet is bus activity ending in an EOP: it leaves a suspend state ("Link Resume": "driven
    // to a non-J state after being in Suspend") and ends Resuming ("awaiting the end of resume
    // signaling"), theory_of_operation.md "USB Link State".
    if (link() == kSuspended || link() == kResuming) {
      set_link(kActiveNoSof);
      set_intr("link_resume");
    } else if (link() == kPoweredSuspended) {
      set_link(kPowered);
      set_intr("link_resume");
    }
    uint8_t pid = p[0];
    if (pid == USB_PID_SOF) {
      // usbstat.frame: the frame number of the last SOF; the link is active once SOFs arrive
      // (theory_of_operation.md "USB Link State"); the frame interrupt on each. usbstat.host_lost:
      // "Start of frame not received from host for 4.096 ms": cleared by a SOF.
      if (n >= 3) {
        set_value(usbstat_, (value(usbstat_) & ~0x7ffu) | ((p[1] | (uint32_t(p[2]) << 8)) & 0x7ff));
        if (link() == kActiveNoSof || link() == kActive) set_link(kActive);
        set_intr("frame");
        since_sof_ = 0;
        set_value(usbstat_, value(usbstat_) & ~(1u << 11));
      }
      return {};
    }
    if (pid == USB_PID_SETUP || pid == USB_PID_OUT || pid == USB_PID_IN) {
      tok_valid_ = false;
      if (n < 3) return {};
      unsigned addr = p[1] & 0x7f, ep = ((p[1] >> 7) | (uint32_t(p[2]) << 1)) & 0xf;
      // usbctrl.device_address: the device answers only at its address; endpoints beyond the
      // implemented ones get no reply.
      if (addr != ((value(usbctrl_) >> 16) & 0x7f) || ep >= kEps) return {};
      log_token(pid, addr, ep);
      if (pid == USB_PID_IN) return in_token(ep);
      tok_valid_ = true;
      tok_setup_ = pid == USB_PID_SETUP;
      tok_ep_ = ep;
      return {};
    }
    if (pid == USB_PID_DATA0 || pid == USB_PID_DATA1) {
      if (!tok_valid_) return {};
      tok_valid_ = false;
      std::vector<uint8_t> data(p + 1, p + (n >= 3 ? n - 2 : 1));  // without the CRC16
      return out_data(tok_setup_, tok_ep_, pid == USB_PID_DATA1, data);
    }
    if (pid == USB_PID_ACK) in_acked();
    return {};
  }
  // The host held the bus in SE0 (usbdpi's bus reset): a Link Reset.
  void usb_link_reset() {
    bool dp, dn;
    pullups(dp, dn);
    if (!vbus_on() || !(dp || dn)) return;
    // usbctrl.device_address "will be zeroed by the hardware when the link resets".
    set_value(usbctrl_, value(usbctrl_) & ~(0x7fu << 16));
    // theory_of_operation.md "Transmission": "a Link Reset cancels any waiting IN transactions by
    // clearing the rdy bit ... The pend bit ... is set for all endpoints with a pending IN".
    for (unsigned ep = 0; ep < kEps; ++ep) {
      uint32_t ci = value(configin_[ep]);
      if (ci & kRdy) set_value(configin_[ep], (ci & ~(kRdy | kSending)) | kPend);
    }
    // USB 2.0 9.1.1.3: after a reset the device is in the Default state; its data toggles
    // start from DATA0.
    out_toggle_ = in_toggle_ = 0;
    in_pending_ = -1;
    tok_valid_ = false;
    set_link(kActiveNoSof);
    set_intr("link_reset");
    sys_.emit(desc().window, "(in) UsbLinkReset");
  }

 private:
  void log_token(uint8_t pid, unsigned addr, unsigned ep) {
    const char *n = pid == USB_PID_SETUP ? "SETUP" : pid == USB_PID_OUT ? "OUT" : "IN";
    sys_.emit_in(desc().window, std::string("packet host->chip ") + n + " addr " +
                                    std::to_string(addr) + " ep " + std::to_string(ep));
  }
  std::vector<uint8_t> reply(uint8_t pid) {
    sys_.emit(desc().window, std::string("packet chip->host ") +
                                 (pid == USB_PID_ACK ? "ACK" : pid == USB_PID_NAK ? "NAK" : "STALL"));
    return {pid};
  }
  std::vector<uint8_t> out_data(bool setup, unsigned ep, bool data1,
                                const std::vector<uint8_t> &data) {
    log_host_packet(setup, (value(usbctrl_) >> 16) & 0x7f, ep, data, data1, false);
    // ep_out_enable: "If the bit is clear, any SETUP or OUT packets sent to the endpoint will be
    // ignored."
    if (!bit(ep_out_en_, ep)) return {};
    if (setup) {
      // rxenable_setup clear, or no SETUP buffer or no Received Buffer FIFO room: "SETUP
      // transactions are ignored" (theory_of_operation.md "Reception").
      if (!bit(rxsetup_, ep) || avsetup_fifo_.empty() || rx_fifo_.size() >= kRxDepth) return {};
      receive(true, ep, data, avsetup_fifo_, false);
      // set_nak_out: hardware clears rxenable_out on the transaction, so later OUTs are NAKed "until
      // the software has decided what action to take for the new SETUP request" (Programmer's Guide
      // "Stalling").
      if (bit(set_nak_out_, ep)) set_value(rxout_, value(rxout_) & ~(1u << ep));
      // Programmer's Guide "Stalling": "Receiving the SETUP token clears the in_stall and out_stall
      // registers for that endpoint."
      set_value(out_stall_, value(out_stall_) & ~(1u << ep));
      set_value(in_stall_, value(in_stall_) & ~(1u << ep));
      // theory_of_operation.md "Transmission": on a SETUP, waiting IN buffers are canceled (rdy
      // cleared), pend set; configin.sending is cleared by "receipt of a SETUP packet".
      uint32_t ci = value(configin_[ep]);
      if (ci & kRdy) set_value(configin_[ep], (ci & ~(kRdy | kSending)) | kPend);
      if (in_pending_ == int(ep)) in_pending_ = -1;
      // USB 2.0 8.5.3: the data and status stages after a SETUP start with DATA1, both ways.
      out_toggle_ |= uint16_t(1u << ep);
      in_toggle_ |= uint16_t(1u << ep);
      return reply(USB_PID_ACK);
    }
    bool is_iso = iso(out_iso_, ep);
    // out_iso: "No handshake packet will be sent for an OUT transaction."
    if (!is_iso && bit(out_stall_, ep)) return reply(USB_PID_STALL);
    // rxenable_out: "If the bit is clear but the endpoint is enabled, then an OUT request will
    // receive a NAK"; no OUT buffer or Received Buffer FIFO room NAKs too (theory_of_operation.md
    // "Reception").
    if (!bit(rxout_, ep) || avout_fifo_.empty() || rx_fifo_.size() + 1 >= kRxDepth) {
      return is_iso ? std::vector<uint8_t>{} : reply(USB_PID_NAK);
    }
    // USB 2.0 8.6.4: a DATAx whose toggle does not match is a retry of a packet already received:
    // ACKed and discarded (usbdev.hjson count_out.datatog_out counts these).
    if (!is_iso && data1 != bool((out_toggle_ >> ep) & 1)) return reply(USB_PID_ACK);
    receive(false, ep, data, avout_fifo_, false);
    if (!is_iso) out_toggle_ ^= uint16_t(1u << ep);
    if (bit(set_nak_out_, ep)) set_value(rxout_, value(rxout_) & ~(1u << ep));
    return is_iso ? std::vector<uint8_t>{} : reply(USB_PID_ACK);
  }
  std::vector<uint8_t> in_token(unsigned ep) {
    // ep_in_enable: "If the bit is clear then any IN packets sent to the endpoint will be ignored."
    if (!bit(ep_in_en_, ep)) return {};
    bool is_iso = iso(in_iso_, ep);
    if (!is_iso && bit(in_stall_, ep)) return reply(USB_PID_STALL);
    uint32_t ci = value(configin_[ep]);
    // USB 2.0: an isochronous transaction has a token and a data phase and no handshake, so with no
    // buffer ready the device sends a zero-length data packet (usbdev's documents do not say; see
    // status/dv_tickets.md), DATA0 at full speed.
    if (!(ci & kRdy)) {
      if (!is_iso) return reply(USB_PID_NAK);
      sys_.emit(desc().window, "packet chip->host DATA0 0 bytes");
      return {USB_PID_DATA0, 0, 0};
    }
    // configin.sending "becomes set upon the first attempt by the host to collect a buffer".
    set_value(configin_[ep], ci | kSending);
    unsigned buffer = ci & 0x1f, size = (ci >> 8) & 0x7f;
    std::vector<uint8_t> pkt(1 + size + 2);
    pkt[0] = !is_iso && ((in_toggle_ >> ep) & 1) ? USB_PID_DATA1 : USB_PID_DATA0;
    Eg2RegBlock::win_read("buffer", 64u * buffer, pkt.data() + 1, size);
    uint32_t crc = CRC16(pkt.data() + 1, int(size));
    pkt[1 + size] = uint8_t(crc);
    pkt[2 + size] = uint8_t(crc >> 8);
    std::ostringstream s;
    s << "packet chip->host " << (pkt[0] == USB_PID_DATA1 ? "DATA1 " : "DATA0 ") << size << " bytes";
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned i = 0; i < size; ++i) s << ' ' << kHex[pkt[1 + i] >> 4] << kHex[pkt[1 + i] & 15];
    sys_.emit(desc().window, s.str());
    in_pending_ = int(ep);
    // in_iso: "No handshake packet will be expected for an IN transaction."
    if (is_iso) in_acked();
    return pkt;
  }
  void in_acked() {
    if (in_pending_ < 0) return;
    unsigned ep = unsigned(in_pending_);
    in_pending_ = -1;
    // theory_of_operation.md "Transmission": on the ACK the rdy bit is cleared and the endpoint's
    // in_sent bit set; configin.sending "is cleared when the packet has been collected".
    set_value(configin_[ep], value(configin_[ep]) & ~(kRdy | kSending));
    set_value(in_sent_, value(in_sent_) | (1u << ep));
    if (!iso(in_iso_, ep)) in_toggle_ ^= uint16_t(1u << ep);
    sys_.emit_in(desc().window, "packet host->chip ACK");
    update_fifos();
  }

  // ---- The host: usbdpi (usb-host/, the model's copy of OpenTitan's RTL USB host) above its line
  // layer, one step per full-speed bit interval; whole packets go to usb_packet instead of D+/D-.
  static constexpr uint64_t kBitPs = 83333;  // 12 Mbit/s
  uint32_t usb_d2p() const {
    bool dp, dn;
    pullups(dp, dn);
    // The wake module's pull-up outputs "drive the actual pullups on the USB" while it is active
    // (theory_of_operation.md "AON/Wake Interface Pins"), whatever VBUS does; usbdev's own count
    // only with VBUS (see the header).
    bool vbus = vbus_on() || active_;
    return (dp && vbus ? D2P_DPPU : 0) | (dn && vbus ? D2P_DNPU : 0);
  }
  void host_start() {
    host_ = static_cast<usbdpi_ctx_t *>(usbdpi_create("usbdpi", 0));
    sys_.at(0, [this] { host_tick(); });
  }
  void host_tick() {
    uint64_t now = sys_.now_ps();
    while (host_ps_ + kBitPs <= now) host_bit();
    sys_.at(now + 1000000, [this] { host_tick(); });  // every microsecond
  }
  // usbdpi_host_to_device without the bit-level driver (ST_SYNC .. ST_EOP): see usbdpi.c.
  void host_bit() {
    usbdpi_ctx_t *c = host_;
    host_ps_ += kBitPs;
    c->tick += 4;
    c->tick_bits = c->tick >> 2;
    uint32_t d2p = usb_d2p();
    if (c->tick_bits == SENSE_AT) c->driving |= P2D_SENSE;
    // A device that drops its pull-up has gone: the host starts anew, except while it has removed
    // VBUS itself (STEP_DISCONNECT_VBUS), which ends with VBUS back and a bus reset.
    if (!(d2p & D2P_PU) && c->step != STEP_DISCONNECT_VBUS) {
      usbdpi_bus_reset(c);
      c->recovery_time = c->tick + 4 * 48;
      host_attached_ = false;
      usb_bus();
      return;
    }
    if (c->tick < c->recovery_time) {
      c->state = ST_IDLE;
      c->frame_start = c->tick_bits;
      usb_bus();
      return;
    }
    // USB 2.0 9.1.2: the host resets a device it has seen attach, and usbdev leaves Powered for
    // Active No SOF on that SE0 (theory_of_operation.md "USB Link State"). On the RTL bench the
    // SE0 comes from the bus wiring, below the level this host models.
    if (!host_attached_) {
      host_attached_ = true;
      usb_link_reset();
    }
    usbdpi_frame_tick(c);
    if (c->state == ST_IDLE) usbdpi_host_step(c, d2p);
    // The line the host leaves (SE0 of a bus reset, K of resume signaling, J or idle).
    usb_bus();
    if (c->state == ST_SYNC) host_send();
  }
  // The host's transfer (a token, and a data packet from data_start), then the device's reply as
  // the bus monitor would decode it.
  void host_send() {
    usbdpi_ctx_t *c = host_;
    const usbdpi_transfer_t *t = c->sending;
    size_t n = t->num_bytes, ds = t->data_start;
    bool two = ds != USBDPI_NO_DATA_STAGE && ds > 0 && ds < n;
    std::vector<uint8_t> r = usb_packet(t->data, two ? ds : n);
    if (two) r = usb_packet(t->data + ds, n - ds);
    if (dbg_on(1)) {
      // Flushed at once: usbdpi's asserts abort the run before the logs are written.
      auto hex = [](const uint8_t *p, size_t k) {
        std::string s;
        static constexpr char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < k; ++i) s += std::string(i ? " " : "") + kHex[p[i] >> 4] + kHex[p[i] & 15];
        return s.empty() ? std::string("none") : s;
      };
      std::printf("[eg2 usbdev] %.3f us: host %s -> device %s\n", sys_.now_ps() / 1e6,
                  hex(t->data, n).c_str(), hex(r.data(), r.size()).c_str());
      std::fflush(stdout);
    }
    // The packets' time on the bus, approximately (SYNC, the bytes, EOP, turnaround).
    uint64_t bits = 8 * (n + (two ? 2 : 1)) + 8 * (r.size() + 1);
    c->tick += 4 * bits;
    c->tick_bits = c->tick >> 2;
    host_ps_ += bits * kBitPs;
    c->state = ST_IDLE;
    if (r.empty()) return;
    c->state = ST_GET;
    usbdpi_data_callback(c, UsbMon_DataType_Sync, 0);
    c->lastrxpid = r[0];
    usbdpi_data_callback(c, UsbMon_DataType_PID, r[0]);
    for (size_t i = 1; i < r.size(); ++i) usbdpi_data_callback(c, UsbMon_DataType_Byte, r[i]);
    usbdpi_device_eop(c);
    usbdpi_data_callback(c, UsbMon_DataType_EOP, 0);
    c->state = ST_IDLE;
  }

  uint16_t usbctrl_, usbstat_, sense_, drive_, config_, wake_ctl_, wake_ev_, rxfifo_, rxsetup_,
      rxout_, set_nak_out_, out_stall_, in_stall_, fifo_ctrl_;
  uint16_t configin_[kEps], avout_, avsetup_, ep_out_en_, ep_in_en_;
  uint16_t in_sent_, out_tog_, in_tog_, out_iso_, in_iso_;
  uint16_t out_toggle_ = 0, in_toggle_ = 0;
  bool tok_valid_ = false, tok_setup_ = false, host_attached_ = false;
  // usb_bus: VBUS as last seen, an SE0 seen while Resuming; run lengths in bits of SE0, J without
  // a packet, time since a SOF, and the wake module's non-J and SE0.
  bool vbus_prev_ = false, resume_se0_ = false;
  uint32_t se0_bits_ = 0, bus_idle_ = 0, since_sof_ = 0, aon_nonj_ = 0, aon_se0_ = 0;
  uint32_t aon_ev_ = 0;  // wake_events while the wake module is active
  unsigned tok_ep_ = 0;
  int in_pending_ = -1;
  usbdpi_ctx_t *host_ = nullptr;
  uint64_t host_ps_ = 0;
  std::deque<uint8_t> avout_fifo_, avsetup_fifo_;
  std::deque<uint32_t> rx_fifo_;
  bool active_ = false, held_dp_ = false, held_dn_ = false, vbus_seen_ = false;
  uint64_t vbus_at_ = 0;
  unsigned lines_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_USB_H_
