// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2Iss: an instruction-set model the core of the EG2 SystemC model (Eg2SpikeCpu) steps instead of
// Spike once CHERIoT mode is locked (doc/system_model in eg2_top_verification: Spike in RISC-V
// mode, MPACT-CHERIoT in CHERIoT mode from the instruction after the CHERIOT_LOCK store). The core
// keeps the time, the quantum, the sleep after WFI and the chip resets; the ISS executes
// instructions and reaches the bus through the core's initiator socket (Transport).

#ifndef EG2_MODEL_EG2_ISS_H_
#define EG2_MODEL_EG2_ISS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <tlm>

// The state the core hands over at the CHERIoT lock (REQ_SOC_07), from Spike: the next PC, the trap
// CSRs, Ibex's cpuctrlsts, x1-x15 (gpr[0] unused). The ISS makes PCC, MTCC and MEPCC executable
// roots at pc, mtvec and mepc, MTDC the memory root, MScratchC the sealing root, x1-x15 integers
// (tag 0), x16-x31 null.
struct Eg2Handover {
  uint32_t pc, mtvec, mepc, mstatus, mie, mcause, mtval, misa;
  uint32_t gpr[16];
  uint32_t cpuctrlsts = 0;  // bits 7:0: the same CSR in both modes, carried over
  uint32_t mcounteren = 0;  // the same CSR in both modes, carried over
};

// cpuctrlsts.ic_scr_key_valid (bit 8): a FENCE.I fetches a new icache scrambling key, which arrives
// in the background (Ibex doc/03_reference/icache.rst, Scramble Key Renewal; cs_registers.rst).
// The model has the key back this many instructions after the FENCE.I; on the chip the request to
// OTP takes some cycles, which the documentation does not give. Both instruction-set models use it
// (2026-10-29: rv_core_ibex_icache_invalidate_test checks that bit 8 drops after a FENCE.I and
// comes back; Spike never set it, and the test never ended).
constexpr uint32_t kIcKeyRenewInsns = 100;

class Eg2Iss {
 public:
  // A transaction on the core's bus: false for an error response (a bus error).
  using Transport =
      std::function<bool(tlm::tlm_command, uint32_t addr, size_t len, uint8_t *bytes)>;

  virtual ~Eg2Iss() = default;
  virtual const char *name() const = 0;
  virtual void handover(const Eg2Handover &s) = 0;
  // The interrupt lines, before a step or while the core sleeps.
  virtual void set_irqs(bool meip, bool mtip, bool msip) = 0;
  // An enabled interrupt is pending (mip & mie): what wakes the core from WFI.
  virtual bool interrupt_pending() = 0;
  // One instruction, or the trap of its fetch; false if the ISS failed (error()).
  virtual bool step() = 0;
  // The last step was a WFI that completed (not wfi(): a macro of Spike's decode.h).
  virtual bool wfi_done() const = 0;
  virtual uint32_t pc() = 0;
  virtual std::string error() const = 0;
  // The last step as one line of the instruction log (--spike-log).
  virtual std::string trace() = 0;
  // The state to hand back to the RISC-V-mode model when the core leaves CHERIoT mode without a
  // reset (the fault cheriot_ena_low): the next PC, x1-x15's addresses and the trap CSRs the ISS
  // exposes; the caller fills *s first with its own values, which stay for what the ISS does not
  // expose. False if the ISS cannot.
  virtual bool take_back(Eg2Handover * /*s*/) { return false; }
  // The crash dump's current PC (the last instruction stepped), next PC and last data access
  // address (rv_core_ibex theory_of_operation.md "Crash Dump Collection"). False if the ISS cannot.
  virtual bool crash_dump(uint32_t * /*pc*/, uint32_t * /*npc*/, uint32_t * /*daddr*/) {
    return false;
  }
  // Ibex's non-maskable interrupt, before the next step (exception_interrupts.rst: interrupt 31,
  // whatever mstatus.MIE and mie say; while its handler runs, until mret, a new one is ignored).
  // False if the ISS cannot take it.
  virtual bool nmi() { return false; }
  // Integer register xr's value (its address, for a capability register), for a debugger's
  // abstract register read while the core is halted. False if the ISS cannot give it.
  virtual bool read_gpr(unsigned /*r*/, uint32_t * /*value*/) { return false; }
  // Debug mode, for an ISS that keeps its own program counter capability (Ibex debug.rst "Debug
  // Mode in CHERIoT"): entry saves it in depcc and runs from mtcc, resume restores it from depcc.
  // False if the ISS has no debug support (the caller then keeps dpc itself).
  virtual bool debug_enter() { return false; }
  virtual bool debug_resume() { return false; }
  // dpc as the ISS keeps it (depcc's address); set_dpc sets it. False if the ISS cannot.
  virtual bool read_dpc(uint32_t * /*pc*/) { return false; }
  virtual bool set_dpc(uint32_t /*pc*/) { return false; }
  // xr written by a debugger's abstract command, as an integer write. False if the ISS cannot.
  virtual bool write_gpr(unsigned /*r*/, uint32_t /*value*/) { return false; }
};

#endif  // EG2_MODEL_EG2_ISS_H_
