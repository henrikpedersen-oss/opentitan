// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI-C interface to the debug module reference model (ip/eg2_dm_core.h), memory binding: a UVM
// bench sends the debugger's DMI transactions, the core's debug-memory accesses and the system
// bus responses to the model, and compares the RTL's DMI read data, debug-memory read data, system
// bus requests and debug_req/ndmreset/dmactive with the model's (eg2_dm_cmodel_pkg.sv).

#include <cstdint>
#include <deque>

#include "../ip/eg2_dm_core.h"

namespace {

struct Handle {
  eg2_model::Eg2DmCore dm{eg2_model::Eg2DmCore::Binding::kMemory};
  // Accesses the model has started and the bench has not yet taken (eg2_dm_sba_take).
  std::deque<eg2_model::Eg2DmCore::SbaRequest> started;
  Handle() {
    dm.set_bus([this](const eg2_model::Eg2DmCore::SbaRequest &r) { started.push_back(r); });
  }
};

Handle *H(void *h) { return static_cast<Handle *>(h); }

}  // namespace

extern "C" {

void *eg2_dm_create() { return new Handle(); }
void eg2_dm_release(void *h) { delete H(h); }

void eg2_dm_reset(void *h) {
  H(h)->dm.reset();
  H(h)->started.clear();
}
void eg2_dm_hart_reset(void *h) { H(h)->dm.hart_reset(); }
void eg2_dm_set_enables(void *h, int dmi, int full) { H(h)->dm.set_enables(dmi != 0, full != 0); }
void eg2_dm_set_unavailable(void *h, int u) { H(h)->dm.set_unavailable(u != 0); }
void eg2_dm_set_cheriot(void *h, int c) { H(h)->dm.set_cheriot(c != 0); }

// 1: done, *data and *mask (the bits the model predicts) set; 0: life cycle gates the DMI.
int eg2_dm_dmi_read(void *h, unsigned int addr, unsigned int *data, unsigned int *mask) {
  uint32_t v = 0, m = 0;
  bool ok = H(h)->dm.dmi_read(addr, &v, &m);
  *data = v;
  *mask = ok ? m : 0;
  return ok;
}
int eg2_dm_dmi_write(void *h, unsigned int addr, unsigned int data) {
  return H(h)->dm.dmi_write(addr, data);
}

int eg2_dm_debug_req(void *h) { return H(h)->dm.debug_req(); }
int eg2_dm_ndmreset(void *h) { return H(h)->dm.ndmreset(); }
int eg2_dm_dmactive(void *h) { return H(h)->dm.dmactive(); }
int eg2_dm_halted(void *h) { return H(h)->dm.halted(); }
int eg2_dm_busy(void *h) { return H(h)->dm.busy(); }

// The core's debug-memory accesses; off is the offset in the debug memory, be the byte enables.
void eg2_dm_mem_write(void *h, unsigned int off, unsigned int data, unsigned int be) {
  H(h)->dm.mem_write(off, data, be);
}
unsigned int eg2_dm_mem_read(void *h, unsigned int off, unsigned int *mask) {
  uint32_t m = 0;
  uint32_t v = H(h)->dm.mem_read(off, &m);
  *mask = m;
  return v;
}

// The oldest access the model started that the bench has not taken: 1 and its fields, or 0.
int eg2_dm_sba_take(void *h, int *write, unsigned int *addr, unsigned int *size,
                    unsigned int *wdata) {
  auto &q = H(h)->started;
  if (q.empty()) return 0;
  *write = q.front().write;
  *addr = q.front().addr;
  *size = q.front().size;
  *wdata = q.front().wdata;
  q.pop_front();
  return 1;
}
// The bus's response to the access in progress (rdata right-aligned).
void eg2_dm_sba_complete(void *h, unsigned int rdata, int error) {
  H(h)->dm.sba_complete(rdata, error != 0);
}
int eg2_dm_sbbusy(void *h) { return H(h)->dm.sbbusy(); }

}  // extern "C"
