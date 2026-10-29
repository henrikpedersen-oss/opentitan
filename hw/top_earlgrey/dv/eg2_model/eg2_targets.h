// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// TLM-2.0 targets of the EG2 SystemC model (doc/system_model in eg2_top_verification):
//   Eg2Memory      a memory (ROM, SRAM, RRAM, the revocation bitmap window): b_transport and DMI;
//   Eg2RegBlock    a device's register block (eg2_regblock.h).
// Eg2ModelStop holds the reason a target stops the run (for example software polling a register
// no model updates), so a gap in the model is never mistaken for behaviour.
// Addresses a target sees are offsets into its window (Eg2Router subtracts the base).

#ifndef EG2_MODEL_EG2_TARGETS_H_
#define EG2_MODEL_EG2_TARGETS_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

class Eg2Memory : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<Eg2Memory> socket;

  Eg2Memory(sc_core::sc_module_name name, uint32_t base, uint32_t size)
      : sc_core::sc_module(name), socket("socket"), base_(base), size_(size), mem_(size, 0) {
    socket.register_b_transport(this, &Eg2Memory::b_transport);
    socket.register_get_direct_mem_ptr(this, &Eg2Memory::get_direct_mem_ptr);
  }

  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  // The memory's bytes, for the IP that owns it (sram_ctrl's initialisation, for example). A
  // window onto another memory (share_start_of) gives that memory's bytes.
  std::vector<uint8_t> &data() { return backing_ ? backing_->data() : mem_; }

  // This window is the start of another memory: the same bytes, its own size. The revocation
  // bitmap window cheriot_mem_sys.revbm is the base of the meta SRAM (top_earlgrey.hjson:
  // sram_ctrl_meta's ram "aliases cheriot_mem_sys.revbm on purpose"), so sram_ctrl_meta's
  // initialisation zeroes the bitmap software reads through the window.
  void share_start_of(Eg2Memory &backing) {
    if (backing.size() >= size_) backing_ = &backing;
  }

  // Backdoor write at an absolute address (image loading); false outside the memory.
  bool write(uint32_t addr, const uint8_t *data, size_t len) {
    if (addr < base_ || uint64_t(addr - base_) + len > size_) return false;
    std::memcpy(bytes() + (addr - base_), data, len);
    return true;
  }

 private:
  uint8_t *bytes() { return data().data(); }

  void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &) {
    uint64_t off = trans.get_address();
    unsigned len = trans.get_data_length();
    if (off + len > size_ || trans.get_byte_enable_ptr()) {
      trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    if (trans.is_read()) std::memcpy(trans.get_data_ptr(), bytes() + off, len);
    if (trans.is_write()) std::memcpy(bytes() + off, trans.get_data_ptr(), len);
    trans.set_dmi_allowed(true);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
  }

  bool get_direct_mem_ptr(tlm::tlm_generic_payload &, tlm::tlm_dmi &dmi) {
    dmi.set_dmi_ptr(bytes());
    dmi.set_start_address(0);
    dmi.set_end_address(size_ - 1);
    dmi.allow_read_write();
    return true;
  }

  uint32_t base_, size_;
  std::vector<uint8_t> mem_;
  Eg2Memory *backing_ = nullptr;
};

// Why a target stopped the run: the first request wins; empty while none.
class Eg2ModelStop {
 public:
  static void request(const std::string &why) {
    if (reason().empty()) reason() = why;
  }
  static std::string &reason() {
    static std::string s;
    return s;
  }
};

#endif  // EG2_MODEL_EG2_TARGETS_H_
