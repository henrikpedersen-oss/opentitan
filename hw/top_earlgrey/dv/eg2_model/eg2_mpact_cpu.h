// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// MPACT-CHERIoT as the CHERIoT-mode instruction-set model of the EG2 SystemC model (Eg2Iss; the
// library is libeg2_mpact.so with its C interface eg2_mpact.h: eg2_top_verification's flake package
// mpact-cheriot, nix/mpact-cheriot.nix). The core (Eg2SpikeCpu) hands it Spike's state at the
// instruction after the CHERIOT_LOCK store and steps it from then on, until the next chip reset.
//
// MPACT-CHERIoT does not decode FENCE.I or any of RV32BFull's bit manipulation (Zba, Zbb, Zbs, Zbc,
// Zbkb, Zbkx), which Ibex executes in CHERIoT mode and the CHERIoT toolchain emits: eg2sim executes
// those itself (step) and MPACT retires them with the result (eg2_mpact_step_emulated). misa is
// Spike's with E=1, I=0 (REQ_SOC_03); MPACT's own is RV32IMAFDC with S.
//
// Every fetch, load and store is a TLM-2.0 transaction on the core's bus. A fetch from a memory
// whose controller refuses fetches, or a bus error on the fetch, is an instruction access fault; a
// bus error on a load or store is a load or store access fault.
//
// Tags: one per 8-byte granule of main and sec SRAM and of the RRAM, in sram_ctrl_meta's RAM at the
// offsets of the meta SRAM map (top_earlgrey.hjson; the EG2 spec's Configuration): SRAM tags at
// 0x8C00 for 0x1000_0000-0x1002_FFFF, NVM tags at 0x0C00 for 0x3000_0000-0x301F_FFFF; granule g of
// a region is bit g % 8 of the byte g / 8. A capability load elsewhere returns tag 0; a capability
// store elsewhere writes its data and drops the tag. Every other store clears the tag of each
// granule it writes.
//
// Not modelled (the run goes on without them): the revocation load filter (a capability loaded
// through the revocation bitmap is not invalidated) and the hardware revoker; an NMI in CHERIoT
// mode (MPACT has none: the run stops, so an alert escalation that reaches the core's NMI in
// CHERIoT mode ends an eg2sim run there); code changed after it ran once (MPACT's decode cache
// keeps the old instruction).
//
// Ibex's custom CSRs, which MPACT has not, are executed by eg2sim: cpuctrlsts (0x7c0; bits 7:0
// written, 8 ic_scr_key_valid reads 1, carried over at the handover in both directions) and
// secureseed (0x7c1; reads 0), CSRRW/S/C and their immediate forms. PCC's
// permit-access-system-registers is not checked for those two: MPACT's interface exposes no PCC
// permissions. Not modelled: the bits the hardware sets (6 sync_exc_seen, 7 double_fault_seen).
// (2026-10-29: on Earl Grey they were left to MPACT, which logs every read of a CSR it lacks;
// rv_core_ibex_icache_invalidate_test_cheriot ran to the instruction limit and wrote 10 GB of log.)
//
// Another board (Eg2MpactConfig; the RTOS test SoC, rtos_soc/) gives its own tag map, the load
// barrier (MPACT's own, eg2_mpact_create_revocation: a CLC of a tagged, non-sealing capability whose
// base's revocation bit is set clears the tag, the bitmap read through the board's port), and
// whether eg2sim executes the Ibex CSRs above.

#ifndef EG2_MODEL_EG2_MPACT_CPU_H_
#define EG2_MODEL_EG2_MPACT_CPU_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "eg2_iss.h"
#include "eg2_mpact.h"  // $(EG2_MPACT)/include

class Eg2Memory;

// A board's tags and load barrier; the default is Earl Grey 2's (above).
struct Eg2MpactConfig {
  struct TagRegion {
    uint32_t base, size, meta_offset;
  };
  std::vector<uint8_t> *meta = nullptr;  // the meta SRAM; nullptr: EG2's sram_ctrl_meta.ram
  std::vector<TagRegion> tag_regions;
  bool barrier = false;                  // the load barrier on this board's bitmap
  uint32_t heap_base = 0;
  // The bitmap byte at offset; false for a failed lookup (the capability is then revoked).
  std::function<bool(uint32_t offset, uint8_t *byte)> revbm;
  bool ibex_csrs = false;                // eg2sim executes cpuctrlsts, secureseed (EG: always)
};

class Eg2MpactCpu : public Eg2Iss {
 public:
  explicit Eg2MpactCpu(Transport transport, const Eg2MpactConfig *cfg = nullptr);
  ~Eg2MpactCpu() override;

  const char *name() const override { return "MPACT-CHERIoT"; }
  void handover(const Eg2Handover &s) override;
  void set_irqs(bool meip, bool mtip, bool msip) override;
  bool interrupt_pending() override;
  bool step() override;
  bool wfi_done() const override;
  uint32_t pc() override;
  std::string error() const override;
  std::string trace() override;
  // PCC's and x1-x15's addresses, mstatus, mie, mcause and mtval; MTCC and MEPCC are special
  // capability registers MPACT's interface does not expose (mtvec and mepc stay Spike's).
  bool take_back(Eg2Handover *s) override;

 private:
  static int fetch_cb(void *user, uint32_t addr, void *data, uint32_t len);
  static int load_cb(void *user, uint32_t addr, void *data, uint32_t len, uint8_t *tags);
  static int store_cb(void *user, uint32_t addr, const void *data, uint32_t len,
                      const uint8_t *tags);
  static int revbm_cb(void *user, uint32_t offset, uint8_t *byte);
  // An Ibex custom CSR instruction (cfg_.ibex_csrs): true if insn was one, executed.
  bool ibex_csr(uint32_t insn);
  bool fetch(uint32_t addr, void *data, uint32_t len);
  // The tag bit of the granule at addr in the meta SRAM, or nullptr: no tag (an untagged region).
  uint8_t *tag_byte(uint32_t addr, unsigned *bit);

  Transport transport_;
  Eg2MpactConfig cfg_;
  bool board_ = false;  // cfg_ given
  eg2_mpact *m_ = nullptr;
  Eg2Memory *meta_ = nullptr;
  uint32_t cpuctrlsts_ = 0;
  uint32_t last_pc_ = 0, last_insn_ = 0;
  bool last_fetch_fault_ = false, last_fence_i_ = false, last_emulated_ = false;
};

#endif  // EG2_MODEL_EG2_MPACT_CPU_H_
