// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2System: what the IP models of the EG2 SystemC model share (milestone M3 of the system model
// plan). One instance, reached through Eg2System::get().
//
//   Time        now_ps() is the instruction-set model's local time: SystemC time plus the time of
//               the instructions it ran since its last wait() (loosely timed, one quantum).
//               A model that has to act at a time (a timer reaching its compare value) calls
//               schedule(model, t); its on_time(now) runs at the first instruction boundary at or
//               after t, or when the core sleeps and the model is the next to act.
//   Interrupts  set_plic_source(id, level): an IP's interrupt line into rv_plic (source IDs from
//               the top, eg2_model_irqs.h). The core's lines (external, timer, software, NMI) are
//               set by rv_plic, rv_timer and rv_core_ibex; the core model reads them every
//               instruction. ips: the top's interrupt table the IP models take their lines from
//               (EG2's; another board, rtos_soc/, gives its own before it builds its models).
//   Alerts      raise_alert(id): an alert into the alert handler model (IDs from the top).
//   Escalation  esc_receiver(sev, f): an escalation receiver of the top (top_earlgrey.hjson
//               alert_handler.esc_tx: 0 rv_core_ibex, 1 and 2 lc_ctrl, 3 pwrmgr) takes severity
//               sev's level; set_escalation(sev, level) is the alert handler's sender.
//               alert_crashdump: the alert handler's crash dump as rstmgr captures it.
//   Core halt   halt_core(source, why): a source disables the core's instruction fetch until the
//               next reset (rv_core_ibex's fatal_hw_err local escalation, lc_ctrl's lc_cpu_en off);
//               the core executes nothing and lets the models run on.
//   Faults      fault handlers by name (lockstep, cheriot_ena_low): a fault injected as an input
//               event (eg2sim --fault, eg2_sysmodel_fault), never a change to a model's behaviour;
//               fault_applied reports when it changes what the chip does.
//   Reset       request_reset(cause): the core and every model restart, as after a reset of the
//               chip; each model decides what it keeps (on_reset), after every model has seen it
//               coming (before_reset: rstmgr captures the crash dump).
//   Verdict     finish(passed, why): the run ends with the software's verdict (the DV window).
//   Outputs     emit(instance, line): one line per item into <instance>.model.log, the format of
//               the bench's <instance>.chip.log, for the agent comparisons (M6); on_emit also
//               gets each item.
//   Inputs      inputs[key](value): an agent's item delivered while the model runs (the chip
//               bench's library, dpi/).
//   DV log      on_dv_log(word): each word software writes to the DV window's log word
//               (eg2sim decodes them, eg2_dvlog.h).
//   Execution   add_exec_region(memory window, allowed): the controller of a memory that can
//               refuse instruction fetches (sram_ctrl EXEC, rram_ctrl EXEC) registers it;
//               fetch_allowed(addr) is what the core asks before each instruction. fetch_map, when
//               set, is the instruction side's own address decode (a board whose instruction
//               crossbar reaches fewer devices than its data crossbar).
//   Monitors    at(t, f): f at time t, for what is not an IP model (a board's run monitor).

#ifndef EG2_MODEL_EG2_SYSTEM_H_
#define EG2_MODEL_EG2_SYSTEM_H_

#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "eg2_model_irqs.h"
#include "eg2_targets.h"

class Eg2IpModel;

// Why the chip resets; rstmgr records it in RESET_INFO.
enum class Eg2ResetCause { kPor, kLowPowerExit, kSoftware, kHwRequest, kEscalation, kNdm };

class Eg2System {
 public:
  static constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

  static Eg2System &get() {
    static Eg2System s;
    return s;
  }

  // ---- Time ----
  std::function<uint64_t()> clock;  // set by the core model
  uint64_t now_ps() const { return clock ? clock() : 0; }
  // The number of whole cycles of a clock of hz that have passed by time t.
  static uint64_t cycles(uint64_t t_ps, uint64_t hz) {
    return uint64_t((__uint128_t(t_ps) * hz) / 1000000000000ull);
  }
  // The time at which a clock of hz completes cycle n.
  static uint64_t time_of_cycle(uint64_t n, uint64_t hz) {
    return uint64_t((__uint128_t(n) * 1000000000000ull + hz - 1) / hz);
  }

  // ---- The chip's clocks ----
  // main, io_div4 and aon run at their sim_dv nominal frequency until set_clock_hz gives another
  // from a time on: the chip bench measures the chip's clocks (input clk.<name>, in Hz), whose
  // oscillators start at a random frequency and are calibrated later. io is io_div4's source,
  // four times its frequency. A model that times anything by a clock asks here, never assumes one.
  enum class Clk { kMain, kIo, kIoDiv4, kAon };
  // The frequency at time t_ps.
  uint64_t clock_hz(Clk c, uint64_t t_ps) const {
    const ClockSeg &s = seg_at(track(c), t_ps);
    return c == Clk::kIo ? 4 * s.hz : s.hz;
  }
  uint64_t clock_hz(Clk c) const { return clock_hz(c, now_ps()); }
  // The cycles of clock c completed by time t_ps, counted across frequency changes.
  uint64_t clock_cycles(Clk c, uint64_t t_ps) const {
    const ClockSeg &s = seg_at(track(c), t_ps);
    uint64_t n = s.cycles + cycles(t_ps - s.t_ps, s.hz);
    return c == Clk::kIo ? 4 * n : n;
  }
  // The time at which clock c completes cycle n (at its frequencies as known now).
  uint64_t clock_time_of_cycle(Clk c, uint64_t n) const {
    if (c == Clk::kIo) n = (n + 3) / 4;
    const std::vector<ClockSeg> &tr = track(c);
    size_t i = tr.size() - 1;
    while (i > 0 && tr[i].cycles > n) --i;
    return tr[i].t_ps + time_of_cycle(n - tr[i].cycles, tr[i].hz);
  }
  // From t_ps on, clock c (main, io_div4 or aon) runs at hz. Returns whether that changed it.
  bool set_clock_hz(Clk c, uint64_t hz, uint64_t t_ps) {
    std::vector<ClockSeg> &tr = track(c);
    if (hz == 0 || c == Clk::kIo || hz == tr.back().hz) return false;
    if (t_ps < tr.back().t_ps) t_ps = tr.back().t_ps;
    uint64_t n = clock_cycles(c, t_ps);
    if (t_ps == tr.back().t_ps) tr.back().hz = hz;
    else tr.push_back({t_ps, n, hz});
    clock_changed();
    return true;
  }
  // Every model's on_clock_change (eg2_ip_model.h): one with an event timed by a clock re-times it.
  void clock_changed();

  // m->on_time(now) at or after t; replaces m's earlier request.
  void schedule(Eg2IpModel *m, uint64_t t_ps) { events_[m] = t_ps; }
  void unschedule(Eg2IpModel *m) { events_.erase(m); }
  // f() at or after t, once (run_due runs it with the models due).
  void at(uint64_t t_ps, std::function<void()> f) { timed_.emplace(t_ps, std::move(f)); }
  uint64_t next_event_ps() const {
    uint64_t t = timed_.empty() ? kNever : timed_.begin()->first;
    for (auto &e : events_) t = std::min(t, e.second);
    return t;
  }
  void run_due(uint64_t now);  // eg2_ip_model.h

  // ---- Interrupts ----
  // The interrupt table (module, register window, interrupts with their PLIC source IDs).
  const eg2_irqs::Ip *ips = eg2_irqs::kIps;
  size_t nips = std::size(eg2_irqs::kIps);
  // The source ID of a line that goes to no PLIC source (an interrupt another block takes).
  static constexpr uint16_t kNoPlicSource = 0xffff;
  void set_plic_source(uint16_t id, bool level) {
    if (id >= eg2_irqs::kPlicSources || plic_src_[id] == level) return;
    plic_src_[id] = level;
    if (on_plic_source) on_plic_source(id, level);
  }
  bool plic_source(uint16_t id) const { return id < eg2_irqs::kPlicSources && plic_src_[id]; }
  std::function<void(uint16_t, bool)> on_plic_source;  // set by the rv_plic model
  bool meip = false, mtip = false, msip = false, nmi = false;

  // ---- Alerts ----
  std::function<void(uint16_t)> on_alert;  // set by the alert handler model
  void raise_alert(uint16_t id) {
    if (on_alert) on_alert(id);
  }
  // The alert ID of a module's alert, by name (eg2_model_irqs.h); -1 if there is none.
  static int alert_id(const std::string &module, const std::string &name) {
    for (const eg2_irqs::Ip &ip : eg2_irqs::kIps) {
      if (module != ip.module) continue;
      for (uint8_t i = 0; i < ip.nalerts; ++i) {
        if (name == ip.alerts[i].name) return ip.alerts[i].id;
      }
    }
    return -1;
  }

  // ---- Escalation ----
  // The four escalation severities and their receivers (top_earlgrey.hjson alert_handler.esc_tx).
  static constexpr unsigned kEscSeverities = 4;
  void esc_receiver(unsigned sev, std::function<void(bool)> f) {
    if (sev < kEscSeverities) esc_rx_[sev].push_back(std::move(f));
  }
  void set_escalation(unsigned sev, bool level) {
    if (sev >= kEscSeverities || esc_[sev] == level) return;
    esc_[sev] = level;
    for (auto &f : esc_rx_[sev]) f(level);
  }
  bool escalation(unsigned sev) const { return sev < kEscSeverities && esc_[sev]; }
  // The alert handler's crash dump (alert_crashdump_t in 32-bit words, word 0 the least
  // significant), set by the alert handler model; rstmgr captures it.
  std::function<std::vector<uint32_t>()> alert_crashdump;

  // ---- Core halt (instruction fetch disabled) ----
  void halt_core(const std::string &source, const std::string &why) {
    if (halts_.count(source)) return;
    halts_[source] = why;
    note("core halted by " + source + ": " + why);
  }
  bool core_halted() const { return !halts_.empty(); }
  std::string halt_reason() const {
    std::string r;
    for (auto &h : halts_) r += (r.empty() ? "" : "; ") + h.first + ": " + h.second;
    return r;
  }

  // ---- Faults (input events) ----
  // A fault's handler applies it now (or arms it, for one that changes something later) and
  // returns false for a name it does not know. A handler calls fault_applied when the fault
  // changes what the chip does: the run report says so, so that a fault that never applied is
  // told from one that applied and was missed.
  std::map<std::string, std::function<bool()>> fault_handlers;
  bool inject_fault(const std::string &name) {
    auto it = fault_handlers.find(name);
    return it != fault_handlers.end() && it->second();
  }
  std::string run_name = "eg2sim";
  std::function<std::string()> where;  // "<n> instructions, pc 0x..., <t> us" (set by the core)
  void fault_applied(const std::string &name, const std::string &what) {
    std::string at = where ? where() : std::to_string(now_ps()) + " ps";
    std::printf("%s: fault %s applied at %s: %s\n", run_name.c_str(), name.c_str(), at.c_str(),
                what.c_str());
    std::fflush(stdout);
    faults_applied_.push_back(name);
    if (logs) {
      FILE *&f = logs_["model.notes"];
      if (!f) f = std::fopen((log_dir + "/model.notes.log").c_str(), "w");
      if (f) {
        std::fprintf(f, "%llu ps: fault %s applied: %s\n", (unsigned long long)now_ps(),
                     name.c_str(), what.c_str());
        std::fflush(f);
      }
    }
  }
  const std::vector<std::string> &faults_applied() const { return faults_applied_; }

  // ---- Reset and low power ----
  // hw_req: for kHwRequest, the request's index in rstmgr's RESET_INFO.HW_REQ.
  void request_reset(Eg2ResetCause cause, const std::string &why, int hw_req = -1) {
    if (!reset_pending_) {
      reset_pending_ = true;
      reset_cause_ = cause;
      reset_why_ = why;
      reset_hw_req_ = hw_req;
    }
  }
  int reset_hw_req() const { return reset_hw_req_; }
  bool reset_pending() const { return reset_pending_; }
  Eg2ResetCause reset_cause() const { return reset_cause_; }
  const std::string &reset_why() const { return reset_why_; }
  // The reset has been applied: fetch is enabled again (each halt's source restarts in reset), the
  // escalation receivers have reset with their senders.
  void reset_done() {
    reset_pending_ = false;
    halts_.clear();
    for (bool &e : esc_) e = false;
  }
  // Every model's before_reset, then every model's on_reset (eg2_ip_model.h).
  void apply_reset(Eg2ResetCause cause);
  // Asked by the core when it executes WFI: whether pwrmgr starts a low-power entry (the core then
  // sleeps until pwrmgr wakes the chip through a reset).
  std::function<bool()> low_power_requested;

  // ---- Verdict ----
  void finish(bool passed, const std::string &why) {
    if (!finished_) {
      finished_ = true;
      passed_ = passed;
      verdict_ = why;
    }
  }
  bool finished() const { return finished_; }
  bool passed() const { return passed_; }
  const std::string &verdict() const { return verdict_; }

  // ---- Outputs ----
  std::string log_dir = ".";
  bool logs = true;
  // Also given every item, when set: the chip bench's library (dpi/eg2_sysmodel.cc) queues them
  // for its scoreboard.
  std::function<void(const std::string &, const std::string &)> on_emit;
  void emit(const std::string &instance, const std::string &line) {
    if (on_emit) on_emit(instance, line);
    if (!logs) return;
    FILE *&f = logs_[instance];
    if (!f) f = std::fopen((log_dir + "/" + instance + ".model.log").c_str(), "w");
    if (f) std::fprintf(f, "%s\n", line.c_str());
  }
  // Something the model did that a run's reader should know but that is not an output item (a byte
  // dropped on a full FIFO): one line in <log_dir>/model.notes.log, and on stderr.
  void note(const std::string &text) {
    std::fprintf(stderr, "eg2 model: %s\n", text.c_str());
    if (!logs) return;
    FILE *&f = logs_["model.notes"];
    if (!f) f = std::fopen((log_dir + "/model.notes.log").c_str(), "w");
    if (f) {
      std::fprintf(f, "%llu ps: %s\n", (unsigned long long)now_ps(), text.c_str());
      std::fflush(f);  // read while the run goes on
    }
  }
  void close_logs() {
    for (auto &l : logs_) {
      if (l.second) std::fclose(l.second);
    }
    logs_.clear();
  }

  // ---- Inputs while running ----
  // What an agent drives into the chip, as the chip receives it: a model that takes an input at
  // run time registers it here by key (uart<N>.rx: one byte from the peer). The chip bench's
  // library (dpi/eg2_sysmodel.cc) delivers the agents' items; eg2sim uses --stim instead.
  std::map<std::string, std::function<void(uint32_t)>> inputs;

  // ---- The DV log's words (set by eg2sim) ----
  std::function<void(uint32_t)> on_dv_log;

  // ---- Memories (the windows without a register block), by window name ----
  std::map<std::string, Eg2Memory *> memories;

  // ---- Instruction fetch permission ----
  void add_exec_region(const std::string &window, std::function<bool()> allowed) {
    exec_.push_back({window, std::move(allowed), 0, 0, false});
  }
  // Whether the controller of the memory holding addr lets the core fetch from it (true for
  // addresses no controller restricts).
  bool fetch_allowed(uint32_t addr) {
    for (ExecRegion &r : exec_) {
      if (!r.resolved) {
        auto it = memories.find(r.window);
        if (it != memories.end()) {
          r.base = it->second->base();
          r.size = it->second->size();
        }
        r.resolved = true;
      }
      if (addr >= r.base && addr - r.base < r.size) return r.allowed();
    }
    return !fetch_map || fetch_map(addr);
  }
  std::function<bool(uint32_t)> fetch_map;

  // ---- The CHERIoT lock ----
  // Called by rv_core_ibex's model when a store to CHERIOT_LOCK locks CHERIoT mode, when set: the
  // core then hands over to its CHERIoT-mode instruction-set model (eg2_spike_cpu.h). When not set,
  // the model stops the run at the lock.
  std::function<void()> on_cheriot_lock;

  // ---- The models ----
  void add(Eg2IpModel *m) { models_.push_back(m); }
  const std::vector<Eg2IpModel *> &models() const { return models_; }
  Eg2IpModel *model(const std::string &window) const;  // eg2_ip_model.h

 private:
  Eg2System() : plic_src_(eg2_irqs::kPlicSources, false) {}

  // A clock's frequency from t_ps on, and the cycles it had completed by then.
  struct ClockSeg {
    uint64_t t_ps, cycles, hz;
  };
  // Nominal sim_dv frequencies (device_sim_dv.c): main 100 MHz, io_div4 24 MHz, aon 200 kHz.
  std::vector<ClockSeg> clk_main_{{0, 0, 100000000}}, clk_io_div4_{{0, 0, 24000000}},
      clk_aon_{{0, 0, 200000}};
  std::vector<ClockSeg> &track(Clk c) {
    return c == Clk::kMain ? clk_main_ : c == Clk::kAon ? clk_aon_ : clk_io_div4_;
  }
  const std::vector<ClockSeg> &track(Clk c) const {
    return c == Clk::kMain ? clk_main_ : c == Clk::kAon ? clk_aon_ : clk_io_div4_;
  }
  static const ClockSeg &seg_at(const std::vector<ClockSeg> &tr, uint64_t t_ps) {
    size_t i = tr.size() - 1;
    while (i > 0 && tr[i].t_ps > t_ps) --i;
    return tr[i];
  }

  std::map<Eg2IpModel *, uint64_t> events_;
  std::multimap<uint64_t, std::function<void()>> timed_;
  std::vector<bool> plic_src_;
  bool reset_pending_ = false;
  Eg2ResetCause reset_cause_ = Eg2ResetCause::kPor;
  int reset_hw_req_ = -1;
  std::string reset_why_;
  bool finished_ = false, passed_ = false;
  std::string verdict_;
  std::map<std::string, FILE *> logs_;
  std::vector<Eg2IpModel *> models_;
  bool esc_[kEscSeverities] = {};
  std::vector<std::function<void(bool)>> esc_rx_[kEscSeverities];
  std::map<std::string, std::string> halts_;
  std::vector<std::string> faults_applied_;
  struct ExecRegion {
    std::string window;
    std::function<bool()> allowed;
    uint32_t base, size;
    bool resolved;
  };
  std::vector<ExecRegion> exec_;
};

#endif  // EG2_MODEL_EG2_SYSTEM_H_
