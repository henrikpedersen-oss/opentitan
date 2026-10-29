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
  // In CHERIoT mode the CHERIoT-mode model does not give these fields: the dump reads zeros there (a
  // model limit) rather than the last RISC-V-mode state.
  sys.cpu_crashdump = [this] {
    if (cheriot_) return std::vector<uint32_t>(8, 0);
    return std::vector<uint32_t>{dump_mtval_,      dump_mepc_,      dump_daddr_,
                                 dump_npc_,        dump_pc_,        dump_prev_mtval_,
                                 dump_prev_mepc_,  uint32_t(dump_prev_valid_)};
  };
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
  SC_THREAD(run);
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
  // The crash dump state resets with the core, cleared at the first step after it: rstmgr captures
  // it during the reset.
  dump_clear_ = true;
  state_t *s = processor_->get_state();
  s->pc = cfg_.boot_addr + 0x80;
  s->mtvec->write(cfg_.boot_addr | 1);
  s->csrmap[CSR_MARCHID] =
      std::make_shared<const_csr_t>(processor_.get(), CSR_MARCHID, kIbexMarchid);
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
        std::make_shared<const_csr_t>(processor_.get(), CSR_MHPMEVENT3 + i, 1 << i);
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
  cheriot_ = true;
  std::printf("eg2sim: CHERIoT mode locked: %s from pc 0x%08x after %llu instructions\n",
              cheriot_iss_->name(), h.pc, (unsigned long long)retired_);
}

// A core in CHERIoT mode from reset: the CHERIoT-mode model starts at the reset state.
void Eg2SpikeCpu::cheriot_reset() {
  cheriot_iss_->handover(cfg_.cheriot_reset);
  cheriot_ = true;
  std::printf("eg2sim: CHERIoT mode from reset: %s from pc 0x%08x\n", cheriot_iss_->name(),
              cfg_.cheriot_reset.pc);
}

void Eg2SpikeCpu::drive_irqs() {
  Eg2System &sys = Eg2System::get();
  if (cheriot_) {
    cheriot_iss_->set_irqs(sys.meip, sys.mtip, sys.msip);
    if (sys.nmi) {
      request_stop("NMI in CHERIoT mode: " + std::string(cheriot_iss_->name()) +
                   " has no NMI (not modelled)");
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
  if (sys.nmi) {
    processor_->get_state()->nmi = true;
    sys.nmi = false;
  }
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
  uint64_t events = 0;
  for (;;) {
    if (sys.finished() || !Eg2ModelStop::reason().empty() || sys.reset_pending()) return true;
    drive_irqs();
    if (!low_power && interrupt_pending()) return true;
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
      processor_->reset();
      ibex_reset();
      dmi_.clear();
      cheriot_ = false;
      lock_pending_ = false;
      halted_events_ = 0;
      if (cfg_.cheriot_at_reset && cheriot_iss_) cheriot_reset();
      std::printf("eg2sim: chip reset (%s) after %llu instructions, %.3f us\n",
                  sys.reset_why().c_str(), (unsigned long long)retired_, double(now_ps()) / 1e6);
      sys.apply_reset(sys.reset_cause());
      sys.reset_done();
      ++resets_;
      continue;
    }
    if (!faults_.empty()) inject_due_faults();
    if (sys.core_halted()) {
      halted_wait();
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
  } else if (insn == 0x30200073) {  // mret
    in_handler_ = false;
  }
  dump_pc_ = pc;
  dump_npc_ = uint32_t(processor_->get_state()->pc);
  if (deny) fetch_denied_ = false;
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
