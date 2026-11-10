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
// PROGRAM_BUFFER holds eight words; bound a program that never reaches ebreak.
constexpr unsigned kProgbufMaxInsns = 1000;
}  // namespace

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

