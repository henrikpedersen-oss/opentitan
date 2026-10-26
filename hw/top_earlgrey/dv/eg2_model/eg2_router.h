// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The bus and address map of the EG2 SystemC model: a TLM-2.0 router. Every window and memory
// comes from the generated window table (eg2_tables.h's EG2_WINDOW_TABLE, from
// top_earlgrey.gen.hjson by eg2_top_verification's scripts/gen_eg2_tables.py), never written by
// hand. An address in no window answers TLM_ADDRESS_ERROR_RESPONSE, as the crossbar answers an
// error. DMI requests are forwarded and translated to absolute addresses.

#ifndef EG2_MODEL_EG2_ROUTER_H_
#define EG2_MODEL_EG2_ROUTER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

class Eg2Router : public sc_core::sc_module {
 public:
  struct Window {
    std::string name;
    uint32_t base;
    uint32_t size;
  };

  tlm_utils::simple_target_socket<Eg2Router> socket;

  // One initiator socket per window, created here: SystemC creates ports only while their module
  // is being constructed.
  Eg2Router(sc_core::sc_module_name name, std::vector<Window> windows)
      : sc_core::sc_module(name), socket("socket"), windows_(std::move(windows)) {
    socket.register_b_transport(this, &Eg2Router::b_transport);
    socket.register_get_direct_mem_ptr(this, &Eg2Router::get_direct_mem_ptr);
    for (size_t i = 0; i < windows_.size(); ++i) {
      sockets_.emplace_back(new tlm_utils::simple_initiator_socket<Eg2Router>(
          sc_core::sc_gen_unique_name("to_target")));
    }
  }

  // The initiator socket of window i, to bind to its target.
  tlm_utils::simple_initiator_socket<Eg2Router> &to(size_t i) { return *sockets_[i]; }

  const std::vector<Window> &windows() const { return windows_; }

 private:
  // A window of size 0 is a default responder: every address no window before it holds.
  int decode(uint64_t addr) const {
    for (size_t i = 0; i < windows_.size(); ++i) {
      if (windows_[i].size == 0) return int(i);
      if (addr >= windows_[i].base && addr - windows_[i].base < windows_[i].size) return int(i);
    }
    return -1;
  }

  void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
    uint64_t addr = trans.get_address();
    int i = decode(addr);
    if (i < 0 || (windows_[i].size != 0 &&
                  addr + trans.get_data_length() > uint64_t(windows_[i].base) + windows_[i].size)) {
      trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    trans.set_address(addr - windows_[i].base);
    (*sockets_[i])->b_transport(trans, delay);
    trans.set_address(addr);
  }

  bool get_direct_mem_ptr(tlm::tlm_generic_payload &trans, tlm::tlm_dmi &dmi) {
    uint64_t addr = trans.get_address();
    int i = decode(addr);
    if (i < 0) return false;
    trans.set_address(addr - windows_[i].base);
    bool ok = (*sockets_[i])->get_direct_mem_ptr(trans, dmi);
    trans.set_address(addr);
    if (ok) {
      dmi.set_start_address(dmi.get_start_address() + windows_[i].base);
      dmi.set_end_address(dmi.get_end_address() + windows_[i].base);
    }
    return ok;
  }

  std::vector<Window> windows_;
  std::vector<std::unique_ptr<tlm_utils::simple_initiator_socket<Eg2Router>>> sockets_;
};

#endif  // EG2_MODEL_EG2_ROUTER_H_
