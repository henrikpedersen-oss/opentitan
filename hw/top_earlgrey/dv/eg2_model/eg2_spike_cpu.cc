// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_spike_cpu.h"

#include "eg2_ip_model.h"
#include "eg2_system.h"
#include "eg2_targets.h"

#include <cstdio>

#include "riscv/mmu.h"

namespace {
// Ibex's marchid (ibex_pkg CSR_MARCHID_VALUE), as Ibex's co-simulation sets it.
constexpr uint32_t kIbexMarchid = 22;
// Instructions per wait(): the loosely-timed quantum.
constexpr uint64_t kQuantum = 1000;
constexpr uint32_t kWfi = 0x10500073;
// Model events a sleeping core sits through before the run is declared stuck.
constexpr uint64_t kMaxSleepEvents = 1000000;
// Instructions a program buffer may run before the model gives up on its ebreak (the program
// buffer holds 8 words, rv_dm.hjson PROGRAM_BUFFER; a loop in it would keep abstractcs.busy set).
constexpr unsigned kProgbufMaxInsns = 1000;

// Ibex performance_counters.rst, "Controlling the counters from software": mcountinhibit bits 0
// and 2 control mcycle and minstret, and bit X controls each implemented mhpmcounterX.
// This Spike has no mcountinhibit of its own. The CSR stores Ibex's bits; the debug-mode time path
// below applies CY while ordinary Spike execution retains Spike's counter behaviour.
class IbexMcountinhibitCsr : public csr_t {
 public:
  IbexMcountinhibitCsr(processor_t *processor, uint32_t mhpm_counter_num)
      : csr_t(processor, CSR_MCOUNTINHIBIT),
        mask_(0x5 | (((uint32_t{1} << mhpm_counter_num) - 1) << 3)) {}

  reg_t read() const noexcept override { return value_; }

 protected:
  bool unlogged_write(reg_t value) noexcept override {
    value_ = value & mask_;
    return true;
  }

 private:
  const uint32_t mask_;
  uint32_t value_ = 0;
};
}  // namespace

Eg2SpikeCpu::Eg2SpikeCpu(sc_core::sc_module_name name, const Eg2CoreConfig &cfg,
                         uint64_t max_insns, const std::string &log_path)
    : sc_core::sc_module(name), socket("socket"), cfg_(cfg), max_insns_(max_insns) {
  ps_per_insn_ = 1000000000000ull / (cfg_.clock_hz ? cfg_.clock_hz : 100000000ull);
  FILE *log_file = nullptr;
  if (!log_path.empty()) {
    log_ = std::make_unique<log_file_t>(log_path.c_str());
    log_file = log_->get();
  }
  isa_parser_ = std::make_unique<isa_parser_t>(cfg_.isa.c_str(), "MU");
  processor_ = std::make_unique<processor_t>(isa_parser_.get(), DEFAULT_VARCH, this, 0, false,
                                             log_file, std::cerr);
  processor_->set_pmp_num(cfg_.pmp_num_regions);
  processor_->set_mhpm_counter_num(cfg_.mhpm_counter_num);
  processor_->set_pmp_granularity(1 << (cfg_.pmp_granularity + 2));
  processor_->set_ibex_flags(cfg_.secure_ibex, cfg_.icache);
  processor_->set_debug_module_range(cfg_.dm_start, cfg_.dm_end);
  ibex_reset();
  if (log_) {
    processor_->set_debug(true);
    processor_->enable_log_commits();
  }
  Eg2System &sys = Eg2System::get();
  sys.clock = [this] { return now_ps(); };
  sys.addr_translation_changed = [this] {
    processor_->get_mmu()->flush_tlb();
    processor_->get_mmu()->flush_icache();
  };
  // rstmgr captures the dump while the chip resets: run() takes it (crash_dump()) before the core
  // is reset and leaves CHERIoT mode. In CHERIoT mode the CHERIoT-mode model gives the PCs and the
  // last data address; the exception fields read zero there (a model limit).
  sys.cpu_crashdump = [this] { return dump_frozen_ ? dump_snapshot_ : crash_dump(); };
  sys.where = [this] {
    char b[96];
    std::snprintf(b, sizeof b, "%llu instructions, pc 0x%08x, %.3f us",
                  (unsigned long long)retired_, pc(), double(now_ps()) / 1e6);
    return std::string(b);
  };
  sys.fault_handlers["cheriot_ena_low"] = [this] {
    cheriot_ena_low();
    return true;
  };
  if (cfg_.dm_halt_addr) sys.hart = this;
  SC_THREAD(run);
}

// ---- Debug mode ----

// The core enters debug mode (RISC-V Debug Specification 0.13.2, 4.1 "Debug Mode"): dpc is the
// address of the instruction that would have run next, dcsr.cause says why, dcsr.prv keeps the
// privilege; the core runs in M mode from rv_dm's halt address (Ibex debug.rst DmHaltAddr).
void Eg2SpikeCpu::enter_debug(unsigned cause) {
  halted_events_ = 0;
  if (cheriot_) {
    // Ibex debug.rst "Debug Mode in CHERIoT": PCC is saved in depcc and replaced by mtcc, as when
    // taking a trap; dpc is depcc's address. The CHERIoT ISS's interface does not give the
    // privilege it runs in, so dcsr.prv reads M (3); xdebugver is 4 (Ibex cs_registers.rst, dcsr).
    cheriot_dpc_ = cheriot_iss_->pc();
    if (cheriot_iss_->debug_enter()) cheriot_iss_->read_dpc(&cheriot_dpc_);
    cheriot_dcsr_ = (4u << 28) | ((cause & 7) << 6) | 3u;
  } else {
    state_t *s = processor_->get_state();
    s->debug_mode = true;
    s->dcsr->write_cause_and_prv(uint8_t(cause), s->prv);
    processor_->set_privilege(PRV_M);
    s->dpc->write(s->pc);
    parked_in_debug_rom();
  }
  debug_ = true;
  std::printf("eg2sim: debug mode entered (dcsr.cause %u) at %s, dpc 0x%08x\n", cause,
              Eg2System::get().where().c_str(),
              cheriot_ ? cheriot_dpc_ : uint32_t(processor_->get_state()->dpc->read()));
}

// In debug mode the core waits in rv_dm's debug ROM, from the halt address on. Spike's own debug
// ROM addresses (DEBUG_ROM_ENTRY, where it goes on entering debug mode and after an ebreak in it)
// are replaced by rv_dm's.
void Eg2SpikeCpu::parked_in_debug_rom() {
  processor_->get_state()->pc = cfg_.dm_halt_addr;
  processor_->get_mmu()->flush_tlb();
  processor_->get_mmu()->flush_icache();
}

// One step of a core in debug mode: time runs on to the next model event, which brings the
// debugger's next action. False if nothing will ever happen (the run stops).
bool Eg2SpikeCpu::debug_wait() {
  Eg2System &sys = Eg2System::get();
  uint64_t t = sys.next_event_ps();
  if (t == Eg2System::kNever || ++halted_events_ > kMaxSleepEvents) {
    request_stop("the core waits in debug mode and " +
                 (t == Eg2System::kNever ? std::string("no debugger action or model event will come")
                                         : std::to_string(kMaxSleepEvents) +
                                               " model events brought no resume"));
    return false;
  }
  uint64_t now = now_ps();
  uint64_t elapsed = t > now ? t - now : ps_per_insn_;
  local_ps_ += elapsed;
  // Ibex performance_counters.rst: mcycle is NumCycles and mcountinhibit.CY stops it. Debug mode
  // does not stop mcycle (Ibex dcsr has no stopcount field; unsupported dcsr fields read zero), so
  // account for clock cycles while the core is parked without incrementing minstret.
  if (!(uint32_t(processor_->get_csr(CSR_MCOUNTINHIBIT)) & 1)) {
    uint64_t total = debug_cycle_remainder_ps_ + elapsed;
    uint64_t cycles = total / ps_per_insn_;
    debug_cycle_remainder_ps_ = total % ps_per_insn_;
    uint64_t mcycle = uint32_t(processor_->get_csr(0xb00)) |
                      (uint64_t(uint32_t(processor_->get_csr(0xb80))) << 32);
    mcycle += cycles;
    processor_->put_csr(0xb00, uint32_t(mcycle));
    processor_->put_csr(0xb80, uint32_t(mcycle >> 32));
  }
  sys.run_due(now_ps());
  return true;
}

// Ibex cs_registers.rst "Control and Status Registers" lists the implemented CSRs. Spike also
// implements architectural CSRs that Ibex omits, so abstract commands must use Ibex's list rather
// than treating every Spike CSR as present.
bool Eg2SpikeCpu::ibex_csr_exists(uint32_t c) const {
  auto in = [c](uint32_t lo, uint32_t hi) { return c >= lo && c <= hi; };
  if (c == 0x300 || c == 0x301 || in(0x304, 0x306) || c == 0x320 || in(0x340, 0x344) ||
      in(0x3a0, 0x3a3) || in(0x3b0, 0x3bf) || c == 0x5a8 || c == 0x747 || c == 0x757 ||
      in(0x7a0, 0x7a3) || c == 0x7a8 || c == 0x7aa || in(0x7b0, 0x7b3) || c == 0x7c0 ||
      c == 0x7c1 || in(0xf11, 0xf14)) {
    return true;
  }
  auto counter = [c](uint32_t base) {
    return c == base || c == base + 2 || (c >= base + 3 && c <= base + 31);
  };
  if (counter(0xb00) || counter(0xb80) || counter(0xc00) || counter(0xc80)) return true;
  return c >= 0x323 && c <= 0x33f;
}

bool Eg2SpikeCpu::ibex_csr_read_only(uint32_t c) const {
  // RISC-V CSR encoding marks bits 11:10 == 3 read-only. Ibex cs_registers.rst additionally lists
  // mip as read-only; mhpmevent3-31 are WARL, so writes to their hardwired values are legal.
  return ((c >> 10) & 3) == 3 || c == 0x344;
}

uint32_t Eg2SpikeCpu::ibex_csr_read(uint32_t c) {
  // Ibex performance_counters.rst: mhpmcounter3-31 exist, unavailable counters always read 0,
  // and event selectors beyond the configured hardwired set are tied to 0.
  auto unconfigured_counter = [this, c](uint32_t base) {
    return c >= base + 3 + cfg_.mhpm_counter_num && c <= base + 31;
  };
  if (unconfigured_counter(0xb00) || unconfigured_counter(0xb80) ||
      unconfigured_counter(0xc00) || unconfigured_counter(0xc80) ||
      (c >= 0x323 + cfg_.mhpm_counter_num && c <= 0x33f)) {
    return 0;
  }
  uint32_t v = uint32_t(processor_->get_csr(int(c)));
  if (c == CSR_DCSR) {
    // Ibex cs_registers.rst dcsr: only xdebugver, ebreakm, ebreaku, cause, step and prv exist;
    // unsupported fields, including stopcount and stoptime, read zero.
    v &= 0xf00091c7u;
  } else if (c == CSR_CPUCTRLSTS) {
    // Ibex cs_registers.rst cpuctrlsts: bits 0-8 are implemented; ic_scr_key_valid (bit 8) is RO.
    v = (v & 0xffu) | (key_wait_ ? 0u : 0x100u);
  }
  return v;
}

void Eg2SpikeCpu::ibex_csr_write(uint32_t c, uint32_t v) {
  Eg2System &sys = Eg2System::get();
  // Ibex performance_counters.rst: unavailable counters are tied to 0 and event selectors are
  // hardwired. Their WARL writes are accepted and legalised back to those documented values.
  auto unconfigured_counter = [this, c](uint32_t base) {
    return c >= base + 3 + cfg_.mhpm_counter_num && c <= base + 31;
  };
  if (unconfigured_counter(0xb00) || unconfigured_counter(0xb80) ||
      (c >= 0x323 && c <= 0x33f)) {
    return;
  }
  if (c == CSR_MCOUNTEREN && sys.mcounteren_writable && !sys.mcounteren_writable()) {
    // rv_core_ibex MCOUNTEREN_WRITABLE: unless MuBi4True, writes are ignored.
    return;
  }
  if (c == CSR_DCSR) {
    uint32_t old = uint32_t(processor_->get_csr(CSR_DCSR));
    v = (old & 0xf00001c0u) | (v & 0x00009007u);
  } else if (c == CSR_CPUCTRLSTS) {
    v &= 0xffu;  // ic_scr_key_valid is supplied by the model and is not writable.
  }
  processor_->put_csr(int(c), v);
}

// Access Register (spec 3.6.1.1): what rv_dm's abstract command does through the core. A GPR or a
// CSR the core has; any other register is an exception (cmderr 3), as the core's access to a CSR
// it does not have is (an illegal instruction in the abstract command's code).
int Eg2SpikeCpu::access_register(uint32_t regno, bool write, uint32_t *value) {
  if (cheriot_) {
    if (!write && regno == CSR_DPC) return *value = cheriot_dpc_, 0;
    // dpc written: depcc's address (eg2_mpact_set_dpc; the metadata rule is not documented).
    if (write && regno == CSR_DPC) {
      if (!cheriot_iss_->set_dpc(*value)) return 2;
      cheriot_iss_->read_dpc(&cheriot_dpc_);
      return 0;
    }
    if (!write && regno == CSR_DCSR) return *value = cheriot_dcsr_, 0;
    if (regno == 0xbc1 || regno == 0xbc2) {
      // Ibex cs_registers.rst: mshwm and mshwmb are CHERIoT-only WARL CSRs whose low four bits
      // are zero. Assumption: an abstract debug access has PCC.PERMIT_ACCESS_SYSTEM_REGISTERS;
      // the CHERIoT ISS interface does not expose that permission for this check.
      uint32_t &r = regno == 0xbc1 ? cheriot_mshwm_ : cheriot_mshwmb_;
      if (write) r = *value & ~0xfu;
      else *value = r;
      return 0;
    }
    if (regno >= 0x1010 && regno < 0x1020) return 3;  // RV32E in CHERIoT mode
    if (!write && regno >= 0x1000 && regno < 0x1010) {
      return cheriot_iss_->read_gpr(regno - 0x1000, value) ? 0 : 2;
    }
    // A 32-bit write of xn: an integer write (Ibex cheriot.rst: the capability portion is
    // "untagged by integer writes"); x0 ignores it, as in RISC-V mode.
    if (write && regno == 0x1000) return 0;
    if (write && regno > 0x1000 && regno < 0x1010) {
      return cheriot_iss_->write_gpr(regno - 0x1000, *value) ? 0 : 2;
    }
    return 2;  // other CSRs, and a capability's metadata or tag: not reached (ticket)
  }
  state_t *s = processor_->get_state();
  if (regno >= 0x1000 && regno < 0x1020) {
    unsigned r = regno - 0x1000;
    if (!write) *value = uint32_t(s->XPR[r]);
    else if (r) s->XPR.write(r, reg_t(int64_t(int32_t(*value))));  // Spike keeps RV32 sign-extended
    return 0;
  }
  if (regno >= 0x1000) return 3;
  if (!ibex_csr_exists(regno) || (write && ibex_csr_read_only(regno))) return 3;
  try {
    if (!write) {
      *value = ibex_csr_read(regno);
    } else {
      ibex_csr_write(regno, *value);
      processor_->get_mmu()->flush_tlb();  // a PMP change
    }
  } catch (trap_t &) {
    return 3;
  }
  return 0;
}

// The program buffer: Spike runs it in debug mode from addr (rv_dm's debug memory, which PMP lets
// the core reach in debug mode: Ibex pmp.rst "Debug Mode") until its ebreak, which in debug mode
// sends Spike to its own DEBUG_ROM_ENTRY; an exception sends it to DEBUG_ROM_TVEC, which rv_dm's ROM
// reports as cmderr 3 (rv_dm.hjson EXCEPTION). Then the core is back in the park loop.
int Eg2SpikeCpu::exec_progbuf(uint32_t addr) {
  if (cheriot_) return 2;
  state_t *s = processor_->get_state();
  s->pc = addr;
  processor_->get_mmu()->flush_icache();  // the program buffer changes between commands
  int err = 3;
  for (unsigned i = 0; i < kProgbufMaxInsns; ++i) {
    uint32_t pc = uint32_t(s->pc), insn = 0;
    if (char *p = addr_to_mem(pc)) std::memcpy(&insn, p, sizeof insn);
    unsigned f3 = (insn >> 12) & 7;
    bool csr_insn = (insn & 0x7f) == 0x73 && f3 != 0 && f3 != 4;
    uint32_t csr = insn >> 20, src = (insn >> 15) & 31;
    bool csr_write = csr_insn && (f3 == 1 || f3 == 5 || src != 0);
    // Ibex cs_registers.rst lists the implemented and read-only CSRs. Program-buffer
    // instructions obey the same illegal-CSR rule as ordinary core instructions; rv_dm's debug
    // ROM reports that exception as cmderr=3 (rv_dm.hjson EXCEPTION).
    if (csr_insn && (!ibex_csr_exists(csr) || (csr_write && ibex_csr_read_only(csr)))) break;
    bool mc_locked = csr_insn && csr == CSR_MCOUNTEREN && csr_write &&
                     Eg2System::get().mcounteren_writable &&
                     !Eg2System::get().mcounteren_writable();
    reg_t mc_old = mc_locked ? processor_->get_csr(CSR_MCOUNTEREN) : 0;
    processor_->step(1);
    if (mc_locked) processor_->put_csr(CSR_MCOUNTEREN, mc_old);
    if (csr_insn && csr_write && csr == CSR_DCSR) {
      uint32_t v = uint32_t(processor_->get_csr(CSR_DCSR));
      ibex_csr_write(CSR_DCSR, v);
    } else if (csr_insn && csr_write && csr == CSR_CPUCTRLSTS) {
      uint32_t v = uint32_t(processor_->get_csr(CSR_CPUCTRLSTS));
      ibex_csr_write(CSR_CPUCTRLSTS, v);
    }
    // Ibex cpuctrlsts.ic_scr_key_valid is supplied by the core, including to CSR reads executed
    // from the program buffer; Spike has no such bit.
    unsigned rd = (insn >> 7) & 31;
    if (csr_insn && csr == CSR_CPUCTRLSTS && rd && uint32_t(s->pc) == pc + 4) {
      uint32_t v = uint32_t(s->XPR[rd]);
      s->XPR.write(rd, (v & ~0x100u) | (key_wait_ ? 0u : 0x100u));
    }
    if (s->pc == DEBUG_ROM_ENTRY) {
      err = 0;
      break;
    }
    if (s->pc == DEBUG_ROM_TVEC) break;
    if (i + 1 == kProgbufMaxInsns) {
      Eg2System::get().note("the program buffer ran " + std::to_string(kProgbufMaxInsns) +
                            " instructions without reaching its ebreak");
      err = 1;
    }
  }
  parked_in_debug_rom();
  return err;
}

// The resume (rv_dm's ROM ends it with dret, spec 4.7): pc from dpc, the privilege from dcsr.prv,
// and a single step if dcsr.step is set.
void Eg2SpikeCpu::resume() {
  if (!debug_) return;
  debug_ = false;
  if (cheriot_) {
    // Ibex debug.rst: "dret restores PCC from depcc" (dcsr.step in CHERIoT mode: not modelled).
    bool restored = cheriot_iss_->debug_resume();
    std::printf("eg2sim: debug mode left at %s (CHERIoT mode: %s)\n",
                Eg2System::get().where().c_str(),
                restored ? "PCC restored from depcc" : "the CHERIoT ISS goes on at its pc");
    return;
  }
  state_t *s = processor_->get_state();
  s->pc = s->dpc->read();
  processor_->set_privilege(s->dcsr->prv);
  s->debug_mode = false;
  if (s->dcsr->step) s->single_step = s->STEP_STEPPING;
  processor_->get_mmu()->flush_tlb();
  processor_->get_mmu()->flush_icache();
  std::printf("eg2sim: debug mode left at %s\n", Eg2System::get().where().c_str());
}

// The fault cheriot_ena_low: from now on the core's cheriot_enable_i is MuBi4False.
void Eg2SpikeCpu::cheriot_ena_low() {
  ena_low_ = true;
  if (cheriot_) {
    leave_cheriot();
  } else {
    std::printf("eg2sim: fault cheriot_ena_low armed at %s: it applies at the CHERIoT lock\n",
                Eg2System::get().where().c_str());
  }
}

// The core in CHERIoT mode loses its CHERIoT enable: Spike goes on from the CHERIoT-mode model's
// state.
void Eg2SpikeCpu::leave_cheriot() {
  state_t *s = processor_->get_state();
  Eg2Handover h{};
  h.pc = uint32_t(s->pc);
  h.mtvec = uint32_t(processor_->get_csr(CSR_MTVEC));
  h.mepc = uint32_t(processor_->get_csr(CSR_MEPC));
  h.mstatus = uint32_t(processor_->get_csr(CSR_MSTATUS));
  h.mie = uint32_t(processor_->get_csr(CSR_MIE));
  h.mcause = uint32_t(processor_->get_csr(CSR_MCAUSE));
  h.mtval = uint32_t(processor_->get_csr(CSR_MTVAL));
  h.cpuctrlsts = uint32_t(processor_->get_csr(CSR_CPUCTRLSTS)) & 0xff;
  h.mcounteren = uint32_t(processor_->get_csr(CSR_MCOUNTEREN));
  for (int r = 1; r < 16; ++r) h.gpr[r] = uint32_t(s->XPR[r]);
  if (!cheriot_iss_->take_back(&h)) {
    request_stop(std::string("fault cheriot_ena_low: ") + cheriot_iss_->name() +
                 " cannot hand its state back to Spike");
    return;
  }
  s->pc = h.pc;
  for (int r = 1; r < 16; ++r) s->XPR.write(r, h.gpr[r]);
  for (int r = 16; r < 32; ++r) s->XPR.write(r, 0);
  processor_->put_csr(CSR_MTVEC, h.mtvec);
  processor_->put_csr(CSR_MEPC, h.mepc);
  processor_->put_csr(CSR_MSTATUS, h.mstatus);
  processor_->put_csr(CSR_MIE, h.mie);
  processor_->put_csr(CSR_MCAUSE, h.mcause);
  processor_->put_csr(CSR_MTVAL, h.mtval);
  processor_->put_csr(CSR_CPUCTRLSTS, h.cpuctrlsts);
  processor_->put_csr(CSR_MCOUNTEREN, h.mcounteren);
  processor_->get_mmu()->flush_tlb();
  processor_->get_mmu()->flush_icache();
  cheriot_ = false;
  char b[200];
  std::snprintf(b, sizeof b,
                "the core's cheriot_enable_i forced to MuBi4False in CHERIoT mode: the core goes "
                "on in RISC-V mode (Spike) from pc 0x%08x; cheriot_mem_sys still sees CHERIoT mode",
                h.pc);
  Eg2System::get().fault_applied("cheriot_ena_low", b);
}

// Every fault due before the next instruction.
void Eg2SpikeCpu::inject_due_faults() {
  for (size_t i = 0; i < faults_.size();) {
    const PendingFault &f = faults_[i];
    bool due = f.when == kAtStart || (f.when == kAtCount && retired_ >= f.value) ||
               (f.when == kAtPc && pc() == uint32_t(f.value));
    if (!due) {
      ++i;
      continue;
    }
    std::string name = f.name;
    faults_.erase(faults_.begin() + long(i));
    if (!Eg2System::get().inject_fault(name)) request_stop("unknown fault " + name);
  }
}

// One step of a halted core: time runs on to the next model event. False if nothing ever will
// happen (the run stops).
bool Eg2SpikeCpu::halted_wait() {
  Eg2System &sys = Eg2System::get();
  sys.nmi = false;  // a core that fetches nothing runs no handler
  uint64_t t = sys.next_event_ps();
  if (t == Eg2System::kNever || ++halted_events_ > kMaxSleepEvents) {
    request_stop("the core is halted (" + sys.halt_reason() + ") and " +
                 (t == Eg2System::kNever ? std::string("no model will ever reset the chip")
                                         : std::to_string(kMaxSleepEvents) +
                                               " model events did not reset the chip"));
    return false;
  }
  uint64_t now = now_ps();
  local_ps_ += t > now ? t - now : ps_per_insn_;
  sys.run_due(now_ps());
  return true;
}

Eg2SpikeCpu::~Eg2SpikeCpu() = default;

uint64_t Eg2SpikeCpu::now_ps() const {
  return uint64_t(sc_core::sc_time_stamp().to_seconds() * 1e12 + 0.5) + local_ps_;
}

// Ibex's reset state, as Ibex's co-simulation sets it up (SpikeCosim::initial_proc_setup): at
// start-up and after every reset of the chip.
void Eg2SpikeCpu::ibex_reset() {
  debug_cycle_remainder_ps_ = 0;
  // The crash dump state resets with the core, cleared at the first step after it: rstmgr captures
  // it during the reset.
  dump_clear_ = true;
  state_t *s = processor_->get_state();
  s->pc = cfg_.boot_addr + 0x80;
  s->mtvec->write(cfg_.boot_addr | 1);
  s->csrmap[CSR_MARCHID] =
      std::make_shared<const_csr_t>(processor_.get(), CSR_MARCHID, kIbexMarchid);
  s->csrmap[CSR_MCOUNTINHIBIT] =
      std::make_shared<IbexMcountinhibitCsr>(processor_.get(), cfg_.mhpm_counter_num);
  uint32_t misa = processor_->get_csr(CSR_MISA);
  if (cfg_.isa.find("Zb") != std::string::npos) misa |= 1 << 1;
  s->csrmap[CSR_MISA] = std::make_shared<const_csr_t>(processor_.get(), CSR_MISA, misa);
  processor_->set_mmu_capability(IMPL_MMU_SBARE);
  for (unsigned i = 0; i < unsigned(processor_->TM.count()); ++i) {
    processor_->TM.tdata2_write(processor_.get(), i, 0);
    processor_->TM.tdata1_write(processor_.get(), i, 0x28001048);
  }
  for (uint32_t i = 0; i < cfg_.mhpm_counter_num; i++) {
    s->csrmap[CSR_MHPMEVENT3 + i] =
        std::make_shared<const_csr_t>(processor_.get(), CSR_MHPMEVENT3 + i, 1u << (i + 3));
  }
  // PMP reset values. mseccfg first: Spike keeps RLB at 0 once any entry is locked
  // (csrs.cc mseccfg_csr_t::unlogged_write), so written last its RLB would be lost and the ROM
  // could not rewrite locked entries as it does on Ibex, which resets with RLB set. Then the
  // addresses before the configurations (a locked entry refuses address writes).
  processor_->put_csr(CSR_MSECCFG, cfg_.mseccfg_rst);
  for (int i = 0; i < 16; i++) processor_->put_csr(CSR_PMPADDR0 + i, cfg_.pmpaddr_rst[i]);
  for (int i = 0; i < 4; i++) {
    uint32_t v = 0;
    for (int b = 0; b < 4; b++) v |= uint32_t(cfg_.pmpcfg_rst[4 * i + b]) << (8 * b);
    processor_->put_csr(CSR_PMPCFG0 + i, v);
  }
  // cpuctrlsts resets to 0 on Ibex. The pinned Spike (lowRISC riscv-isa-sim 4b97396) never
  // initialises cpuctrlsts_csr_t's value (riscv/csrs.cc:1335) and its read() passes bits 6 and 7
  // (sync_exc_seen, double_fault_seen) through its mask, so they can read 1 before any trap: the
  // chip bench mismatched on it at the test ROM's first csrr cpuctrlsts (rtl_tickets.md F15).
  processor_->put_csr(CSR_CPUCTRLSTS, 0);
  // Ibex cs_registers.rst: "mseccfgh reads as all 0s and ignores all writes" with PMP. The pinned
  // Spike creates mseccfg but no mseccfgh, so the mask ROM's csrr mseccfgh trapped as illegal.
  s->csrmap[CSR_MSECCFGH] =
      std::make_shared<masked_csr_t>(processor_.get(), CSR_MSECCFGH, 0, 0);
}

void Eg2SpikeCpu::set_cheriot_iss(Eg2Iss *iss) {
  cheriot_iss_ = iss;
  if (cfg_.cheriot_at_reset) {
    cheriot_reset();
    return;
  }
  // rv_core_ibex's model reports the lock of CHERIoT mode during the store; the handover is after
  // the store's step (step_spike).
  Eg2System::get().on_cheriot_lock = [this] { lock_pending_ = true; };
}

// The handover at the CHERIoT lock (REQ_SOC_07): Spike's next PC, trap CSRs and x1-x15.
void Eg2SpikeCpu::handover() {
  state_t *s = processor_->get_state();
  Eg2Handover h{};
  h.pc = uint32_t(s->pc);
  h.mtvec = uint32_t(processor_->get_csr(CSR_MTVEC));
  h.mepc = uint32_t(processor_->get_csr(CSR_MEPC));
  h.mstatus = uint32_t(processor_->get_csr(CSR_MSTATUS));
  h.mie = uint32_t(processor_->get_csr(CSR_MIE));
  h.mcause = uint32_t(processor_->get_csr(CSR_MCAUSE));
  h.mtval = uint32_t(processor_->get_csr(CSR_MTVAL));
  // In CHERIoT mode misa has E=1, I=0 (REQ_SOC_03); the other bits are Ibex's, as in RISC-V mode.
  h.misa = (uint32_t(processor_->get_csr(CSR_MISA)) & ~(1u << 8)) | (1u << 4);
  h.cpuctrlsts = uint32_t(processor_->get_csr(CSR_CPUCTRLSTS)) & 0xff;
  h.mcounteren = uint32_t(processor_->get_csr(CSR_MCOUNTEREN));
  for (int r = 1; r < 16; ++r) h.gpr[r] = uint32_t(s->XPR[r]);
  cheriot_iss_->handover(h);
  cheriot_mshwm_ = cheriot_mshwmb_ = 0;
  cheriot_ = true;
  std::printf("eg2sim: CHERIoT mode locked: %s from pc 0x%08x after %llu instructions\n",
              cheriot_iss_->name(), h.pc, (unsigned long long)retired_);
}

// A core in CHERIoT mode from reset: the CHERIoT-mode model starts at the reset state.
void Eg2SpikeCpu::cheriot_reset() {
  cheriot_iss_->handover(cfg_.cheriot_reset);
  cheriot_mshwm_ = cheriot_mshwmb_ = 0;
  cheriot_ = true;
  std::printf("eg2sim: CHERIoT mode from reset: %s from pc 0x%08x\n", cheriot_iss_->name(),
              cfg_.cheriot_reset.pc);
}

void Eg2SpikeCpu::drive_irqs() {
  Eg2System &sys = Eg2System::get();
  if (cheriot_) {
    cheriot_iss_->set_irqs(sys.meip, sys.mtip, sys.msip);
    if (sys.nmi || sys.nmi_level) {
      if (!cheriot_iss_->nmi()) {
        request_stop("NMI in CHERIoT mode: " + std::string(cheriot_iss_->name()) +
                     " has no NMI (not modelled)");
      }
      sys.nmi = false;
    }
    return;
  }
  reg_t want = (sys.meip ? MIP_MEIP : 0) | (sys.mtip ? MIP_MTIP : 0) | (sys.msip ? MIP_MSIP : 0);
  // The Ibex fork of Spike takes interrupts from mip's pre_val (processor.h
  // take_pending_interrupt: read_pre_val() & mie), which only write_pre_val updates; Ibex's
  // co-simulation sets both (SpikeCosim::set_mip). Here both are the lines' current value.
  mip_csr_t *mip = processor_->get_state()->mip.get();
  mip->backdoor_write_with_mask(MIP_MEIP | MIP_MTIP | MIP_MSIP, want);
  mip->write_pre_val(mip->read());
  // Ibex's NMI (alert escalation, watchdog bark): Spike takes state.nmi before the next
  // instruction and clears it.
  // The line is a level (Eg2System::nmi_level): taken again after the handler's mret while it is
  // still high, ignored while the handler runs.
  if ((sys.nmi || sys.nmi_level) && !spike_in_nmi_) processor_->get_state()->nmi = true;
  sys.nmi = false;
}

std::vector<uint32_t> Eg2SpikeCpu::crash_dump() {
  if (cheriot_) {
    uint32_t pc = 0, npc = 0, daddr = 0;
    std::vector<uint32_t> d(8, 0);
    if (cheriot_iss_ && cheriot_iss_->crash_dump(&pc, &npc, &daddr)) {
      d[2] = daddr;
      d[3] = npc;
      d[4] = pc;
    }
    return d;
  }
  return std::vector<uint32_t>{dump_mtval_,      dump_mepc_,      dump_daddr_,
                               dump_npc_,        dump_pc_,        dump_prev_mtval_,
                               dump_prev_mepc_,  uint32_t(dump_prev_valid_)};
}

bool Eg2SpikeCpu::interrupt_pending() {
  if (cheriot_) return cheriot_iss_->interrupt_pending();
  state_t *s = processor_->get_state();
  return (s->mip->read() & s->mie->read()) != 0 || s->nmi;
}

// After a WFI: sleep until an enabled interrupt is pending, advancing time to the next model event.
// Returns false if nothing can ever wake the core (the run then stops).
bool Eg2SpikeCpu::sleep(uint32_t wfi_pc) {
  Eg2System &sys = Eg2System::get();
  bool low_power = sys.low_power_requested && sys.low_power_requested();
  // pwrmgr theory_of_operation.md "Fall Through Handling": only an interrupt shown right after
  // WFI can cancel entry. pwrmgr additionally checks CONTROL.CORE_CLK_EN and records
  // WAKE_INFO.FALL_THROUGH; later interrupts are not independent low-power wake-up sources.
  if (low_power) {
    drive_irqs();
    if (interrupt_pending() && sys.cpu_interrupt_wakeup) sys.cpu_interrupt_wakeup();
    if (sys.in_low_power && !sys.in_low_power()) return true;
  }
  uint64_t events = 0;
  for (;;) {
    if (sys.finished() || !Eg2ModelStop::reason().empty() || sys.reset_pending()) return true;
    drive_irqs();
    if (!low_power && (interrupt_pending() || sys.debug_req)) return true;
    // Woken from normal sleep: pwrmgr left low power without a reset; continue after the WFI.
    if (low_power && sys.in_low_power && !sys.in_low_power()) return true;
    uint64_t t = sys.next_event_ps();
    if (t == Eg2System::kNever) {
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "%s at pc 0x%08x and no model will ever wake the core",
                    low_power ? "low-power entry" : "WFI", wfi_pc);
      request_stop(buf);
      return false;
    }
    uint64_t now = now_ps();
    local_ps_ += t > now ? t - now : ps_per_insn_;
    sys.run_due(now_ps());
    if (++events > kMaxSleepEvents) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "%s at pc 0x%08x: %llu model events and nothing woke the core",
                    low_power ? "low-power entry" : "WFI", wfi_pc,
                    (unsigned long long)kMaxSleepEvents);
      request_stop(buf);
      return false;
    }
  }
}

void Eg2SpikeCpu::run() {
  Eg2System &sys = Eg2System::get();
  while (stop_reason_.empty()) {
    if (now_ps() >= sys.next_event_ps()) sys.run_due(now_ps());
    if (sys.reset_pending()) {
      // A reset of the chip: the core restarts, every model does what its IP does on this reset.
      // The chip's reset leaves CHERIoT mode (rv_core_ibex's model unlocks): Spike runs again.
      // The crash dump is the core's state now, before any of that (rstmgr reads it in
      // apply_reset).
      dump_snapshot_ = crash_dump();
      dump_frozen_ = true;
      processor_->reset();
      ibex_reset();
      dmi_.clear();
      spike_in_nmi_ = false;
      debug_ = false;  // the reset takes the core out of debug mode
      cheriot_ = false;
      lock_pending_ = false;
      halted_events_ = 0;
      if (cfg_.cheriot_at_reset && cheriot_iss_) cheriot_reset();
      std::printf("eg2sim: chip reset (%s) after %llu instructions, %.3f us\n",
                  sys.reset_why().c_str(), (unsigned long long)retired_, double(now_ps()) / 1e6);
      sys.apply_reset(sys.reset_cause());
      dump_frozen_ = false;
      sys.reset_done();
      ++resets_;
      continue;
    }
    if (!faults_.empty()) inject_due_faults();
    if (sys.core_halted()) {
      halted_wait();
    } else if (debug_) {
      debug_wait();
    } else if (sys.debug_req) {
      enter_debug(DCSR_CAUSE_DEBUGINT);  // dcsr.cause 3, halt request
    } else {
      drive_irqs();
      uint32_t pc = this->pc();
      bool wfi = cheriot_ ? step_cheriot() : step_spike();
      ++retired_;
      local_ps_ += ps_per_insn_;
      if (wfi) sleep(pc);
    }
    if (sys.finished()) request_stop(sys.verdict());
    if (!Eg2ModelStop::reason().empty()) request_stop(Eg2ModelStop::reason());
    if (max_insns_ && retired_ >= max_insns_) request_stop("instruction limit reached");
    if (local_ps_ >= kQuantum * ps_per_insn_) {
      sc_core::sc_time dt(double(local_ps_), sc_core::SC_PS);
      local_ps_ = 0;
      wait(dt);
    }
  }
  sc_core::sc_stop();
}

// One instruction on Spike; true if it was a WFI (Spike executes it as a no-op).
bool Eg2SpikeCpu::step_spike() {
  Eg2System &sys = Eg2System::get();
  uint32_t pc = uint32_t(processor_->get_state()->pc);
  uint32_t insn = 0;
  if (char *p = addr_to_mem(pc)) std::memcpy(&insn, p, 4);
  // A memory whose controller refuses fetches (sram_ctrl / rram_ctrl EXEC): for this step the
  // core has no DMI pointer into it, so Spike fetches through mmio_load, which refuses: an
  // instruction access fault, as the chip's bus error on the fetch gives. Spike's TLB and decode
  // cache are flushed around the step, so neither holds a pointer the step must not use.
  uint32_t fetch_addr = sys.addr_translate ? sys.addr_translate(pc, true) : pc;
  bool deny = !sys.fetch_allowed(fetch_addr);
  if (deny) {
    fetch_denied_ = true;
    processor_->get_mmu()->flush_tlb();
    processor_->get_mmu()->flush_icache();
  }
  if (dump_clear_) {
    dump_clear_ = false;
    dump_pc_ = dump_npc_ = dump_daddr_ = dump_mtval_ = dump_mepc_ = 0;
    dump_prev_mtval_ = dump_prev_mepc_ = 0;
    dump_prev_valid_ = in_handler_ = false;
  }
  // The crash dump's last data access address: a load's or store's effective address, from the
  // registers before it runs (RV32I loads and stores, C.LW/C.SW/C.LWSP/C.SWSP).
  bool data_access = false;
  {
    state_t *st = processor_->get_state();
    auto x = [&](unsigned r) { return uint32_t(st->XPR[r]); };
    uint32_t c = insn & 0xffff, op = insn & 0x7f;
    if ((insn & 3) == 3 && (op == 0x03 || op == 0x23)) {
      data_access = true;
      int32_t imm = op == 0x03 ? int32_t(insn) >> 20
                               : ((int32_t(insn) >> 25) << 5) | int32_t((insn >> 7) & 31);
      dump_daddr_ = x((insn >> 15) & 31) + uint32_t(imm);
    } else if ((c & 3) == 0 && ((c >> 13) == 2 || (c >> 13) == 6)) {
      data_access = true;
      dump_daddr_ = x(8 + ((c >> 7) & 7)) +
                    ((((c >> 10) & 7) << 3) | (((c >> 6) & 1) << 2) | (((c >> 5) & 1) << 6));
    } else if ((c & 3) == 2 && (c >> 13) == 2) {
      data_access = true;
      dump_daddr_ = x(2) + ((((c >> 12) & 1) << 5) | (((c >> 4) & 7) << 2) | (((c >> 2) & 3) << 6));
    } else if ((c & 3) == 2 && (c >> 13) == 6) {
      data_access = true;
      dump_daddr_ = x(2) + ((((c >> 9) & 0xf) << 2) | (((c >> 7) & 3) << 6));
    }
  }
  // A load of a word that answers with a bus error (Eg2System::read_error): no direct pointer for
  // it in this step, so Spike reads through mmio_load, which refuses: a load access fault, as the
  // chip's d_error gives (rram_ctrl programmers_guide.md: "Uncorrectable ECC errors or integrity
  // failures produce in-band TL-UL error responses which will trigger a processor exception").
  bool is_load = data_access && (((insn & 3) == 3 && (insn & 0x7f) == 0x03) ||
                                 (insn & 0xe003) == 0x4000 || (insn & 0xe003) == 0x4002);
  read_denied_ = is_load && sys.read_error && sys.read_error(dump_daddr_, false);
  if (read_denied_) processor_->get_mmu()->flush_tlb();
  // A CSR instruction on mcounteren (0x306) while rv_core_ibex's MCOUNTEREN_WRITABLE is not
  // MuBi4True reads but does not write it (Eg2System::mcounteren_writable).
  bool mc_locked = (insn & 0x7f) == 0x73 && ((insn >> 12) & 3) != 0 && (insn >> 20) == 0x306 &&
                   sys.mcounteren_writable && !sys.mcounteren_writable();
  reg_t mc_old = mc_locked ? processor_->get_csr(CSR_MCOUNTEREN) : 0;
  reg_t instret = processor_->get_csr(CSR_MINSTRET);
  // addr_to_mem above probes the current fetch before Spike does, and the decoder found this
  // instruction's data address. If either is translated, discard the ordinary page mapping filled
  // by earlier accesses so Spike asks for the redirected pointer.
  bool translated_data = data_access && sys.addr_translate &&
                         sys.addr_translate(dump_daddr_, false) != dump_daddr_;
  if (translation_used_ || translated_data) {
    translation_used_ = false;
    processor_->get_mmu()->flush_tlb();
    processor_->get_mmu()->flush_icache();
  }
  processor_->step(1);
  if (mc_locked) processor_->put_csr(CSR_MCOUNTEREN, mc_old);
  // Spike entered debug mode in this step: an ebreak with dcsr.ebreakm or ebreaku set, the step
  // after a single-stepped instruction, or a trigger with the debug-mode action.
  if (processor_->get_state()->debug_mode) {
    if (deny) fetch_denied_ = false;
    read_denied_ = false;
    translation_used_ = false;
    debug_ = true;
    halted_events_ = 0;
    parked_in_debug_rom();
    std::printf("eg2sim: debug mode entered (dcsr.cause %u) at %s, dpc 0x%08x\n",
                unsigned(processor_->get_state()->dcsr->cause), sys.where().c_str(),
                uint32_t(processor_->get_state()->dpc->read()));
    return false;
  }
  // A trap: the instruction did not retire and the core went elsewhere (not a WFI). A synchronous
  // exception while a handler runs is a double fault: the current state moves to previous.
  if (processor_->get_csr(CSR_MINSTRET) == instret && uint32_t(processor_->get_state()->pc) != pc) {
    if (!(processor_->get_csr(CSR_MCAUSE) >> 31)) {
      if (in_handler_) {
        dump_prev_mtval_ = dump_mtval_;
        dump_prev_mepc_ = dump_mepc_;
        dump_prev_valid_ = true;
      }
      in_handler_ = true;
    }
    dump_mtval_ = uint32_t(processor_->get_csr(CSR_MTVAL));
    dump_mepc_ = uint32_t(processor_->get_csr(CSR_MEPC));
    if (uint32_t(processor_->get_csr(CSR_MCAUSE)) == 0x8000001fu) spike_in_nmi_ = true;
  } else if (insn == 0x30200073) {  // mret
    in_handler_ = false;
    spike_in_nmi_ = false;
  }
  dump_pc_ = pc;
  dump_npc_ = uint32_t(processor_->get_state()->pc);
  if (deny) fetch_denied_ = false;
  if (read_denied_) {
    read_denied_ = false;
    processor_->get_mmu()->flush_tlb();
  }
  if (deny || translation_used_) {
    translation_used_ = false;
    processor_->get_mmu()->flush_tlb();
    processor_->get_mmu()->flush_icache();
  }
  // cpuctrlsts.ic_scr_key_valid, which Spike does not model (kIcKeyRenewInsns): FENCE.I drops the
  // key; a CSR instruction on cpuctrlsts that retired gets bit 8 in its result from the model.
  if (key_wait_) --key_wait_;
  bool retired = uint32_t(processor_->get_state()->pc) == pc + 4;
  if ((insn & 0x707f) == 0x100f) key_wait_ = kIcKeyRenewInsns;
  unsigned f3 = (insn >> 12) & 7, rd = (insn >> 7) & 31;
  if (retired && (insn & 0x7f) == 0x73 && f3 != 0 && f3 != 4 && (insn >> 20) == 0x7c0 && rd) {
    uint32_t v = uint32_t(processor_->get_state()->XPR[rd]);
    processor_->get_state()->XPR.write(rd, (v & ~(1u << 8)) | (key_wait_ ? 0 : 1u << 8));
  }
  if (lock_pending_) {
    lock_pending_ = false;
    if (ena_low_) {
      Eg2System::get().fault_applied(
          "cheriot_ena_low",
          "CHERIoT mode locked, but the core's cheriot_enable_i is forced to MuBi4False: the core "
          "stays in RISC-V mode (Spike) while cheriot_mem_sys sees CHERIoT mode");
    } else {
      handover();
    }
  }
  return insn == kWfi && uint32_t(processor_->get_state()->pc) == pc + 4;
}

// One instruction on the CHERIoT-mode instruction-set model; true if it was a WFI.
bool Eg2SpikeCpu::step_cheriot() {
  if (!cheriot_iss_->step()) {
    request_stop(std::string(cheriot_iss_->name()) + ": " + cheriot_iss_->error());
    return false;
  }
  if (log_) std::fprintf(log_->get(), "%s\n", cheriot_iss_->trace().c_str());
  return cheriot_iss_->wfi_done();
}

char *Eg2SpikeCpu::addr_to_mem(reg_t addr) {
  // rv_core_ibex theory_of_operation.md, "Simple Address Translation": instruction and data
  // transactions have separate translation controls and only accesses inside a matching region
  // are redirected. During a Spike step an access at the current PC is an instruction fetch;
  // other direct-memory accesses are data transactions. Spike fetches only from direct memory, so
  // return the redirected pointer and flush its page-sized cache after the instruction; a NAPOT
  // region may be smaller than that page and must not redirect neighbouring addresses.
  Eg2System &sys = Eg2System::get();
  bool ibus = uint32_t(addr) == pc();
  reg_t original = addr;
  if (sys.addr_translate) addr = sys.addr_translate(uint32_t(addr), ibus);
  if (addr != original) translation_used_ = true;
  if (fetch_denied_ && ibus && !sys.fetch_allowed(uint32_t(addr))) return nullptr;
  if (read_denied_ && !ibus && sys.read_error && sys.read_error(uint32_t(addr), false)) return nullptr;
  for (const tlm::tlm_dmi &d : dmi_) {
    if (addr >= d.get_start_address() && addr <= d.get_end_address()) {
      return reinterpret_cast<char *>(d.get_dmi_ptr() + (addr - d.get_start_address()));
    }
  }
  // Ask the bus once per address: a memory grants DMI, a device does not.
  tlm::tlm_generic_payload trans;
  trans.set_address(addr);
  tlm::tlm_dmi dmi;
  if (socket->get_direct_mem_ptr(trans, dmi)) {
    dmi_.push_back(dmi);
    return reinterpret_cast<char *>(dmi.get_dmi_ptr() + (addr - dmi.get_start_address()));
  }
  return nullptr;
}

bool Eg2SpikeCpu::transport(tlm::tlm_command cmd, reg_t addr, size_t len, uint8_t *bytes) {
  tlm::tlm_generic_payload trans;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  trans.set_command(cmd);
  trans.set_address(addr);
  trans.set_data_ptr(bytes);
  trans.set_data_length(unsigned(len));
  trans.set_streaming_width(unsigned(len));
  trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
  socket->b_transport(trans, delay);
  // An error response is a bus error: Spike raises the access fault, as Ibex does.
  return trans.is_response_ok();
}

bool Eg2SpikeCpu::mmio_load(reg_t addr, size_t len, uint8_t *bytes) {
  Eg2System &sys = Eg2System::get();
  bool ibus = uint32_t(addr) == pc();
  if (sys.addr_translate) {
    reg_t original = addr;
    addr = sys.addr_translate(uint32_t(addr), ibus);
    if (addr != original) translation_used_ = true;
  }
  if (fetch_denied_ && ibus && !sys.fetch_allowed(uint32_t(addr))) return false;
  if (read_denied_ && !ibus && sys.read_error && sys.read_error(uint32_t(addr), true)) return false;
  return transport(tlm::TLM_READ_COMMAND, addr, len, bytes);
}

bool Eg2SpikeCpu::mmio_store(reg_t addr, size_t len, const uint8_t *bytes) {
  Eg2System &sys = Eg2System::get();
  if (sys.addr_translate) {
    reg_t original = addr;
    addr = sys.addr_translate(uint32_t(addr), false);
    if (addr != original) translation_used_ = true;
  }
  return transport(tlm::TLM_WRITE_COMMAND, addr, len, const_cast<uint8_t *>(bytes));
}
