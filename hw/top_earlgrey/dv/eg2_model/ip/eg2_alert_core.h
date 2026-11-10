// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The alert handler's behaviour as the EG2 model has it, in plain C++ (no SystemC): one core for
// two bindings, as the power manager's (eg2_pwrmgr_core.h):
//   eg2sim      Eg2AlertHandler (eg2_core_system.h), on the SystemC model's register store, time and
//               escalation receivers;
//   block bench eg2_alert_dpi.cc, its own register file, driven by the alert_handler block bench's
//               ports and TL-UL accesses (alert_handler_cmodel_sb).
// The core reaches its registers, its time and the system only through Eg2AlertPort.
//
// From the alert handler's documents (hw/top_earlgrey/ip_autogen/alert_handler:
// doc/theory_of_operation.md, doc/programmers_guide.md, data/alert_handler.hjson), never from its
// RTL. On its clock, whose cycles count every timer below (eg2sim: io_div4, top_earlgrey.hjson
// clock_srcs clk_i).
//
//   Classification  An alert enabled in ALERT_EN_SHADOWED sets its ALERT_CAUSE and triggers its
//                   class (ALERT_CLASS_SHADOWED); the local alerts likewise (LOC_ALERT_*). The
//                   class interrupt always fires. An alert of a low-power group in its low-power
//                   state is ignored ("Low-power Management of Alert Channels": the receivers
//                   "ignore alert events from the sender side"; the binding says which).
//   Accumulation    With the class enabled (CLASSx_CTRL_SHADOWED.EN) a trigger counts in
//                   CLASSx_ACCUM_CNT, saturating at 0xffff; a trigger when the count has reached
//                   CLASSx_ACCUM_THRESH escalates ("Once the threshold has been reached, the next
//                   occurrence triggers the escalation protocol"; threshold 0: the first one).
//                   Triggers of one class in the same cycle count once ("all triggers per class are
//                   ORd before sending to the accumulation counter as an increment signal").
//   Timeout         With EN and CLASSx_TIMEOUT_CYC > 0, the class interrupt's INTR_STATE bit (set
//                   by an alert or by INTR_TEST: programmer's guide) starts the Timeout state;
//                   clearing the bit returns to Idle; reaching the count escalates.
//   Escalation      Phase0..Phase3, each CLASSx_PHASEn_CYC cycles long (at least one), then
//                   Terminal until software clears the class. CLASSx_ESC_CNT counts the cycles of
//                   the Timeout state or the current phase (0 otherwise). Escalation signal e is
//                   asserted while a class with EN_Ee is in phase MAP_Ee.
//   Lock and clear  A write of 1 to CLASSx_CLR_SHADOWED, while CLASSx_CLR_REGWEN, clears the count
//                   and aborts the escalation; CTRL.LOCK clears CLR_REGWEN once the class escalates.
//   Crash dump      alert_crashdump_t (theory of operation): alert causes, local causes, counts,
//                   escalation counters and states, live until a class enters the phase of its
//                   CLASSx_CRASHDUMP_TRIGGER_SHADOWED, then held until every escalated class is
//                   cleared. Packed as SystemVerilog packs the struct (first member most
//                   significant) into 32-bit words, word 0 least significant.
//   Shadowed        Every *_SHADOWED register takes a value on the second of two equal writes
//                   (util/reggen/README.md "Shadow Registers": the first write stages, a read
//                   restarts); two different writes are the local alert "shadow reg update error"
//                   and leave the register as it was. A write while the register's REGWEN is clear
//                   is dropped and does not change the phase.
//   Local alerts    alert_handler.hjson's local alert IDs: 0 alert ping failure, 1 escalation ping
//                   failure, 2 alert integrity failure, 3 escalation integrity failure, 4 bus
//                   integrity failure, 5 shadow register update error, 6 shadow register storage
//                   error.
//   Ping            (Only where the binding models it: eg2sim. The block bench sees the RTL's
//                   LFSR-chosen pings at the ports and reports their outcome as local alerts.)
//                   With PING_TIMER_EN_SHADOWED set, the timer alternates alert senders and
//                   escalation receivers. Only enabled and locked alerts are eligible; all four
//                   escalation receivers are. A ping starts after a fixed four-cycle wait. An
//                   alert sender answers after the documented three-cycle handshake, scaled from
//                   its clock to the alert handler's clock (the binding's alert_ping_response); an
//                   escalation receiver answers after its documented one-cycle latency. A shorter
//                   timeout raises local alert 0 (alert) or 1 (escalation). The fixed wait is one
//                   permitted LFSR result: theory_of_operation.md "LFSR Timer" gives a minimum
//                   spacing of four cycles but leaves each randomized value free.
//
// Not modelled: ping signal integrity failures, bus integrity, shadow storage errors, FsmError; the
// alert handshake's latency (an alert arrives at once). The cycle the RTL takes to enter Phase0
// after the trigger is not modelled: Phase0 starts at the trigger. The gating of the count by EN is
// from the RTL (alert_handler_accu.sv), the documentation does not say.

#ifndef EG2_MODEL_IP_EG2_ALERT_CORE_H_
#define EG2_MODEL_IP_EG2_ALERT_CORE_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../eg2_model_regs.h"

// What the core needs from its binding.
class Eg2AlertPort {
 public:
  static constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
  virtual ~Eg2AlertPort() = default;
  // The register store (alert_handler's eg2_regs::Block, values by register index).
  virtual const eg2_regs::Block &desc() const = 0;
  virtual uint32_t value(uint16_t r) const = 0;
  virtual void set_value(uint16_t r, uint32_t v) = 0;
  // The alert handler's clock: the current cycle; schedule(c): call Eg2AlertCore::advance() at
  // cycle c (kNever: no call needed).
  virtual uint64_t now_cyc() const = 0;
  virtual void schedule(uint64_t cyc) = 0;
  // The class interrupt k: its INTR_STATE bit set (event-type interrupt).
  virtual void set_intr(unsigned k) = 0;
  // Escalation signal e (0..3) to its receiver.
  virtual void set_escalation(unsigned e, bool level) = 0;
  virtual bool escalation(unsigned e) const = 0;
  virtual void note(const std::string &) {}
  virtual std::string alert_name(uint16_t id) const { return std::to_string(id); }
  // Ping: whether the core runs the ping timer itself, and an alert sender's response in
  // alert-handler cycles.
  virtual bool ping_modelled() const { return true; }
  virtual uint64_t alert_ping_response(unsigned) const { return 3; }
  // Alert id's low-power group is in its low-power state (the receivers ignore its alerts).
  virtual bool lpg_low_power(uint16_t) const { return false; }
};

class Eg2AlertCore {
 public:
  enum : uint32_t { kIdle = 0, kTimeout = 1, kFsmError = 2, kTerminal = 3, kPhase0 = 4 };
  enum : unsigned {
    kClasses = 4,
    kLocAlerts = 7,
    kEscSeverities = 4,
    kLocAlertPingFail = 0,
    kLocEscPingFail = 1,
    kLocAlertIntFail = 2,
    kLocEscIntFail = 3,
    kLocBusIntFail = 4,
    kShadowUpdateErr = 5,  // LOC_ALERT index: "shadow reg update error"
  };
  static constexpr uint64_t kNever = Eg2AlertPort::kNever;

  // nalerts: the top's number of alerts (alert_handler.hjson NAlerts).
  Eg2AlertCore(Eg2AlertPort &p, unsigned nalerts) : p_(p) {
    for (unsigned i = 0; i < nalerts; ++i) {
      en_.push_back(reg(fmt("ALERT_EN_SHADOWED_", i)));
      cls_.push_back(reg(fmt("ALERT_CLASS_SHADOWED_", i)));
      cause_.push_back(reg(fmt("ALERT_CAUSE_", i)));
    }
    for (unsigned i = 0; i < kLocAlerts; ++i) {
      loc_en_.push_back(reg(fmt("LOC_ALERT_EN_SHADOWED_", i)));
      loc_cls_.push_back(reg(fmt("LOC_ALERT_CLASS_SHADOWED_", i)));
      loc_cause_.push_back(reg(fmt("LOC_ALERT_CAUSE_", i)));
    }
    ping_timeout_ = reg("PING_TIMEOUT_CYC_SHADOWED");
    ping_enable_ = reg("PING_TIMER_EN_SHADOWED");
    intr_state_ = find("INTR_STATE");
    intr_enable_ = find("INTR_ENABLE");
    const char *c = "ABCD";
    for (unsigned k = 0; k < kClasses; ++k) {
      std::string pre = std::string("CLASS") + c[k] + "_";
      ctrl_[k] = reg(pre + "CTRL_SHADOWED");
      accum_[k] = reg(pre + "ACCUM_CNT");
      thresh_[k] = reg(pre + "ACCUM_THRESH_SHADOWED");
      timeout_[k] = reg(pre + "TIMEOUT_CYC_SHADOWED");
      dump_trig_[k] = reg(pre + "CRASHDUMP_TRIGGER_SHADOWED");
      for (unsigned ph = 0; ph < 4; ++ph) {
        phase_cyc_[k][ph] = reg(pre + "PHASE" + char('0' + ph) + "_CYC_SHADOWED");
      }
      clr_[k] = reg(pre + "CLR_SHADOWED");
      clr_regwen_[k] = reg(pre + "CLR_REGWEN");
      esc_cnt_[k] = reg(pre + "ESC_CNT");
      state_[k] = reg(pre + "STATE");
    }
    staged_.assign(p_.desc().nregs, false);
    stage_.assign(p_.desc().nregs, 0);
  }

  // The registers the core keeps up to date (hardware updates them).
  std::vector<uint16_t> modelled_regs() const {
    std::vector<uint16_t> v(cause_);
    v.insert(v.end(), loc_cause_.begin(), loc_cause_.end());
    for (unsigned k = 0; k < kClasses; ++k) {
      for (uint16_t r : {accum_[k], state_[k], esc_cnt_[k], clr_regwen_[k]}) v.push_back(r);
    }
    return v;
  }

  // After the binding has reset the registers to their reset values.
  void reset() {
    for (Cls &k : cls_st_) k = Cls{};
    for (uint64_t &c : last_count_) c = kNever;
    std::fill(staged_.begin(), staged_.end(), false);
    ping_waiting_ = false;
    ping_due_ = 0;
    ping_next_alert_ = true;
    next_alert_ = 0;
    next_esc_ = 0;
    dump_latched_ = false;
    for (unsigned e = 0; e < kEscSeverities; ++e) p_.set_escalation(e, false);
  }

  // An alert from a module (its ID in the top's alert list).
  void alert(uint16_t id) {
    advance();
    if (id >= en_.size() || !(p_.value(en_[id]) & 1)) return;
    if (p_.lpg_low_power(id)) {
      p_.note("alert_handler: alert " + p_.alert_name(id) + " ignored: its low-power group is off");
      return;
    }
    p_.set_value(cause_[id], p_.value(cause_[id]) | 1);
    trigger(p_.value(cls_[id]) & 3, "alert " + p_.alert_name(id));
  }

  // A local alert (LOC_ALERT index).
  void local_alert(unsigned i) {
    advance();
    local_alert_now(i);
  }

  // The crash dump at this time (alert_crashdump_t, 32-bit words, word 0 least significant).
  std::vector<uint32_t> crashdump() {
    advance();
    return dump_latched_ ? dump_ : snapshot();
  }
  bool dump_latched() const { return dump_latched_; }

  // ---- Register access hooks for the binding ----
  // Before a TL-UL write is applied: false drops it (the first write of a shadowed register only
  // stages; a locked or mismatched one changes nothing).
  bool before_write(uint16_t r, uint32_t wdata, uint32_t wmask) {
    const eg2_regs::Reg &rd = p_.desc().regs[r];
    if (!(rd.flags & 2)) return true;  // not shadowed
    if (rd.regwen >= 0 && (p_.value(uint16_t(rd.regwen)) & rd.regwen_mask) != rd.regwen_true) {
      return true;  // locked: the block drops the write, the phase does not change
    }
    uint32_t raw = (p_.value(r) & ~wmask) | (wdata & wmask);
    if (!staged_[r]) {
      staged_[r] = true;
      stage_[r] = raw;
      return false;
    }
    staged_[r] = false;
    if (raw != stage_[r]) {
      p_.note(std::string("alert_handler: ") + rd.name +
              ": the second write differs from the first (shadow update error)");
      local_alert(kShadowUpdateErr);
      return false;
    }
    return true;
  }
  // Before a read (a read of a shadowed register restarts its staging).
  void before_read(uint16_t r) {
    if (p_.desc().regs[r].flags & 2) staged_[r] = false;
    advance();
  }
  // After a TL-UL write has been applied.
  void after_write(uint16_t r, uint32_t wdata, uint32_t wmask) {
    advance();
    for (unsigned k = 0; k < kClasses; ++k) {
      if (r == clr_[k] && (wdata & wmask & 1) && (p_.value(clr_regwen_[k]) & 1)) clear(k);
    }
    // An INTR_STATE, INTR_ENABLE or INTR_TEST write may start or stop a class's interrupt
    // timeout; a CTRL write may enable it.
    update_timeouts();
    update_outputs();
    reschedule();
  }
  bool is_cause(uint16_t r, uint16_t *id) const {
    for (uint16_t i = 0; i < cause_.size(); ++i) {
      if (cause_[i] == r) {
        *id = i;
        return true;
      }
    }
    return false;
  }

  // Every state change due by now, in time order; then the registers that count.
  void advance() {
    uint64_t n = p_.now_cyc();
    for (;;) {
      int first = -1;
      uint64_t at = 0;
      for (unsigned k = 0; k < kClasses; ++k) {
        uint32_t s = cls_st_[k].state;
        if (s != kTimeout && !in_phase(s)) continue;
        uint64_t end = cls_st_[k].start + duration(k);
        if (end <= n && (first < 0 || end < at)) {
          first = int(k);
          at = end;
        }
      }
      if (first < 0) break;
      unsigned k = unsigned(first);
      uint32_t s = cls_st_[k].state;
      if (s == kTimeout) {
        start_escalation(k, at, "the interrupt timeout");
      } else if (s < kPhase0 + 3) {
        enter(k, s + 1, at);
      } else {
        enter(k, kTerminal, at);
      }
    }
    if (p_.ping_modelled()) advance_ping(n);
    for (unsigned k = 0; k < kClasses; ++k) p_.set_value(esc_cnt_[k], esc_cnt(k, n));
    reschedule();
  }

  // ---- Queries (the block bench's binding) ----
  uint32_t class_state(unsigned k) const { return cls_st_[k].state; }
  // The cycle class k's current state began, and how long it lasts (0: until something moves it).
  uint64_t class_start(unsigned k) const { return cls_st_[k].start; }
  uint64_t class_duration(unsigned k) const { return duration(k); }
  static bool in_phase(uint32_t s) { return s >= kPhase0; }

 private:
  struct Cls {
    uint32_t state = kIdle;
    uint64_t start = 0;      // the cycle the state began
    bool escalated = false;  // escalated since the last clear
  };

  static std::string fmt(const char *pre, unsigned i) { return pre + std::to_string(i); }
  int find(const std::string &name) const {
    const eg2_regs::Block &d = p_.desc();
    for (uint16_t i = 0; i < d.nregs; ++i) {
      if (name == d.regs[i].name) return i;
    }
    return -1;
  }
  uint16_t reg(const std::string &name) const {
    int i = find(name);
    return i < 0 ? 0 : uint16_t(i);
  }

  uint32_t ctrl(unsigned k) const { return p_.value(ctrl_[k]); }
  bool en(unsigned k) const { return ctrl(k) & 1; }
  bool intr_state(unsigned k) const {
    return intr_state_ >= 0 && ((p_.value(uint16_t(intr_state_)) >> k) & 1);
  }
  // The class's interrupt output: state and enable (prim_intr_hw intr_o). The escalation timer's
  // timeout_en_i is this output (alert_handler.sv: .timeout_en_i(irq[k])), so the interrupt timeout
  // counts only while the class interrupt is enabled. (From the RTL, as eg2sim had it: the
  // documents do not say whether INTR_ENABLE gates the timeout; a finding.)
  bool intr_out(unsigned k) const {
    return intr_state(k) && intr_enable_ >= 0 &&
           ((p_.value(uint16_t(intr_enable_)) >> k) & 1);
  }
  static const char *state_name(uint32_t s) {
    static const char *const kNames[] = {"Idle",   "Timeout", "FsmError", "Terminal",
                                         "Phase0", "Phase1",  "Phase2",   "Phase3"};
    return kNames[s & 7];
  }
  void note(unsigned k, const std::string &what) {
    p_.note(std::string("alert_handler: class ") + "ABCD"[k] + ": " + what);
  }

  void local_alert_now(unsigned i) {
    if (i >= kLocAlerts || !(p_.value(loc_en_[i]) & 1)) return;
    p_.set_value(loc_cause_[i], p_.value(loc_cause_[i]) | 1);
    trigger(p_.value(loc_cls_[i]) & 3, "local alert " + std::to_string(i));
  }

  // The cycles class k stays in its state; 0: until something else moves it.
  uint64_t duration(unsigned k) const {
    uint32_t s = cls_st_[k].state;
    if (s == kTimeout) return p_.value(timeout_[k]);
    if (in_phase(s)) return std::max<uint64_t>(p_.value(phase_cyc_[k][s - kPhase0]), 1);
    return 0;
  }
  uint32_t esc_cnt(unsigned k, uint64_t n) const {
    const Cls &c = cls_st_[k];
    if (c.state != kTimeout && !in_phase(c.state)) return 0;
    return uint32_t(std::min<uint64_t>(n - c.start, 0xffffffffu));
  }

  // An alert classified into class k.
  void trigger(unsigned k, const std::string &what) {
    p_.set_intr(k);
    uint64_t n = p_.now_cyc();
    if (en(k) && last_count_[k] == n) {
      note(k, what + " (in the same cycle as the class's last trigger: counted once)");
    } else if (en(k)) {
      last_count_[k] = n;
      uint32_t cnt = p_.value(accum_[k]);
      bool escalate = cnt >= p_.value(thresh_[k]);
      if (cnt < 0xffff) p_.set_value(accum_[k], cnt + 1);
      note(k, what + ", accumulation count " + std::to_string(cnt + (cnt < 0xffff)));
      if (escalate) {
        start_escalation(k, p_.now_cyc(),
                         "the accumulation threshold " + std::to_string(p_.value(thresh_[k])));
      }
    } else {
      note(k, what + " (escalation mechanisms disabled: CTRL.EN clear)");
    }
    update_timeouts();
    update_outputs();
    reschedule();
  }

  void start_escalation(unsigned k, uint64_t at, const std::string &why) {
    Cls &c = cls_st_[k];
    if (in_phase(c.state) || c.state == kTerminal) return;
    c.escalated = true;
    if ((ctrl(k) >> 1) & 1) p_.set_value(clr_regwen_[k], 0);  // LOCK
    note(k, "escalation triggered by " + why);
    enter(k, kPhase0, at);
  }

  void enter(unsigned k, uint32_t s, uint64_t at) {
    Cls &c = cls_st_[k];
    c.state = s;
    c.start = at;
    p_.set_value(state_[k], s);
    if (in_phase(s)) {
      note(k, std::string(state_name(s)) + " for " + std::to_string(duration(k)) + " cycles");
      if (!dump_latched_ && p_.value(dump_trig_[k]) == s - kPhase0) {
        dump_ = snapshot();
        dump_latched_ = true;
        note(k, "crash dump latched");
      }
    } else if (s == kTerminal) {
      note(k, "Terminal");
    }
    update_outputs();
  }

  void clear(unsigned k) {
    Cls &c = cls_st_[k];
    p_.set_value(accum_[k], 0);
    last_count_[k] = kNever;
    if (c.state != kIdle) note(k, std::string("cleared in ") + state_name(c.state));
    c = Cls{};
    c.start = p_.now_cyc();
    p_.set_value(state_[k], kIdle);
    bool any = false;
    for (const Cls &o : cls_st_) any = any || o.escalated;
    if (!any) dump_latched_ = false;
  }

  void update_timeouts() {
    uint64_t n = p_.now_cyc();
    for (unsigned k = 0; k < kClasses; ++k) {
      Cls &c = cls_st_[k];
      bool armed = en(k) && p_.value(timeout_[k]) > 0 && intr_out(k);
      if (c.state == kIdle && armed) {
        c.state = kTimeout;
        c.start = n;
        p_.set_value(state_[k], kTimeout);
        note(k, "interrupt timeout started (" + std::to_string(p_.value(timeout_[k])) +
                    " cycles)");
      } else if (c.state == kTimeout && !armed) {
        c.state = kIdle;
        p_.set_value(state_[k], kIdle);
      }
    }
  }

  void update_outputs() {
    for (unsigned e = 0; e < kEscSeverities; ++e) {
      bool level = false;
      for (unsigned k = 0; k < kClasses; ++k) {
        uint32_t s = cls_st_[k].state, cr = ctrl(k);
        bool en_e = (cr >> (2 + e)) & 1;
        uint32_t map = (cr >> (6 + 2 * e)) & 3;
        level = level || (in_phase(s) && en_e && s - kPhase0 == map);
      }
      if (level != p_.escalation(e)) {
        p_.note("alert_handler: escalation signal " + std::to_string(e) +
                (level ? " asserted" : " deasserted"));
      }
      p_.set_escalation(e, level);
    }
  }

  void reschedule() {
    uint64_t next = kNever;
    for (unsigned k = 0; k < kClasses; ++k) {
      uint32_t s = cls_st_[k].state;
      if (s == kTimeout || in_phase(s)) next = std::min(next, cls_st_[k].start + duration(k));
    }
    if (p_.ping_modelled() && (p_.value(ping_enable_) & 1) &&
        p_.value(ping_timeout_) < max_ping_response()) {
      if (!ping_due_) ping_due_ = p_.now_cyc() + kPingSpacing;
      next = std::min(next, ping_due_);
    } else {
      ping_waiting_ = false;
      ping_due_ = 0;
    }
    p_.schedule(next);
  }

  // theory_of_operation.md "LFSR Timer": the timer "alternatingly pings either an alert line or an
  // escalation line"; "Only alerts that have been enabled and locked will be pinged" while
  // "Escalation channels are always enabled"; at the programmed timeout it "will additionally
  // raise a 'pingfail' alert." The alert handshake diagram says a request at time 1 responds at
  // time 4. alert_handler.hjson says the timeout is measured in equivalent slow-sender cycles.
  void advance_ping(uint64_t n) {
    if (!(p_.value(ping_enable_) & 1)) {
      ping_waiting_ = false;
      ping_due_ = 0;
      return;
    }
    // A successful handshake changes no software-visible state. Treat it analytically instead of
    // scheduling every four-cycle response; otherwise these benign events can hide a much later
    // wake event from a sleeping core. Only a timeout needs a model event.
    if (p_.value(ping_timeout_) >= max_ping_response()) {
      ping_waiting_ = false;
      ping_due_ = 0;
      return;
    }
    if (!ping_due_) ping_due_ = n + kPingSpacing;
    while (ping_due_ <= n) {
      uint64_t at = ping_due_;
      if (!ping_waiting_) {
        ping_is_alert_ = ping_next_alert_ && choose_alert();
        ping_next_alert_ = !ping_next_alert_;
        if (!ping_is_alert_) ping_index_ = next_esc_++ % kEscSeverities;
        ping_response_ = ping_is_alert_ ? p_.alert_ping_response(ping_index_) : 1;
        p_.note(std::string("alert_handler: ping ") +
                (ping_is_alert_ ? "alert sender " : "escalation receiver ") +
                std::to_string(ping_index_));
        ping_waiting_ = true;
        uint64_t timeout = p_.value(ping_timeout_);
        ping_due_ = at + std::min<uint64_t>(timeout, ping_response_);
        continue;
      }
      bool timed_out = p_.value(ping_timeout_) < ping_response_;
      bool alert = ping_is_alert_;
      unsigned index = ping_index_;
      ping_waiting_ = false;
      ping_due_ = at + kPingSpacing;
      if (timed_out) {
        p_.note(std::string("alert_handler: ping timeout from ") +
                (alert ? "alert sender " : "escalation receiver ") + std::to_string(index));
        local_alert_now(alert ? kLocAlertPingFail : kLocEscPingFail);
      }
    }
  }

  bool alert_locked(unsigned i) const {
    const eg2_regs::Reg &r = p_.desc().regs[en_[i]];
    return r.regwen >= 0 && !(p_.value(uint16_t(r.regwen)) & r.regwen_mask);
  }

  bool choose_alert() {
    for (unsigned n = 0; n < en_.size(); ++n) {
      unsigned i = (next_alert_ + n) % en_.size();
      if ((p_.value(en_[i]) & 1) && alert_locked(i)) {
        ping_index_ = i;
        next_alert_ = (i + 1) % en_.size();
        return true;
      }
    }
    return false;
  }

  uint64_t max_ping_response() const {
    uint64_t response = 1;  // escalation receiver: one alert-handler cycle
    for (unsigned i = 0; i < en_.size(); ++i) {
      if ((p_.value(en_[i]) & 1) && alert_locked(i)) {
        response = std::max(response, p_.alert_ping_response(i));
      }
    }
    return response;
  }

  // alert_crashdump_t: {alert_cause[NAlerts], loc_alert_cause[7], class_accum_cnt[4][16],
  // class_esc_cnt[4][32], class_esc_state[4][3]}, the last member least significant.
  std::vector<uint32_t> snapshot() const {
    const unsigned bits = unsigned(en_.size()) + kLocAlerts + 4 * 16 + 4 * 32 + 4 * 3;
    std::vector<uint32_t> w((bits + 31) / 32, 0);
    unsigned pos = 0;
    auto put = [&](uint64_t v, unsigned width) {
      for (unsigned b = 0; b < width; ++b, ++pos) {
        if ((v >> b) & 1) w[pos / 32] |= 1u << (pos % 32);
      }
    };
    uint64_t n = p_.now_cyc();
    for (unsigned k = 0; k < kClasses; ++k) put(cls_st_[k].state, 3);
    for (unsigned k = 0; k < kClasses; ++k) put(esc_cnt(k, n), 32);
    for (unsigned k = 0; k < kClasses; ++k) put(p_.value(accum_[k]), 16);
    for (unsigned i = 0; i < kLocAlerts; ++i) put(p_.value(loc_cause_[i]) & 1, 1);
    for (unsigned i = 0; i < en_.size(); ++i) put(p_.value(cause_[i]) & 1, 1);
    return w;
  }

  static constexpr uint64_t kPingSpacing = 4;

  Eg2AlertPort &p_;
  std::vector<uint16_t> en_, cls_, cause_, loc_en_, loc_cls_, loc_cause_;
  uint16_t ping_timeout_ = 0, ping_enable_ = 0;
  int intr_state_ = -1, intr_enable_ = -1;
  uint16_t ctrl_[kClasses] = {}, accum_[kClasses] = {}, thresh_[kClasses] = {},
           timeout_[kClasses] = {}, dump_trig_[kClasses] = {}, phase_cyc_[kClasses][4] = {},
           clr_[kClasses] = {}, clr_regwen_[kClasses] = {}, esc_cnt_[kClasses] = {},
           state_[kClasses] = {};
  Cls cls_st_[kClasses];
  uint64_t last_count_[kClasses] = {kNever, kNever, kNever, kNever};  // a class's last counted cycle
  std::vector<bool> staged_;
  std::vector<uint32_t> stage_;
  bool dump_latched_ = false;
  std::vector<uint32_t> dump_;
  bool ping_waiting_ = false, ping_next_alert_ = true, ping_is_alert_ = false;
  uint64_t ping_due_ = 0;
  uint64_t ping_response_ = 1;
  unsigned ping_index_ = 0, next_alert_ = 0, next_esc_ = 0;
};

#endif  // EG2_MODEL_IP_EG2_ALERT_CORE_H_
