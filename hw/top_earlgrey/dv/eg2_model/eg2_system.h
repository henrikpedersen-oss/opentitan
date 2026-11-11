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
//               the core executes nothing and lets the models run on. hold_core(source, why) holds
//               it the same way through resets, until release_core(source) (rv_dm's ndmreset).
//   Debug       debug_req: rv_dm's debug request to the core; hart: the core's side of debug
//               (Eg2DebugHart: debug mode, abstract register access, the program buffer, resume);
//               sba: rv_dm's system bus host port.
//   Faults      fault handlers by name (lockstep, cheriot_ena_low): a fault injected as an input
//               event (eg2sim --fault, eg2_sysmodel_fault), never a change to a model's behaviour;
//               fault_applied reports when it changes what the chip does.
//   Reset       request_reset(cause): the core and every model restart, as after a reset of the
//               chip; each model decides what it keeps (on_reset), after every model has seen it
//               coming (before_reset: rstmgr captures the crash dump).
//   Verdict     finish(passed, why): the run ends with the software's verdict (the DV window).
//   Outputs     emit(instance, line): one line per item into <instance>.model.log, the format of
//               the bench's <instance>.chip.log, for the agent comparisons (M6); on_emit also
//               gets each item. emit_in(instance, line): an item the chip received, in the file
//               only, marked as the bench marks it ("< " a value the core read, "(in) " one on
//               the pins).
//   Debug       debug(instance, level, text): a peripheral model's readable line for software
//               engineers (a configuration written, a transfer started or finished, an overflow,
//               an interrupt raised) in <instance>.debug.log, at periph_debug's level.
//   Inputs      inputs[key](value): an agent's item delivered while the model runs (the chip
//               bench's library, dpi/).
//   DV log      on_dv_log(word): each word software writes to the DV window's log word
//               (eg2sim decodes them, eg2_dvlog.h).
//   Execution   add_exec_region(memory window, allowed): the controller of a memory that can
//               refuse instruction fetches (sram_ctrl EXEC, rram_ctrl EXEC) registers it;
//               fetch_allowed(addr) is what the core asks before each instruction. fetch_map, when
//               set, is the instruction side's own address decode (a board whose instruction
//               crossbar reaches fewer devices than its data crossbar). addr_translate is the
//               rv_core_ibex wrapper's separate instruction/data address translation.
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
#include "eg2_rom_image.h"

class Eg2IpModel;

// Why the chip resets; rstmgr records it in RESET_INFO.
enum class Eg2ResetCause { kPor, kLowPowerExit, kSoftware, kHwRequest, kEscalation, kNdm };

// The core's side of debug, which the core model gives the debug module (Eg2System::hart): what
// the RISC-V Debug Specification 0.13.2 asks of a hart (chapter 4, "RISC-V Debug") and what rv_dm's
// execution-based debug makes the core do from its debug ROM (hw/ip/rv_dm/doc, PULP
// debug-system.md "Debug Memory"). The functions return the abstract command error the debug
// module reports (abstractcs.cmderr, spec 3.12.6): 0 none, 2 not supported, 3 exception.
class Eg2DebugHart {
 public:
  virtual ~Eg2DebugHart() = default;
  // The core is in debug mode (halted).
  virtual bool debug_mode() const = 0;
  // The core currently executes in CHERIoT mode.
  virtual bool cheriot_mode() const = 0;
  // Access Register (spec 3.6.1.1), 32 bits: regno 0x1000 + n is GPR xn, 0x0000-0x0fff a CSR.
  virtual int access_register(uint32_t regno, bool write, uint32_t *value) = 0;
  // Runs the program buffer at addr (in rv_dm's debug memory) in debug mode, until its ebreak.
  virtual int exec_progbuf(uint32_t addr) = 0;
  // Leaves debug mode as dret does: pc from dpc, privilege from dcsr.prv, single step if dcsr.step.
  virtual void resume() = 0;
};

class Eg2System {
 public:
  static constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

  static Eg2System &get() {
    static Eg2System s;
    return s;
  }

  // ---- Time ----
  std::function<uint64_t()> clock;  // set by the core model
  // lc_ctrl_function_signals_table.md ESCALATE_EN, decoded by lc_ctrl for its consumers.
  std::function<bool()> lc_escalate_en;
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
  // Ibex's irq_nm_i as a level: rv_core_ibex's NMI_STATE & NMI_ENABLE (NMI_STATE "Current state",
  // rw1c: it stays until software clears it). Ibex exception_interrupts.rst: "When handling the
  // NMI, all interrupts including the NMI are ignored. Nested NMIs are not supported": the core
  // takes it again after the handler's mret while the level stays high.
  bool nmi_level = false;

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
  // The core's crash dump as rstmgr captures it (rv_core_ibex theory_of_operation.md "Crash Dump
  // Collection"): mtval, mepc, the last data access address, the next PC, the current PC, the
  // previous exception's mtval and mepc, and the previous state's valid bit (8 words).
  std::function<std::vector<uint32_t>()> cpu_crashdump;

  // ---- Core halt (instruction fetch disabled) ----
  void halt_core(const std::string &source, const std::string &why) {
    auto old = halts_.find(source);
    if (old != halts_.end() && old->second == why) return;
    halts_[source] = why;
    note("core halted by " + source + ": " + why);
  }
  bool core_halted() const { return !halts_.empty() || !holds_.empty(); }
  std::string halt_reason() const {
    std::string r;
    for (auto &h : halts_) r += (r.empty() ? "" : "; ") + h.first + ": " + h.second;
    for (auto &h : holds_) r += (r.empty() ? "" : "; ") + h.first + ": " + h.second;
    return r;
  }
  // A hold lasts through resets until its source releases it (a reset held asserted).
  void hold_core(const std::string &source, const std::string &why) {
    if (holds_.count(source)) return;
    holds_[source] = why;
    note("core held by " + source + ": " + why);
  }
  void release_core(const std::string &source) {
    if (holds_.erase(source)) note("core released by " + source);
  }
  void release_halt(const std::string &source) {
    if (halts_.erase(source)) note("core halt released by " + source);
  }

  // ---- Debug ----
  // rv_dm's debug_req_o (a level): the core enters debug mode at its next instruction boundary,
  // also from WFI (Ibex integration.rst: a debug request wakes the core).
  bool debug_req = false;
  Eg2DebugHart *hart = nullptr;  // set by the core model
  // rv_dm's system bus host (sba_tl_h, on xbar_main), set by the board: false for an error response.
  std::function<bool(bool write, uint32_t addr, uint32_t len, uint8_t *bytes)> sba;

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
  // Stimulus applied while the model runs (eg2sim --stim <key>=<value>@<when>): what a chip-bench
  // sequence drives into the chip at a point of the test, an input event, never a change of the
  // model. A model registers a handler for each key it takes; the handler applies the value now
  // and returns false for a value it cannot take.
  std::map<std::string, std::function<bool(const std::string &)>> stim_handlers;
  // The test image's symbols at run time, as the bench's sw_symbol_backdoor_read / _overwrite
  // reach them (set by eg2sim; false for a symbol the image does not have or no memory holds).
  std::function<bool(const std::string &, uint8_t *, size_t)> read_test_symbol;
  std::function<bool(const std::string &, const uint8_t *, size_t)> write_test_symbol;
  bool has_lc_destination = false;
  uint8_t lc_destination = 0;
  bool apply_stim(const std::string &key, const std::string &value) {
    auto it = stim_handlers.find(key);
    return it != stim_handlers.end() && it->second(value);
  }
  // A point of the run a chip-bench sequence waits for, as the chip shows it: "wfi" (the core
  // goes to sleep on a WFI, Ibex's core_sleep_o) and "lowpower" (pwrmgr starts a low-power entry,
  // the bench's pwrmgr_low_power_if). Set by eg2sim for its triggered stimulus; a stimulus applied
  // from here takes effect during the event (pwrmgr's entry has not finished).
  std::function<void(const std::string &)> on_chip_event;
  void chip_event(const std::string &what) {
    if (on_chip_event) on_chip_event(what);
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
  // hw_req: for kHwRequest, the request's index in rstmgr's RESET_INFO.HW_REQ. in_low_power: the
  // request came while the chip was in low power, so rstmgr records the low-power exit too (rstmgr
  // theory_of_operation.md "Reset Information": all allowed causes are recorded, "a security
  // escalation during low power transition for example").
  void request_reset(Eg2ResetCause cause, const std::string &why, int hw_req = -1,
                     bool in_low_power = false) {
    if (!reset_pending_) {
      reset_pending_ = true;
      reset_cause_ = cause;
      reset_why_ = why;
      reset_hw_req_ = hw_req;
      reset_in_low_power_ = in_low_power;
    }
  }
  int reset_hw_req() const { return reset_hw_req_; }
  bool reset_in_low_power() const { return reset_in_low_power_; }
  bool reset_pending() const { return reset_pending_; }
  Eg2ResetCause reset_cause() const { return reset_cause_; }
  const std::string &reset_why() const { return reset_why_; }
  // The reset has been applied: fetch is enabled again (each halt's source restarts in reset), the
  // escalation receivers have reset with their senders.
  void reset_done() {
    reset_pending_ = false;
  }
  // Every model's before_reset, then every model's on_reset (eg2_ip_model.h).
  void apply_reset(Eg2ResetCause cause);
  // Asked by the core when it executes WFI: whether pwrmgr starts a low-power entry. The core then
  // sleeps until pwrmgr wakes the chip: through a reset from deep sleep, or by leaving low power
  // from normal sleep (in_low_power false), when the core continues after the WFI.
  std::function<bool()> low_power_requested;
  std::function<bool()> in_low_power;
  // The core left WFI because it saw an interrupt. pwrmgr theory_of_operation.md "Fall Through
  // Handling": "an interrupt is shown to the processor, thus breaking it out of its currently
  // stopped state" and pwrmgr then "returns the system to active state".
  std::function<void()> cpu_interrupt_wakeup;
  // rv_core_ibex.cfg MCOUNTEREN_WRITABLE is MuBi4True (the core's mcounteren_writable_i): software
  // writes to mcounteren take effect only then (the hjson). Unset: writable.
  std::function<bool()> mcounteren_writable;
  // The non-volatile memory controller's RMA operation, requested by lc_ctrl before it programs
  // the RMA state. False reports a failed wipe.
  std::function<bool()> nvm_rma;
  std::function<bool()> lc_hw_debug_en;
  // clkmgr has the clock of the block (by module name) off: an access to it never completes.
  std::function<bool(const std::string &block)> clock_off;
  // rstmgr holds the block (by instance name) in software reset: an access to it never completes.
  std::function<bool(const std::string &block)> held_in_reset;

  // ---- Console, the core's bitmap port, bus integrity ----
  // Each complete line a UART sends (its window, the line without the end of line), set by the
  // bench steps (eg2_bench.h).
  std::function<void(const std::string &window, const std::string &line)> on_console_line;
  // The core's TRVK revocation-bitmap lookup over cheriot_mem_sys.corerevbm_tl: the bitmap byte at
  // `offset`; false for a failed lookup (an error or integrity fault on the response), which
  // revokes. Set by the cheriot_mem_sys model; the CHERIoT-mode core asks it for each lookup.
  std::function<bool(uint32_t offset, uint8_t *byte)> core_revbm;
  // A bus integrity error the core's bus interface sees on a response (rv_core_ibex interfaces.md,
  // fatal_hw_err: "A bus integrity error is seen"): ERR_STATUS.FATAL_INTG_ERR, fatal_hw_err, fetch
  // disabled. Set by the rv_core_ibex model.
  std::function<void(const std::string &why)> core_bus_intg_error;
  // A bus word whose read answers with a TL-UL error response (d_error), recorded by the model that
  // owns it (a relocated RRAM row: rram_ctrl theory_of_operation.md "Address-XOR"). The core asks
  // read_error(addr, false) before a load and calls read_error(addr, true) when it takes the error
  // (a load access fault).
  std::function<bool(uint32_t addr, bool take)> read_error;
  // Whether the memory at `addr` refuses the access (Eg2Memory::refuses: a write to a read-only
  // window, any access to an error range). Set by eg2sim; a core model that stores through a
  // direct pointer (Spike) asks it before a store and takes the bus path instead, so the store
  // faults as the chip's d_error gives.
  std::function<bool(uint32_t addr, bool write)> bus_refuses;
  // A double fault (Ibex double_fault_seen_o; rv_core_ibex interfaces.md fatal_hw_err "A double
  // fault is seen"): fatal_hw_err, fetch disabled. Set by the rv_core_ibex model.
  std::function<void(const std::string &why)> core_double_fault;

  // ---- Bench steps (eg2_bench.h) ----
  // What the chip bench does for a test, done by the model (EG2 BENCH lines; Henrik 2026-11-04:
  // the model is self-contained). An op's handler starts it; the step is answered by bench_ack(n,
  // result), at once or when the op is complete. bench_check counts a check the bench makes
  // (EG2CHK ok / FAIL lines); a failed one fails the run's verdict.
  std::map<std::string, std::function<void(uint32_t n, uint32_t a, uint32_t b)>> bench_ops;
  std::function<void(uint32_t n, uint32_t result)> bench_ack;
  std::function<bool(const std::string &what, bool ok, const std::string &detail)> bench_check;
  // A must-fail run (+eg2_inject_fail=1 on the chip; --stim bench.inject_fail=1 here): the bench
  // inverts its check the image's build broke.
  bool bench_inject_fail = false;
  // The bench ends the run with its own verdict (the core's fetch is off, the software cannot).
  std::function<void(const std::string &why)> bench_end;
  // Each message the software logs (the DV log, decoded), for the bench's line-driven steps (the
  // chip bench's sw_logger_vif lines).
  std::vector<std::function<void(const std::string &)>> sw_log_watchers;
  // ---- Host programs (ip/eg2_test_hosts.h) ----
  // Each test status word the software writes, after the stimulus points have seen it.
  std::vector<std::function<void(uint32_t)>> sw_status_watchers;
  // A host program that boots the chip several times (it resets the chip between its phases, as
  // the FPGA host does) takes a pass status that is not its last: true means that status does not
  // end the run.
  std::function<bool(uint32_t)> host_takes_status;
  // The address of a test-image symbol as the running software sees it (its VMA: the .data copy in
  // SRAM, not the load image), as a host's ELF lookup gives it.
  std::function<bool(const std::string &, uint32_t *)> test_symbol_addr;
  // The memories at a bus address while the model runs (a host's backdoor read or write).
  std::function<bool(bool write, uint32_t addr, uint8_t *bytes, size_t n)> mem_rw;
  // A parameter written into the test image (kEg2Param_<name>), as the bench's set_param.
  bool set_test_param(const std::string &name, uint32_t v) {
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    return write_test_symbol && write_test_symbol("kEg2Param_" + name, b, 4);
  }

  // ---- Verdict ----
  void finish(bool passed, const std::string &why) {
    if (!finished_) {
      if (passed) {
        for (const auto &check : verdict_checks) {
          std::string failure = check();
          if (!failure.empty()) {
            finished_ = true;
            passed_ = false;
            verdict_ = failure;
            return;
          }
        }
      }
      finished_ = true;
      passed_ = passed;
      verdict_ = why;
    }
  }
  bool finished() const { return finished_; }
  bool passed() const { return passed_; }
  const std::string &verdict() const { return verdict_; }
  // Bench checks that must have completed before either software or a bench stimulus may pass the
  // run. An empty result means complete; otherwise it is the failing verdict.
  std::vector<std::function<std::string()>> verdict_checks;

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
  // An item the chip received, in <instance>.model.log only: on_emit gets only what the chip sends
  // (the chip bench compares those). As in the bench's <instance>.chip.log, line starts with "< "
  // for a value the core read from the peripheral, "(in) " for one on the pins.
  void emit_in(const std::string &instance, const std::string &line) {
    if (!logs) return;
    FILE *&f = logs_[instance];
    if (!f) f = std::fopen((log_dir + "/" + instance + ".model.log").c_str(), "w");
    if (f) std::fprintf(f, "%s\n", line.c_str());
  }
  // ---- Peripheral debug lines ----
  // One readable line per significant software action or device event of a peripheral model, in
  // <log_dir>/<instance>.debug.log: "<time> us <instance>: <text>". periph_debug 1 (the default):
  // configuration, transfers, overflows, underflows and interrupts, one line each; 2: also every
  // byte and FIFO access; 0: none. periph_debug_stderr: each line also on stderr. eg2sim
  // --periph-debug <level>[,stderr].
  int periph_debug = 1;
  bool periph_debug_stderr = false;
  bool debug_on(int level) const { return level <= periph_debug; }
  void debug(const std::string &instance, int level, const std::string &text) {
    if (!debug_on(level)) return;
    char t[32];
    std::snprintf(t, sizeof t, "%12.3f us", double(now_ps()) / 1e6);
    if (periph_debug_stderr) std::fprintf(stderr, "%s %s: %s\n", t, instance.c_str(), text.c_str());
    if (!logs) return;
    FILE *&f = logs_[instance + ".debug"];
    if (!f) {
      f = std::fopen((log_dir + "/" + instance + ".debug.log").c_str(), "w");
      if (f) std::setvbuf(f, nullptr, _IOLBF, 0);  // read while the run goes on
    }
    if (f) std::fprintf(f, "%s %s: %s\n", t, instance.c_str(), text.c_str());
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

  // ---- The DV log's words and the test status words (set by eg2sim) ----
  std::function<void(uint32_t)> on_dv_log;
  std::function<void(uint32_t)> on_dv_status;  // each test status word (the bench's sw_test_status)

  // ---- Memories (the windows without a register block), by window name ----
  std::map<std::string, Eg2Memory *> memories;
  eg2_model::Eg2RomImage rom_image;  // independent stored checker words and configuration

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
  std::function<uint32_t(uint32_t, bool)> addr_translate;
  std::function<void()> addr_translation_changed;

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
  bool reset_in_low_power_ = false;
  std::string reset_why_;
  bool finished_ = false, passed_ = false;
  std::string verdict_;
  std::map<std::string, FILE *> logs_;
  std::vector<Eg2IpModel *> models_;
  bool esc_[kEscSeverities] = {};
  std::vector<std::function<void(bool)>> esc_rx_[kEscSeverities];
  std::map<std::string, std::string> halts_, holds_;
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
