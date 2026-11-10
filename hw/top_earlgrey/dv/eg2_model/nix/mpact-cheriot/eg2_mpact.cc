// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// libeg2_mpact (eg2_mpact.h): MPACT-CHERIoT's CheriotTop with its memory replaced by the caller's
// bus. Built inside the MPACT-CHERIoT workspace as package //eg2 (BUILD.bazel here;
// nix/mpact-cheriot.nix copies this directory in).

#include "eg2_mpact.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_cat.h"
#include "cheriot/cheriot_decoder.h"
#include "cheriot/cheriot_register.h"
#include "cheriot/cheriot_state.h"
#include "cheriot/cheriot_top.h"
#include "mpact/sim/generic/data_buffer.h"
#include "mpact/sim/generic/instruction.h"
#include "mpact/sim/util/memory/atomic_memory.h"
#include "mpact/sim/util/memory/memory_interface.h"
#include "mpact/sim/util/memory/tagged_memory_interface.h"
#include "riscv/riscv_counter_csr.h"
#include "riscv/riscv_state.h"

// MPACT's load barrier reads these when CheriotState is constructed (cheriot_state.cc).
ABSL_DECLARE_FLAG(uint64_t, revocation_ram_base);
ABSL_DECLARE_FLAG(uint64_t, revocation_mem_base);

namespace {

using ::mpact::sim::cheriot::CheriotDecoder;
using ::mpact::sim::cheriot::CheriotRegister;
using ::mpact::sim::cheriot::CheriotState;
using ::mpact::sim::cheriot::CheriotTop;
using ::mpact::sim::generic::DataBuffer;
using ::mpact::sim::generic::Instruction;
using ::mpact::sim::generic::ReferenceCount;
using ::mpact::sim::riscv::RiscVCounterCsr;
using ::mpact::sim::riscv::RiscVCounterCsrHigh;
using EC = ::mpact::sim::riscv::ExceptionCode;

constexpr unsigned kGranule = 8;  // bytes per capability tag

// A load completes by executing its child instruction (it writes the destination register), now
// or after the data buffer's latency, as mpact-sim's own memories do.
void finish_load(int latency, Instruction *inst, ReferenceCount *context) {
  if (inst == nullptr) return;
  if (latency == 0) {
    inst->Execute(context);
    return;
  }
  inst->IncRef();
  if (context != nullptr) context->IncRef();
  inst->state()->function_delay_line()->Add(latency, [inst, context]() {
    inst->Execute(context);
    if (context != nullptr) context->DecRef();
    inst->DecRef();
  });
}

// Data accesses: the caller's bus. A bus error is the access fault of the current instruction (its
// destination register is not written).
class BusMemory : public ::mpact::sim::util::TaggedMemoryInterface {
 public:
  explicit BusMemory(const eg2_mpact_bus &bus) : bus_(bus) {}
  void set_state(CheriotState *state) { state_ = state; }
  void set_pc(uint32_t pc) { pc_ = pc; }
  // The load barrier's bitmap port (eg2_mpact_create_revocation): MPACT's revocation memory base
  // is then 0, so the address of its bitmap read is the byte's offset in the bitmap.
  void set_revocation(const eg2_mpact_revocation &rev) {
    rev_ = rev;
    has_rev_ = true;
  }

  void Load(uint64_t address, DataBuffer *db, Instruction *inst,
            ReferenceCount *context) override {
    // CheriotState::MustRevoke's read of the bitmap is the only load without an instruction (but
    // the debugger's, DbgLoadMemory, which nothing here calls).
    if (has_rev_ && inst == nullptr && context == nullptr) {
      uint8_t *p = static_cast<uint8_t *>(db->raw_ptr());
      unsigned n = db->size<uint8_t>();
      for (unsigned i = 0; i < n; ++i) {
        if (rev_.revbm(rev_.user, uint32_t(address + i), &p[i]) != 0) p[i] = 0xff;
      }
      return;
    }
    if (bus_.load(bus_.user, uint32_t(address), db->raw_ptr(), db->size<uint8_t>(), nullptr)) {
      fault(address, *EC::kLoadAccessFault);
      return;
    }
    finish_load(db->latency(), inst, context);
  }
  void Load(DataBuffer *address_db, DataBuffer *mask_db, int el_size, DataBuffer *db,
            Instruction *inst, ReferenceCount *context) override {
    auto addrs = address_db->Get<uint64_t>();
    auto mask = mask_db->Get<bool>();
    uint8_t *p = static_cast<uint8_t *>(db->raw_ptr());
    for (size_t i = 0; i < addrs.size(); ++i) {
      if (!mask[i]) continue;
      if (bus_.load(bus_.user, uint32_t(addrs[i]), p + i * el_size, el_size, nullptr)) {
        fault(addrs[i], *EC::kLoadAccessFault);
        return;
      }
    }
    finish_load(db->latency(), inst, context);
  }
  void Store(uint64_t address, DataBuffer *db) override {
    if (bus_.store(bus_.user, uint32_t(address), db->raw_ptr(), db->size<uint8_t>(), nullptr)) {
      fault(address, *EC::kStoreAccessFault);
    }
  }
  void Store(DataBuffer *address_db, DataBuffer *mask_db, int el_size, DataBuffer *db) override {
    auto addrs = address_db->Get<uint64_t>();
    auto mask = mask_db->Get<bool>();
    const uint8_t *p = static_cast<const uint8_t *>(db->raw_ptr());
    for (size_t i = 0; i < addrs.size(); ++i) {
      if (mask[i] && bus_.store(bus_.user, uint32_t(addrs[i]), p + i * el_size, el_size, nullptr)) {
        fault(addrs[i], *EC::kStoreAccessFault);
        return;
      }
    }
  }
  // Capability accesses. db is null for a load of the tags alone.
  void Load(uint64_t address, DataBuffer *db, DataBuffer *tags, Instruction *inst,
            ReferenceCount *context) override {
    unsigned n = tags->size<uint8_t>();
    std::vector<uint8_t> scratch;
    void *data = db ? db->raw_ptr() : (scratch.resize(n * kGranule), scratch.data());
    if (bus_.load(bus_.user, uint32_t(address), data, n * kGranule,
                  static_cast<uint8_t *>(tags->raw_ptr()))) {
      fault(address, *EC::kLoadAccessFault);
      return;
    }
    finish_load(db ? db->latency() : tags->latency(), inst, context);
  }
  void Store(uint64_t address, DataBuffer *db, DataBuffer *tags) override {
    if (bus_.store(bus_.user, uint32_t(address), db->raw_ptr(), db->size<uint8_t>(),
                   static_cast<const uint8_t *>(tags->raw_ptr()))) {
      fault(address, *EC::kStoreAccessFault);
    }
  }

 private:
  void fault(uint64_t address, uint64_t code) {
    state_->Trap(/*is_interrupt=*/false, address, code, pc_, nullptr);
  }

  eg2_mpact_bus bus_;
  eg2_mpact_revocation rev_{};
  bool has_rev_ = false;
  CheriotState *state_ = nullptr;
  uint32_t pc_ = 0;
};

// Instruction fetches: the caller's bus (the caller checks the fetch before each step and raises
// the access fault itself; a failed fetch here decodes as zeros, an illegal instruction).
class FetchMemory : public ::mpact::sim::util::MemoryInterface {
 public:
  explicit FetchMemory(const eg2_mpact_bus &bus) : bus_(bus) {}
  void Load(uint64_t address, DataBuffer *db, Instruction *inst,
            ReferenceCount *context) override {
    unsigned n = db->size<uint8_t>();
    if (bus_.fetch(bus_.user, uint32_t(address), db->raw_ptr(), n)) {
      std::memset(db->raw_ptr(), 0, n);
    }
    finish_load(db->latency(), inst, context);
  }
  void Load(DataBuffer *, DataBuffer *, int, DataBuffer *, Instruction *,
            ReferenceCount *) override {}
  void Store(uint64_t, DataBuffer *) override {}
  void Store(DataBuffer *, DataBuffer *, int, DataBuffer *) override {}

 private:
  eg2_mpact_bus bus_;
};

}  // namespace

struct eg2_mpact {
  eg2_mpact(const eg2_mpact_bus &bus, const eg2_mpact_revocation *rev)
      : data(bus), fetch(bus), atomic(&data) {
    if (rev) {
      // The barrier's heap base and bitmap, for this CheriotState only.
      data.set_revocation(*rev);
      uint64_t ram = absl::GetFlag(FLAGS_revocation_ram_base);
      uint64_t mem = absl::GetFlag(FLAGS_revocation_mem_base);
      absl::SetFlag(&FLAGS_revocation_ram_base, rev->heap_base);
      absl::SetFlag(&FLAGS_revocation_mem_base, 0);
      state = new CheriotState("CherIoT", &data, &atomic);
      absl::SetFlag(&FLAGS_revocation_ram_base, ram);
      absl::SetFlag(&FLAGS_revocation_mem_base, mem);
    } else {
      state = new CheriotState("CherIoT", &data, &atomic);
    }
    data.set_state(state);
    decoder = new CheriotDecoder(state, &fetch);
    top = new CheriotTop("Cheriot", state, decoder);
    // minstret and mcycle count retired instructions and cycles, as in MPACT's own front ends.
    auto *csrs = state->csr_set();
    auto minstret = csrs->GetCsr("minstret"), minstreth = csrs->GetCsr("minstreth");
    auto mcycle = csrs->GetCsr("mcycle"), mcycleh = csrs->GetCsr("mcycleh");
    if (minstret.ok() && minstreth.ok() && mcycle.ok() && mcycleh.ok()) {
      static_cast<RiscVCounterCsr<uint32_t, CheriotState> *>(*minstret)
          ->set_counter(top->counter_num_instructions());
      static_cast<RiscVCounterCsrHigh<CheriotState> *>(*minstreth)
          ->set_counter(top->counter_num_instructions());
      static_cast<RiscVCounterCsr<uint32_t, CheriotState> *>(*mcycle)
          ->set_counter(top->counter_num_cycles());
      static_cast<RiscVCounterCsrHigh<CheriotState> *>(*mcycleh)
          ->set_counter(top->counter_num_cycles());
    }
    state->set_on_wfi([this](const Instruction *) {
      wfi = true;
      return true;
    });
  }
  ~eg2_mpact() {
    delete depcc;
    delete top;
    delete decoder;
    delete state;
  }

  CheriotRegister *reg(const std::string &name) {
    auto it = state->registers()->find(name);
    return it == state->registers()->end() ? nullptr
                                           : static_cast<CheriotRegister *>(it->second);
  }
  CheriotRegister *pcc() { return reg(CheriotState::kPcName); }
  bool set_csr(const char *name, uint32_t v) {
    auto c = state->csr_set()->GetCsr(name);
    if (!c.ok()) return false;
    (*c)->Set(v);
    return true;
  }

  BusMemory data;
  FetchMemory fetch;
  ::mpact::sim::util::AtomicMemory atomic;
  CheriotState *state = nullptr;
  CheriotDecoder *decoder = nullptr;
  CheriotTop *top = nullptr;
  bool wfi = false;
  std::string error, disasm;
  // Ibex's debug SCR depcc (24), which MPACT has not (cs_registers.rst "CHERIoT Special Capability
  // Registers": "holds dpc as cap"); created at the first debug entry.
  CheriotRegister *depcc = nullptr;
  bool debug = false;
};

extern "C" {

eg2_mpact *eg2_mpact_create(const eg2_mpact_bus *bus) { return new eg2_mpact(*bus, nullptr); }

eg2_mpact *eg2_mpact_create_revocation(const eg2_mpact_bus *bus, const eg2_mpact_revocation *rev) {
  return new eg2_mpact(*bus, rev);
}

void eg2_mpact_destroy(eg2_mpact *m) { delete m; }

void eg2_mpact_handover(eg2_mpact *m, const eg2_mpact_handover_state *s) {
  CheriotRegister *pcc = m->pcc();
  pcc->ResetExecuteRoot();
  pcc->set_address(s->pc);
  m->state->mtcc()->ResetExecuteRoot();
  m->state->mtcc()->set_address(s->mtvec);
  m->state->mepcc()->ResetExecuteRoot();
  m->state->mepcc()->set_address(s->mepc);
  m->state->mtdc()->ResetMemoryRoot();
  m->state->mscratchc()->ResetSealingRoot();
  for (unsigned r = 1; r < 32; ++r) {
    CheriotRegister *c = m->reg(absl::StrCat(CheriotState::kCregPrefix, r));
    if (!c) continue;
    c->ResetNull();
    if (r < 16) c->set_address(s->gpr[r]);
    c->set_tag(false);
  }
  m->set_csr("mstatus", s->mstatus);
  m->set_csr("mie", s->mie);
  m->set_csr("mcause", s->mcause);
  m->set_csr("mtval", s->mtval);
  m->set_csr("misa", s->misa);
  m->state->set_branch(false);
}

void eg2_mpact_set_irqs(eg2_mpact *m, int meip, int mtip, int msip) {
  m->state->mip()->set_meip(meip != 0);
  m->state->mip()->set_mtip(mtip != 0);
  m->state->mip()->set_msip(msip != 0);
  // The available interrupt is recomputed from the lines as they are now.
  m->state->reset_is_interrupt_available();
  m->state->CheckForInterrupt();
}

int eg2_mpact_step(eg2_mpact *m) {
  m->wfi = false;
  m->data.set_pc(m->pcc()->address());
  auto r = m->top->Step(1);
  if (!r.ok()) {
    m->error = std::string(r.status().message());
    return -1;
  }
  return 0;
}

void eg2_mpact_fetch_fault(eg2_mpact *m) {
  uint32_t pc = m->pcc()->address();
  m->state->Trap(/*is_interrupt=*/false, pc, *EC::kInstructionAccessFault, pc, nullptr);
  // Trap leaves the branch flag for CheriotTop's step loop, which is not running here: the next
  // step starts at the handler, from PCC.
  m->state->set_branch(false);
}

void eg2_mpact_step_emulated(eg2_mpact *m, uint32_t len, unsigned rd, uint32_t value) {
  m->wfi = false;
  if (rd != 0) {
    if (CheriotRegister *c = m->reg(absl::StrCat(CheriotState::kCregPrefix, rd))) {
      c->ResetNull();
      c->set_address(value);
      c->set_tag(false);
    }
  }
  uint32_t next = m->pcc()->address() + len;
  m->pcc()->set_address(next);
  m->top->counter_num_instructions()->Increment(1);
  m->top->counter_num_cycles()->Increment(1);
  if (m->state->is_interrupt_available()) {
    m->state->TakeAvailableInterrupt(next);
    m->state->set_branch(false);
  }
}

void eg2_mpact_fence_i(eg2_mpact *m) { m->top->decode_cache()->InvalidateAll(); }

void eg2_mpact_clear_cap_tag(eg2_mpact *m, unsigned rd) {
  if (rd == 0) return;
  if (CheriotRegister *c = m->reg(absl::StrCat(CheriotState::kCregPrefix, rd))) c->set_tag(false);
}

void eg2_mpact_nmi(eg2_mpact *m) {
  m->wfi = false;
  uint32_t pc = m->pcc()->address();
  m->state->Trap(/*is_interrupt=*/true, /*trap_value=*/0, /*exception_code=*/31, pc, nullptr);
  // As for a fetch fault: the next step starts at the handler, from PCC.
  m->state->set_branch(false);
}

int eg2_mpact_wfi(const eg2_mpact *m) { return m->wfi; }

uint32_t eg2_mpact_pc(eg2_mpact *m) { return m->pcc()->address(); }

// Debug mode (Ibex debug.rst "Debug Mode in CHERIoT"): "On entry into Debug Mode, PCC is saved in
// depcc and replaced by mtcc, as when taking a trap, and dret restores PCC from depcc."
void eg2_mpact_debug_enter(eg2_mpact *m) {
  if (m->debug) return;
  if (!m->depcc) m->depcc = new CheriotRegister(m->state, "depcc");
  m->depcc->CopyFrom(*m->pcc());
  m->pcc()->CopyFrom(*m->state->mtcc());
  m->debug = true;
}

void eg2_mpact_debug_resume(eg2_mpact *m) {
  if (!m->debug) return;
  m->pcc()->CopyFrom(*m->depcc);
  m->debug = false;
}

uint32_t eg2_mpact_dpc(eg2_mpact *m) { return m->depcc ? m->depcc->address() : 0; }

// dpc written by the debugger: depcc's address, as CSetAddr sets an address (the tag is cleared
// when the new address is not representable). What a dpc write does to depcc's metadata is not
// documented (ticket).
void eg2_mpact_set_dpc(eg2_mpact *m, uint32_t pc) {
  if (m->depcc) m->depcc->set_address(pc);
}

// x<r> written as an integer (Ibex cheriot.rst: the capability portion of a register is "untagged
// by integer writes"): a null capability holding the value as its address.
int eg2_mpact_set_gpr(eg2_mpact *m, unsigned r, uint32_t value) {
  CheriotRegister *c = r && r < 32 ? m->reg(absl::StrCat(CheriotState::kCregPrefix, r)) : nullptr;
  if (!c) return -1;
  c->ResetNull();
  c->set_address(value);
  c->set_tag(false);
  return 0;
}

uint32_t eg2_mpact_gpr(eg2_mpact *m, unsigned r) {
  CheriotRegister *c = r < 32 ? m->reg(absl::StrCat(CheriotState::kCregPrefix, r)) : nullptr;
  return c ? c->address() : 0;
}

int eg2_mpact_csr(eg2_mpact *m, const char *name, uint32_t *value) {
  auto c = m->state->csr_set()->GetCsr(name);
  if (!c.ok()) return -1;
  *value = (*c)->AsUint32();
  return 0;
}

const char *eg2_mpact_disasm(eg2_mpact *m, uint32_t pc) {
  auto d = m->top->GetDisassembly(pc);
  m->disasm = d.ok() ? *d : std::string("?");
  return m->disasm.c_str();
}

const char *eg2_mpact_error(const eg2_mpact *m) { return m->error.c_str(); }

}  // extern "C"
