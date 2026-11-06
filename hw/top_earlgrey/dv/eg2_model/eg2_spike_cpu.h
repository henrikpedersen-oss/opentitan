// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Spike as a TLM-2.0 initiator of the EG2 SystemC model: the RISC-V-mode instruction-set model
// (doc/system_model in eg2_top_verification). It implements Spike's simif_t: memories are reached
// through DMI pointers (Spike's fast path, addr_to_mem), every other access through b_transport
// on the bus. It starts from rv_core_ibex's reset state (Ibex configuration, PMP and mseccfg reset
// values) and runs loosely timed: one instruction per CPU clock cycle (sim_dv's 100 MHz), in
// quanta of kQuantum instructions.
//
// Milestone M3: the core reads its interrupt lines (external from rv_plic, timer from rv_timer,
// software from rv_plic's MSIP, NMI) from Eg2System before every instruction. Spike executes WFI as
// a no-op, so the core sleeps itself after one: it advances time to the next model event until an
// enabled interrupt is pending (Ibex wakes on mip & mie, whatever mstatus.MIE), or, when pwrmgr
// takes the WFI as a low-power entry, until a wake-up/reset or an interrupt returns normal sleep
// to active. A chip reset
// (Eg2System::request_reset) restarts the core from its reset state.
//
// Milestone M4: CHERIoT mode. With a CHERIoT-mode instruction-set model given (set_cheriot_iss,
// Eg2Iss: MPACT-CHERIoT), the core hands Spike's state over to it (REQ_SOC_07) after the store to
// CHERIOT_LOCK that locks CHERIoT mode, and steps it instead of Spike until the next chip reset,
// which restarts Spike in RISC-V mode. Without one, rv_core_ibex's model stops the run at the lock.
//
// A core in CHERIoT mode from reset (Eg2CoreConfig::cheriot_at_reset: another board, whose core has
// cheriot_enable_i tied on) never runs Spike: at start-up and after every reset the CHERIoT-mode
// model starts from the core's CHERIoT reset state (cheriot_reset; Eg2Iss::handover with it).
//
// A halted core (Eg2System::halt_core: fetch disabled by rv_core_ibex's fatal_hw_err or lc_ctrl's
// lc_cpu_en) executes nothing and takes no interrupt; time runs on to the next model event (an
// escalation phase, the reset it ends with). If no model will ever act, the run stops, naming the
// halt.
//
// Debug mode (Eg2DebugHart, for rv_dm's model; Ibex debug.rst, RISC-V Debug Specification 0.13.2
// chapter 4): rv_dm's debug request (Eg2System::debug_req) takes the core into debug mode at the
// next instruction boundary, also out of WFI: dpc gets the next instruction's address, dcsr the
// cause (3, halt request) and the privilege it ran in, and the core runs in M mode from rv_dm's
// halt address (debug ROM + 0x800, PULP debug-system.md "Debug Memory Map"). Spike enters debug
// mode itself on an ebreak with dcsr.ebreakm/ebreaku set (cause 1), after a single step (dcsr.step,
// cause 4) and on a trigger whose action is debug mode (cause 2). In debug mode the core waits in
// the debug ROM's park loop for the debugger: the model does not run the ROM's code (its contents
// are rv_dm's implementation, which no document gives) but what the debug module asks of the core,
// in debug mode with interrupts masked (exception_interrupts.rst): an abstract register access
// (read or write a GPR or CSR), the program buffer (executed by Spike from rv_dm's debug memory
// until its ebreak; an exception ends it with cmderr 3), and the resume (dret's effect). Time runs on
// as for a halted core. In CHERIoT mode MPACT-CHERIoT has no debug mode: the core stops stepping it
// and keeps dpc (its PC) and dcsr itself; GPR reads give MPACT's integer value; register writes,
// other registers and the program buffer answer "not supported" (cmderr 2), and the resume goes
// on at MPACT's PC.
//
// Faults (add_fault; eg2sim --fault <name>[@<when>]): each is injected once, through
// Eg2System::inject_fault, before the first instruction (no when), before the instruction whose
// index (instructions retired so far) is when, or before the first instruction at PC when. The
// core's own fault is cheriot_ena_low: its cheriot_enable_i forced to MuBi4False from then on (the
// switch and cheriot_mem_sys still see the mode the switch locked). A CHERIoT lock then leaves the
// core in RISC-V mode (Spike); if the core is already in CHERIoT mode, it goes back to Spike from
// the CHERIoT-mode model's state (Eg2Iss::take_back). The force lasts through resets.

#ifndef EG2_MODEL_EG2_SPIKE_CPU_H_
#define EG2_MODEL_EG2_SPIKE_CPU_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

#include "eg2_iss.h"
#include "eg2_system.h"
#include "riscv/devices.h"
#include "riscv/log_file.h"
#include "riscv/processor.h"
#include "riscv/simif.h"

struct Eg2CoreConfig {
  std::string isa;
  uint32_t boot_addr;
  uint32_t pmp_num_regions;
  uint32_t pmp_granularity;
  uint32_t mhpm_counter_num;
  bool secure_ibex;
  bool icache;
  uint32_t dm_start, dm_end;
  // rv_dm's halt address, where the core waits in debug mode (Ibex DmHaltAddr); 0: no debug module.
  uint32_t dm_halt_addr = 0;
  // PMP and mseccfg reset values (rv_core_ibex's PMPRstCfg/PMPRstAddr/PMPRstMsecCfg).
  uint8_t pmpcfg_rst[16];
  uint32_t pmpaddr_rst[16];
  uint32_t mseccfg_rst;
  uint64_t clock_hz;  // CPU clock: one instruction per cycle
  // CHERIoT mode from reset: the CHERIoT-mode model's state at reset (x1-x15 0: every register
  // null).
  bool cheriot_at_reset = false;
  Eg2Handover cheriot_reset{};
};

class Eg2SpikeCpu : public sc_core::sc_module, public simif_t, public Eg2DebugHart {
 public:
  tlm_utils::simple_initiator_socket<Eg2SpikeCpu> socket;

  Eg2SpikeCpu(sc_core::sc_module_name name, const Eg2CoreConfig &cfg, uint64_t max_insns,
              const std::string &log_path);
  ~Eg2SpikeCpu() override;

  uint64_t retired() const { return retired_; }
  uint32_t pc() const {
    return cheriot_ ? cheriot_iss_->pc() : uint32_t(processor_->get_state()->pc);
  }
  // The CHERIoT-mode instruction-set model the core switches to at the CHERIoT lock.
  void set_cheriot_iss(Eg2Iss *iss);
  bool cheriot() const { return cheriot_; }
  // A transaction on the bus, through the core's initiator socket: false for an error response.
  bool transport(tlm::tlm_command cmd, reg_t addr, size_t len, uint8_t *bytes);
  // The core's local time: SystemC time plus the instructions run since the last wait().
  uint64_t now_ps() const;
  // Set by any device to end the run after the current instruction.
  void request_stop(const std::string &why) {
    if (stop_reason_.empty()) stop_reason_ = why;
  }
  const std::string &stop_reason() const { return stop_reason_; }
  unsigned resets() const { return resets_; }
  // A fault to inject: before the first instruction (kAtStart), before instruction `value`
  // (kAtCount), or at the first instruction at PC `value` (kAtPc).
  enum FaultWhen { kAtStart, kAtCount, kAtPc };
  void add_fault(const std::string &name, FaultWhen when, uint64_t value) {
    faults_.push_back({name, when, value});
  }

  // simif_t
  char *addr_to_mem(reg_t addr) override;
  bool mmio_load(reg_t addr, size_t len, uint8_t *bytes) override;
  bool mmio_store(reg_t addr, size_t len, const uint8_t *bytes) override;
  void proc_reset(unsigned id) override {}
  const char *get_symbol(uint64_t addr) override { return nullptr; }

  // Eg2DebugHart
  bool debug_mode() const override { return debug_; }
  bool cheriot_mode() const override { return cheriot_; }
  int access_register(uint32_t regno, bool write, uint32_t *value) override;
  int exec_progbuf(uint32_t addr) override;
  void resume() override;

 private:
  void enter_debug(unsigned cause);
  void parked_in_debug_rom();
  bool debug_wait();
  bool ibex_csr_exists(uint32_t csr) const;
  bool ibex_csr_read_only(uint32_t csr) const;
  uint32_t ibex_csr_read(uint32_t csr);
  void ibex_csr_write(uint32_t csr, uint32_t value);
  void run();
  void ibex_reset();
  void drive_irqs();
  bool interrupt_pending();
  bool sleep(uint32_t wfi_pc);
  void handover();
  void cheriot_reset();
  bool step_spike();
  bool step_cheriot();
  void inject_due_faults();
  bool halted_wait();
  void cheriot_ena_low();
  void leave_cheriot();

  Eg2CoreConfig cfg_;
  std::unique_ptr<isa_parser_t> isa_parser_;
  std::unique_ptr<log_file_t> log_;
  std::unique_ptr<processor_t> processor_;
  std::vector<tlm::tlm_dmi> dmi_;
  uint64_t max_insns_;
  uint64_t retired_ = 0;
  uint64_t ps_per_insn_;
  uint64_t local_ps_ = 0;  // time ahead of SystemC time
  uint64_t debug_cycle_remainder_ps_ = 0;
  unsigned resets_ = 0;
  bool fetch_denied_ = false;
  bool read_denied_ = false;  // this step's load reads a word that answers with a bus error
  bool spike_in_nmi_ = false;  // an NMI handler runs on Spike: no NMI until its mret  // this step's fetch is from a region that refuses fetches
  bool translation_used_ = false;  // this step cached a translated instruction or data address
  uint32_t dump_pc_ = 0, dump_npc_ = 0, dump_daddr_ = 0;  // the crash dump's PCs and data address
  // The exception state Ibex latches at a trap (mtval, mepc), the previous one after a double fault
  // (a synchronous exception before the handler's mret), and whether a handler is running.
  uint32_t dump_mtval_ = 0, dump_mepc_ = 0, dump_prev_mtval_ = 0, dump_prev_mepc_ = 0;
  bool dump_prev_valid_ = false, in_handler_ = false, dump_clear_ = false;
  Eg2Iss *cheriot_iss_ = nullptr;
  bool cheriot_ = false;       // CHERIoT mode: cheriot_iss_ executes
  // The crash dump (rv_core_ibex's cpu_info words) as of now, and the copy taken at a reset.
  std::vector<uint32_t> crash_dump();
  std::vector<uint32_t> dump_snapshot_;
  bool dump_frozen_ = false;
  bool lock_pending_ = false;  // the current Spike step locked CHERIoT mode
  uint32_t key_wait_ = 0;     // instructions until the icache key is valid again (kIcKeyRenewInsns)
  bool ena_low_ = false;       // fault cheriot_ena_low: cheriot_enable_i forced to MuBi4False
  struct PendingFault {
    std::string name;
    FaultWhen when;
    uint64_t value;
  };
  std::vector<PendingFault> faults_;
  uint64_t halted_events_ = 0;
  bool debug_ = false;  // in debug mode
  // In CHERIoT mode, which MPACT has no debug mode for: dpc and dcsr kept here.
  uint32_t cheriot_dpc_ = 0, cheriot_dcsr_ = 0;
  uint32_t cheriot_mshwm_ = 0, cheriot_mshwmb_ = 0;
  std::string stop_reason_;
};

#endif  // EG2_MODEL_EG2_SPIKE_CPU_H_
