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
// takes the WFI as a low-power entry, until a model resets the chip. A chip reset
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

class Eg2SpikeCpu : public sc_core::sc_module, public simif_t {
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

 private:
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
  unsigned resets_ = 0;
  bool fetch_denied_ = false;  // this step's fetch is from a region that refuses fetches
  Eg2Iss *cheriot_iss_ = nullptr;
  bool cheriot_ = false;       // CHERIoT mode: cheriot_iss_ executes
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
  std::string stop_reason_;
};

#endif  // EG2_MODEL_EG2_SPIKE_CPU_H_
