// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The core system of the EG2 SystemC model (milestone M3): rv_plic, the alert handler,
// rv_core_ibex's configuration window (the CHERIoT mode switch, the DV window) and rv_timer. Each
// from its IP's documentation (hw/ip/<ip>/README.md and doc/, the hjson); where the documentation is
// silent and the RTL was read, the comment says so.

#ifndef EG2_MODEL_IP_EG2_CORE_SYSTEM_H_
#define EG2_MODEL_IP_EG2_CORE_SYSTEM_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_alert_core.h"
#include "eg2_crypto_entropy.h"

namespace eg2_model {

constexpr uint32_t kMuBi4True = 0x6, kMuBi4False = 0x9;

inline std::string fmt(const char *f, unsigned i) {
  char b[64];
  std::snprintf(b, sizeof b, f, i);
  return b;
}

// rv_plic (hw/ip_templates/rv_plic, doc/theory_of_operation.md): level-triggered gateways, one
// target. A source's pending bit is set while its line is high and it is not claimed; a claim (a
// read of CC0) returns the highest-priority pending enabled source (the lowest ID
// on a tie) and clears its pending bit; a complete (a write of its ID to CC0) lets the gateway
// take the line again. The external interrupt is any pending enabled source above the threshold;
// MSIP0 is the core's software interrupt.
//
// The number of sources is the block's: one PRIO register each (PRIO_<i> in EG2's rv_plic, PRIO<i>
// in Sonata's), IP and IE0 one bit each (IP_<w> / IE0_<w>, or IP / IE0 where one word holds them
// all). msip: whether MSIP0 drives the core's software interrupt (Sonata leaves msip_o open).
class Eg2RvPlic : public Eg2IpModel {
 public:
  Eg2RvPlic(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base, bool msip = true)
      : Eg2IpModel(n, d, base), msip_out_(msip) {
    while (true) {
      int r = reg_index(fmt("PRIO_%u", N));
      if (r < 0) r = reg_index(fmt("PRIO%u", N));
      if (r < 0) break;
      prio_.push_back(uint16_t(r));
      ++N;
    }
    pending_.assign(N, false);
    claimed_.assign(N, false);
    for (unsigned w = 0; w < (N + 31) / 32; ++w) {
      bool one = N <= 32 && reg_index("IP") >= 0;
      ip_.push_back(reg(one ? "IP" : fmt("IP_%u", w).c_str()));
      ie_.push_back(reg(one ? "IE0" : fmt("IE0_%u", w).c_str()));
      set_modelled(ip_.back());
    }
    thr_ = reg("THRESHOLD0");
    cc_ = reg("CC0");
    msip_ = reg("MSIP0");
    set_modelled(cc_);
    sys_.on_plic_source = [this](uint16_t id, bool level) {
      if (id >= N) return;
      if (level && !claimed_[id]) pending_[id] = true;
      update();
    };
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    std::fill(pending_.begin(), pending_.end(), false);
    std::fill(claimed_.begin(), claimed_.end(), false);
    for (unsigned i = 1; i < N; ++i) pending_[i] = sys_.plic_source(uint16_t(i));
    update();
  }
  unsigned sources() const { return N; }

  // A claim is determined only by this model's pending requests and priorities.
  // A timing-dependent difference from the DUT must not change the model's
  // gateway state. Retain these diagnostics for existing DPI callers.
  unsigned claims_adopted() const { return 0; }
  unsigned claims_unknown() const { return 0; }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == cc_) {
      uint32_t id = value(cc_);
      if (id > 0 && id < N) {
        claimed_[id] = false;
        if (sys_.plic_source(uint16_t(id))) pending_[id] = true;
      }
    }
    (void)wdata;
    (void)wmask;
    update();
  }

  void reg_before_read(uint16_t r) override {
    if (r != cc_) return;
    unsigned best = 0;
    // "Interrupt Claims": THRESHOLD0 gates notification, not claiming. Software
    // may poll CC0 even while the threshold suppresses the external interrupt.
    uint32_t best_prio = 0;
    for (unsigned i = 1; i < N; ++i) {
      if (pending_[i] && enabled(i) && value(prio_[i]) > best_prio) {
        best = i;
        best_prio = value(prio_[i]);
      }
    }
    if (best) {
      pending_[best] = false;
      claimed_[best] = true;
    }
    set_value(cc_, best);
    update();
  }

 private:
  unsigned N = 0;  // sources, including the reserved source 0
  bool msip_out_;

  bool enabled(unsigned i) const { return (value(ie_[i / 32]) >> (i % 32)) & 1; }

  void update() {
    bool meip = false;
    for (unsigned w = 0; w < ip_.size(); ++w) {
      uint32_t v = 0;
      for (unsigned b = 0; b < 32 && 32 * w + b < N; ++b) {
        unsigned i = 32 * w + b;
        if (pending_[i]) v |= 1u << b;
        if (pending_[i] && enabled(i) && value(prio_[i]) > value(thr_)) meip = true;
      }
      set_value(ip_[w], v);
    }
    sys_.meip = meip;
    if (msip_out_) sys_.msip = value(msip_) & 1;
  }

  std::vector<uint16_t> prio_, ip_, ie_;
  uint16_t thr_, cc_, msip_;
  std::vector<bool> pending_, claimed_;
};

// The alert handler (hw/top_earlgrey/ip_autogen/alert_handler: doc/theory_of_operation.md,
// doc/programmers_guide.md, data/alert_handler.hjson), on its clock, io_div4 (top_earlgrey.hjson
// clock_srcs clk_i), whose cycles count every timer. Its behaviour is the core's
// (ip/eg2_alert_core.h, shared with the alert_handler block bench's reference model); this class
// binds it to the SystemC model: the register store, io_div4 time, the four escalation signals to
// the top's receivers (Eg2System::set_escalation: 0 rv_core_ibex NMI, 1 and 2 lc_ctrl, 3 pwrmgr),
// the class interrupts, the ping response of each sender (an AON sender answers in its slower
// clock), and fatal alerts re-sent by their senders.
//
// A fatal alert arrives once, where the chip's sender repeats it until reset (it counts again in an
// enabled class's accumulator); the low-power groups are not modelled here (no alert is ignored).
class Eg2AlertHandler : public Eg2IpModel, private Eg2AlertPort {
 public:
  enum : uint32_t {
    kIdle = Eg2AlertCore::kIdle,
    kTimeout = Eg2AlertCore::kTimeout,
    kFsmError = Eg2AlertCore::kFsmError,
    kTerminal = Eg2AlertCore::kTerminal,
    kPhase0 = Eg2AlertCore::kPhase0
  };
  enum : unsigned {
    kClasses = Eg2AlertCore::kClasses,
    kLocAlerts = Eg2AlertCore::kLocAlerts,
    kShadowUpdateErr = Eg2AlertCore::kShadowUpdateErr,
  };

  Eg2AlertHandler(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), core_(*this, eg2_irqs::kAlerts) {
    for (uint16_t r : core_.modelled_regs()) set_modelled(r);
    sys_.on_alert = [this](uint16_t id) { alert(id); };
    sys_.alert_crashdump = [this] { return crashdump(); };
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    core_.reset();
  }
  void on_time(uint64_t) override { core_.advance(); }
  void on_clock_change() override { core_.advance(); }

  // An alert from a module (its ID in the top's alert list).
  void alert(uint16_t id) { core_.alert(id); }
  // A local alert (LOC_ALERT index).
  void local_alert(unsigned i) { core_.local_alert(i); }
  // The crash dump at this time (alert_crashdump_t, 32-bit words, word 0 least significant).
  std::vector<uint32_t> crashdump() { return core_.crashdump(); }

 protected:
  bool reg_before_write(uint16_t r, uint32_t wdata, uint32_t wmask) override {
    return core_.before_write(r, wdata, wmask);
  }
  void reg_before_read(uint16_t r) override { core_.before_read(r); }
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    core_.after_write(r, wdata, wmask);
    // Fatal alerts "are continuously triggered until reset" (comportability README, "Alerts"; the
    // alert sender latches a fatal alert until reset; Eg2IpModel::fatal_fault): an ALERT_CAUSE
    // cleared while its sender still holds the alert is set again by the next alert event.
    uint16_t id;
    if (core_.is_cause(r, &id) && !(value(r) & 1)) {
      for (Eg2IpModel *m : sys_.models()) {
        bool held = false;
        for (uint16_t a : m->fatal_alerts_) held = held || a == id;
        if (held) {
          alert(id);
          break;
        }
      }
    }
  }

 private:
  static constexpr Eg2System::Clk kClk = Eg2System::Clk::kIoDiv4;

  // ---- Eg2AlertPort ----
  const eg2_regs::Block &desc() const override { return Eg2IpModel::desc(); }
  uint32_t value(uint16_t r) const override { return Eg2IpModel::value(r); }
  void set_value(uint16_t r, uint32_t v) override { Eg2IpModel::set_value(r, v); }
  uint64_t now_cyc() const override { return sys_.clock_cycles(kClk, sys_.now_ps()); }
  void schedule(uint64_t cyc) override {
    if (cyc == Eg2AlertPort::kNever) {
      sys_.unschedule(this);
    } else {
      sys_.schedule(this, sys_.clock_time_of_cycle(kClk, cyc));
    }
  }
  void set_intr(unsigned k) override { Eg2IpModel::set_intr(k); }
  void set_escalation(unsigned e, bool level) override { sys_.set_escalation(e, level); }
  bool escalation(unsigned e) const override { return sys_.escalation(e); }
  void note(const std::string &s) override { sys_.note(s); }
  std::string alert_name(uint16_t id) const override {
    for (const eg2_irqs::Ip &ip : eg2_irqs::kIps) {
      for (uint8_t i = 0; i < ip.nalerts; ++i) {
        if (ip.alerts[i].id == id) return std::string(ip.module) + "." + ip.alerts[i].name;
      }
    }
    return std::to_string(id);
  }
  bool aon_sender(unsigned id) const {
    for (const auto &ip : eg2_irqs::kIps) {
      for (unsigned a = 0; a < ip.nalerts; ++a) {
        if (ip.alerts[a].id != id) continue;
        for (const char *m : eg2_irqs::kAonModules) {
          if (std::string(ip.module) == m) return true;
        }
        return false;
      }
    }
    return false;
  }
  // top_earlgrey.hjson gives alert_handler 24 MHz (io_div4) and AON senders 200 kHz; the alert
  // handshake diagram says a request at time 1 responds at time 4.
  uint64_t alert_ping_response(unsigned id) const override {
    if (!aon_sender(id)) return 3;
    uint64_t handler = sys_.clock_hz(Eg2System::Clk::kIoDiv4);
    uint64_t sender = sys_.clock_hz(Eg2System::Clk::kAon);
    return std::max<uint64_t>(3, (3 * handler + sender - 1) / sender);
  }

  Eg2AlertCore core_;
};

// rv_core_ibex's configuration window (rv_core_ibex.hjson; RFC CHERIoT in OpenTitan "mode
// switch"; hw/top_earlgrey/ip_autogen/rv_core_ibex/doc/theory_of_operation.md). Its "Simple
// Address Translation" has two NAPOT regions on each bus, with the lowest index winning; matching
// accesses keep their offset in the region and take the upper bits from REMAP_ADDR.
//
// CHERIOT_LOCK
// written with MuBi4True locks the mode CHERIOT_ENA holds; writes after the lock change nothing. A
// lock with any other value, or a CHERIOT_ENA that is neither True nor False at the lock, is the
// switch's error state ("Error State"): fatal_hw_err, and Ibex's internal major alert on the invalid
// mode sets ERR_STATUS.FATAL_CORE_ERR and halts instruction fetch until reset. A lock of CHERIoT
// mode goes to Eg2System::on_cheriot_lock, the core's switch to its CHERIoT-mode instruction-set
// model (M4); with none, the run stops at the lock, saying so.
//
// Fatal hardware errors ("Fetch Enable", "Local Escalation Path"): fatal_hw_err disables fetch until
// rv_core_ibex is reset (Eg2System::halt_core). The fault `lockstep` (an input event: eg2sim
// --fault, eg2_sysmodel_fault) is the dual-core lockstep comparator's mismatch: Ibex raises
// alert_major_internal_o, so ERR_STATUS.FATAL_CORE_ERR, fatal_hw_err and the halt.
//
// NMIs (NMI_ENABLE, NMI_STATE): source 0 is the alert handler's escalation signal 0 (the top's
// esc_tx[0]), source 1 the watchdog bark. NMI_STATE records a source while it is asserted (a
// software clear while it still is has no effect: hwaccess hrw); the core takes an NMI when a
// recorded and enabled source appears. The core's re-taking of a source still asserted after its
// handler's mret is not modelled.
//
// The DV window (DV_SIM_WINDOW, the chip bench's sw_test_status): word 0 is the test status
// (sw/device/lib/testing/test_framework/status.h): 0x900d passed and 0xbaad failed end the run with
// that verdict, 0xb057 is the ROM's halt. Word 1 is the DV log's bypass (device_sim_dv.c): each
// word goes to the system's DV-log decoder (eg2_dvlog.h).
//
// SW_RECOV_ERR / SW_FATAL_ERR written with anything but MuBi4False raise recov_sw_err /
// fatal_sw_err. RND_DATA, from the hjson: "Random bits taken from the EDN ... When valid, reading
// from this register invalidates the data and requests new data from the EDN. The register becomes
// valid again when the EDN provides new data. When invalid the register value will read as 0x0
// with an EDN request for new data pending. Upon reset the data will be invalid with a new EDN
// request pending." The words come from edn0 (Eg2Edn::get); after the request has been observable
// as pending, it is retried whenever software reads RND_STATUS or RND_DATA.
class Eg2RvCoreIbexCfg : public Eg2IpModel {
 public:
  Eg2RvCoreIbexCfg(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ena_ = reg("CHERIOT_ENA");
    mcw_ = reg("MCOUNTEREN_WRITABLE");
    sys_.mcounteren_writable = [this] { return (value(mcw_) & 0xf) == kMuBi4True; };
    lock_ = reg("CHERIOT_LOCK");
    recov_ = reg("SW_RECOV_ERR");
    fatal_ = reg("SW_FATAL_ERR");
    rnd_data_ = reg("RND_DATA");
    rnd_status_ = reg("RND_STATUS");
    dv_ = win_index("DV_SIM_WINDOW");
    nmi_enable_ = reg("NMI_ENABLE");
    nmi_state_ = reg("NMI_STATE");
    err_status_ = reg("ERR_STATUS");
    for (unsigned i = 0; i < 2; ++i) {
      ibus_en_[i] = reg(fmt("IBUS_ADDR_EN_%u", i).c_str());
      ibus_match_[i] = reg(fmt("IBUS_ADDR_MATCHING_%u", i).c_str());
      ibus_remap_[i] = reg(fmt("IBUS_REMAP_ADDR_%u", i).c_str());
      dbus_en_[i] = reg(fmt("DBUS_ADDR_EN_%u", i).c_str());
      dbus_match_[i] = reg(fmt("DBUS_ADDR_MATCHING_%u", i).c_str());
      dbus_remap_[i] = reg(fmt("DBUS_REMAP_ADDR_%u", i).c_str());
    }
    sys_.addr_translate = [this](uint32_t addr, bool ibus) { return translate(addr, ibus); };
    set_modelled(rnd_data_);
    set_modelled(rnd_status_);
    set_modelled(nmi_state_);
    set_modelled(err_status_);
    on_reset(Eg2ResetCause::kPor);
    if (Eg2IpModel *m = sys_.model("edn0")) edn0_ = dynamic_cast<eg2_model::Eg2Edn *>(m);
    // The top's esc_tx[0]: the alert NMI.
    sys_.esc_receiver(0, [this](bool level) { nmi_level(0, level); });
    sys_.fault_handlers["lockstep"] = [this] {
      sys_.fault_applied("lockstep",
                         "the dual-core lockstep comparator reports a mismatch (Ibex "
                         "alert_major_internal_o): ERR_STATUS.FATAL_CORE_ERR, fatal_hw_err, "
                         "instruction fetch disabled until reset");
      fatal_core_error("dual-core lockstep mismatch");
      return true;
    };
    // A response integrity error on one of the core's bus ports (interfaces.md fatal_hw_err: "A bus
    // integrity error is seen"; rv_core_ibex.hjson ERR_STATUS.FATAL_INTG_ERR "rv_core_ibex detected
    // a response integrity error"; Ibex security.rst "Bus integrity checking": a bus major alert).
    sys_.core_bus_intg_error = [this](const std::string &why) { fatal_intg_error(why); };
    sys_.core_double_fault = [this](const std::string &why) {
      sys_.halt_core("rv_core_ibex", "fatal_hw_err (double fault: " + why +
                                         "): instruction fetch disabled");
      raise_alert("fatal_hw_err");
    };
  }

  // ERR_STATUS.FATAL_INTG_ERR, fatal_hw_err, and fetch disabled until rv_core_ibex is reset
  // (theory_of_operation.md "Fetch Enable").
  void fatal_intg_error(const std::string &why) {
    set_value(err_status_, value(err_status_) | (1u << 8));
    sys_.halt_core("rv_core_ibex", "fatal_hw_err (" + why + "): instruction fetch disabled");
    raise_alert("fatal_hw_err");
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    locked_ = false;
    cheriot_ = false;
    switch_error_ = false;
    set_value(rnd_data_, 0);
    set_value(rnd_status_, 0);
    rnd_ready_cycle_ = 0;
    nmi_src_ = 0;
    nmi_line_ = 0;
    sys_.nmi_level = false;
  }

  bool cheriot() const { return cheriot_; }

  // An NMI source (0 alert escalation, 1 watchdog bark) asserted (once: the watchdog bark) or its
  // level (the escalation signal).
  void nmi(unsigned src) {
    set_value(nmi_state_, value(nmi_state_) | (1u << src));
    update_nmi();
  }
  void nmi_level(unsigned src, bool level) {
    nmi_src_ = level ? (nmi_src_ | (1u << src)) : (nmi_src_ & ~(1u << src));
    if (level) {
      sys_.note(std::string("rv_core_ibex: NMI source ") + (src ? "watchdog" : "alert escalation") +
                " asserted");
      nmi(src);
    }
  }

  // A fatal internal error of the core (Ibex's alert_major_internal_o): ERR_STATUS.FATAL_CORE_ERR,
  // fatal_hw_err, and fetch disabled until rv_core_ibex is reset.
  void fatal_core_error(const std::string &why) {
    set_value(err_status_, value(err_status_) | (1u << 9));
    sys_.halt_core("rv_core_ibex", "fatal_hw_err (" + why + "): instruction fetch disabled");
    raise_alert("fatal_hw_err");
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    // theory_of_operation.md, "Translation and Instruction Caching": software must invalidate
    // cached instructions after translation controls change. Drop both of Spike's address and
    // decoded-instruction caches here as well, so the model observes the completed register write.
    for (unsigned i = 0; i < 2; ++i) {
      if (r == ibus_en_[i] || r == ibus_match_[i] || r == ibus_remap_[i] || r == dbus_en_[i] ||
          r == dbus_match_[i] || r == dbus_remap_[i]) {
        if (sys_.addr_translation_changed) sys_.addr_translation_changed();
        break;
      }
    }
    uint32_t w = wdata & wmask;
    if (r == nmi_state_ || r == nmi_enable_) {
      // A source still asserted stays recorded.
      set_value(nmi_state_, value(nmi_state_) | nmi_src_);
      update_nmi();
      return;
    }
    if (r == lock_ && !locked_ && !switch_error_) {
      uint32_t ena = value(ena_) & 0xf;
      if ((w & 0xf) != kMuBi4True || (ena != kMuBi4True && ena != kMuBi4False)) {
        switch_error_ = true;
        sys_.note("rv_core_ibex: the CHERIoT switch enters its error state (lock 0x" + hex(w & 0xf) +
                  ", CHERIOT_ENA 0x" + hex(ena) + ")");
        fatal_core_error("the CHERIoT switch's error state, an invalid cheriot_ena");
        return;
      }
      locked_ = true;
      cheriot_ = ena == kMuBi4True;
      if (cheriot_ && sys_.on_cheriot_lock) {
        sys_.on_cheriot_lock();
      } else if (cheriot_) {
        Eg2ModelStop::request(
            "CHERIoT mode locked: the model's instruction-set model is Spike (RV32); "
            "CHERIoT mode needs MPACT-CHERIoT "
            "(eg2sim built with EG2_MPACT, not --cheriot-iss none)");
      }
    } else if (r == recov_ && (w & 0xf) != kMuBi4False) {
      raise_alert("recov_sw_err");
    } else if (r == fatal_ && (w & 0xf) != kMuBi4False) {
      raise_alert("fatal_sw_err");
    }
  }

  void reg_before_read(uint16_t r) override {
    if ((r == rnd_data_ || r == rnd_status_) && !(value(rnd_status_) & 1)) {
      // EDN theory_of_operation.md, "Peripheral Hardware Interface - Req/Ack": ack is low after
      // a word is consumed until EDN supplies fresh bits. Its waveform has separate handshakes;
      // keep the request visible through the following status/data/status reads.
      uint64_t now = sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps());
      if (now >= rnd_ready_cycle_) fetch_rnd();
    }
  }
  void reg_after_read(uint16_t r) override {
    if (r == rnd_data_ && (value(rnd_status_) & 1)) {
      set_value(rnd_data_, 0);
      set_value(rnd_status_, 0);
      uint64_t now = sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps());
      rnd_ready_cycle_ = now + 4;
    }
  }
  // The pending EDN request. EDN theory_of_operation.md: "Peripheral ports distribute data to the
  // endpoints using four signals: req, ack, bus, and fips"; "The fips signal is used to identify
  // whether the values received on the bus have been prepared with complete adherence to the
  // recommendations in NIST SP 800-90." The rv_core_ibex hjson says RND_DATA_FIPS indicates
  // whether valid RND_DATA is FIPS quality.
  void fetch_rnd() {
    if (!edn0_) edn0_ = dynamic_cast<eg2_model::Eg2Edn *>(sys_.model("edn0"));
    uint32_t w;
    bool fips = false;
    if (edn0_ && edn0_->get_core(&w, &fips)) {
      set_value(rnd_data_, w);
      set_value(rnd_status_, 1u | uint32_t(fips) << 1);
    }
  }

  void win_write(uint16_t w, uint32_t off, unsigned len) override {
    if (int(w) != dv_ || len != 4) return;
    uint32_t word;
    std::memcpy(&word, &win_mem(w)[off], 4);
    if (off == 4) {
      if (sys_.on_dv_log) sys_.on_dv_log(word);
      return;
    }
    if (off != 0) return;
    if (sys_.on_dv_status) sys_.on_dv_status(word);
    // A host program that boots the chip again after this pass (ip/eg2_test_hosts.h) takes it.
    if (word == 0x900d && sys_.host_takes_status && sys_.host_takes_status(word)) return;
    if (word == 0x900d) sys_.finish(true, "test passed (DV status 0x900d)");
    if (word == 0xbaad) sys_.finish(false, "test failed (DV status 0xbaad)");
    if (word == 0xb057) Eg2ModelStop::request("the ROM halted (DV status 0xb057)");
  }

 private:
  uint32_t translate(uint32_t addr, bool ibus) const {
    const uint16_t *en = ibus ? ibus_en_ : dbus_en_;
    const uint16_t *matching = ibus ? ibus_match_ : dbus_match_;
    const uint16_t *remap = ibus ? ibus_remap_ : dbus_remap_;
    for (unsigned i = 0; i < 2; ++i) {
      if (!(value(en[i]) & 1)) continue;
      uint32_t napot = value(matching[i]);
      unsigned ones = 0;
      while (ones < 31 && ((napot >> ones) & 1)) ++ones;
      uint32_t mask = ones == 31 ? 0xffffffffu : (uint32_t(1) << (ones + 1)) - 1;
      if ((addr & ~mask) == (napot & ~mask)) return (value(remap[i]) & ~mask) | (addr & mask);
    }
    return addr;
  }

  static std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
  }
  // The core's NMI: a recorded and enabled source that was not before.
  void update_nmi() {
    uint32_t line = value(nmi_state_) & value(nmi_enable_) & 3;
    if (line & ~nmi_line_) sys_.nmi = true;
    nmi_line_ = line;
    sys_.nmi_level = line != 0;
  }
  uint16_t mcw_, ena_, lock_, recov_, fatal_, rnd_data_, rnd_status_, nmi_enable_, nmi_state_,
      err_status_;
  uint16_t ibus_en_[2], ibus_match_[2], ibus_remap_[2];
  uint16_t dbus_en_[2], dbus_match_[2], dbus_remap_[2];
  int dv_;
  bool locked_ = false, cheriot_ = false, switch_error_ = false;
  uint64_t rnd_ready_cycle_ = 0;
  uint32_t nmi_src_ = 0, nmi_line_ = 0;
  eg2_model::Eg2Edn *edn0_ = nullptr;
};

// rv_timer (hw/ip/rv_timer/README.md, doc/theory_of_operation.md): one hart, one timer, on the
// peripheral clock (io_div4, Eg2System's clock). While CTRL.ACTIVE_0, mtime advances by CFG0.STEP
// every CFG0.PRESCALE + 1 clock cycles. INTR_STATE0 is set while the timer is active and
// mtime >= mtimecmp (set again after software clears it while that holds), and a write to the
// compare value clears it; the documentation says only "mtime >= mtimecmp raises an interrupt",
// the gating by CTRL.ACTIVE and the clear on a compare write are from the RTL (timer_core.sv,
// rv_timer.sv mtimecmp_update). Its line, state & enable, is the core's timer interrupt and a
// PLIC source.
class Eg2RvTimer : public Eg2IpModel {
 public:
  Eg2RvTimer(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    cfg_ = reg("CFG0");
    lo_ = reg("TIMER_V_LOWER0");
    hi_ = reg("TIMER_V_UPPER0");
    cmp_lo_ = reg("COMPARE_LOWER0_0");
    cmp_hi_ = reg("COMPARE_UPPER0_0");
    ie_ = reg("INTR_ENABLE0");
    is_ = reg("INTR_STATE0");
    it_ = reg("INTR_TEST0");
    set_modelled(lo_);
    set_modelled(hi_);
    set_modelled(is_);
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    rebase(0);
    evaluate();
  }
  void on_time(uint64_t) override { evaluate(); }
  void on_clock_change() override { evaluate(); }

 protected:
  void reg_before_read(uint16_t r) override {
    if (r == lo_ || r == hi_ || r == is_) evaluate();
  }
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == lo_ || r == hi_) {
      // Software wrote the count: it counts on from there.
      rebase((uint64_t(value(hi_)) << 32) | value(lo_));
    } else if (r == it_ && (wdata & wmask & 1)) {
      set_value(is_, value(is_) | 1);
    } else if (r == cmp_lo_ || r == cmp_hi_) {
      set_value(is_, value(is_) & ~1u);
    }
    evaluate();
  }

 private:
  bool active() const { return value(ctrl_) & 1; }
  uint32_t prescale() const { return value(cfg_) & 0xfff; }
  uint32_t step() const { return (value(cfg_) >> 16) & 0xff; }

  // mtime at time t: the value at the base time plus the ticks since then.
  uint64_t mtime_at(uint64_t t) const {
    if (!active_ || t <= base_ps_) return base_mtime_;
    uint64_t c = sys_.clock_cycles(kClk, t) - sys_.clock_cycles(kClk, base_ps_);
    return base_mtime_ + (c / (prescale_ + 1)) * step_;
  }
  void rebase(uint64_t mtime) {
    uint64_t now = sys_.now_ps();
    base_ps_ = now;
    base_mtime_ = mtime;
    active_ = active();
    prescale_ = prescale();
    step_ = step();
  }

  void evaluate() {
    uint64_t now = sys_.now_ps();
    // Settings written since the base apply from their write: rebase on every change.
    if (active() != active_ || prescale() != prescale_ || step() != step_) rebase(mtime_at(now));
    uint64_t mt = mtime_at(now);
    set_value(lo_, uint32_t(mt));
    set_value(hi_, uint32_t(mt >> 32));
    uint64_t cmp = (uint64_t(value(cmp_hi_)) << 32) | value(cmp_lo_);
    if (active() && mt >= cmp) set_value(is_, value(is_) | 1);
    bool line = value(is_) & value(ie_) & 1;
    sys_.mtip = line;
    drive_irq(0, line);
    // When mtime will reach mtimecmp.
    // A compare value beyond 2^40 ticks away (dif_rv_timer_reset's all-ones) is never reached
    // within a run, and would overflow the time computation.
    uint64_t ticks = step_ ? (cmp - mt + step_ - 1) / step_ : 0;
    if (active_ && step_ && mt < cmp && ticks < (1ull << 40)) {
      uint64_t c0 = sys_.clock_cycles(kClk, base_ps_);
      uint64_t done = (mt - base_mtime_) / step_;
      sys_.schedule(this, sys_.clock_time_of_cycle(kClk, c0 + (done + ticks) * (prescale_ + 1)));
    } else {
      sys_.unschedule(this);
    }
  }

  static constexpr Eg2System::Clk kClk = Eg2System::Clk::kIoDiv4;  // the peripheral clock
  uint16_t ctrl_, cfg_, lo_, hi_, cmp_lo_, cmp_hi_, ie_, is_, it_;
  uint64_t base_ps_ = 0, base_mtime_ = 0;
  bool active_ = false;
  uint32_t prescale_ = 0, step_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CORE_SYSTEM_H_
