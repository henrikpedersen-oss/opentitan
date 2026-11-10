// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Address decoding for the functional model. Each transaction carries an
// absolute address; the selected target receives an offset within its window.
// The Earl Grey platform supplies the generated EG2_WINDOW_TABLE. Other boards
// supply their own window list. This class does not model crossbar arbitration,
// pipelines, outstanding requests or cycle timing.
//
// Window order matters: the first matching window wins. Earl Grey deliberately
// places the revocation-bitmap window before its overlapping meta-SRAM window.
// A zero-sized window is the optional default responder and must come last.

#ifndef EG2_MODEL_EG2_ROUTER_H_
#define EG2_MODEL_EG2_ROUTER_H_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include "eg2_transaction.h"

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
  // Find the first matching window, or -1 when the bus must return an error.
  int decode(uint64_t address) const {
    for (size_t i = 0; i < windows_.size(); ++i) {
      const Window &window = windows_[i];
      if (window.size == 0) return int(i);
      if (address >= window.base && address - window.base < window.size) return int(i);
    }
    return -1;
  }

  void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
    if (!eg2_model::valid_transaction(trans)) return;
    const uint64_t address = trans.get_address();
    const int index = decode(address);
    if (index < 0 || (windows_[index].size != 0 &&
                     !eg2_model::range_fits(address - windows_[index].base,
                                           trans.get_data_length(), windows_[index].size))) {
      trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    trans.set_address(address - windows_[index].base);
    (*sockets_[index])->b_transport(trans, delay);
    trans.set_address(address);
  }

  bool get_direct_mem_ptr(tlm::tlm_generic_payload &trans, tlm::tlm_dmi &dmi) {
    const uint64_t address = trans.get_address();
    const int index = decode(address);
    if (index < 0) return false;
    // DMI could bypass an earlier overlapping window (for example the bitmap's
    // word-write restriction). Keep overlapping regions on the checked path.
    const auto &window = windows_[index];
    if (!window.size) return false;
    for (int i = 0; i < index; ++i) {
      if (uint64_t(windows_[i].base) < uint64_t(window.base) + window.size &&
          uint64_t(window.base) < uint64_t(windows_[i].base) + windows_[i].size)
        return false;
    }
    trans.set_address(address - windows_[index].base);
    const bool granted = (*sockets_[index])->get_direct_mem_ptr(trans, dmi);
    trans.set_address(address);
    if (granted) {
      if (dmi.get_start_address() > address - window.base ||
          dmi.get_end_address() < address - window.base) return false;
      dmi.set_end_address(std::min<uint64_t>(dmi.get_end_address(), uint64_t(window.size) - 1));
      dmi.set_start_address(dmi.get_start_address() + windows_[index].base);
      dmi.set_end_address(dmi.get_end_address() + windows_[index].base);
    }
    return granted;
  }

  std::vector<Window> windows_;
  std::vector<std::unique_ptr<tlm_utils::simple_initiator_socket<Eg2Router>>> sockets_;
};

#endif  // EG2_MODEL_EG2_ROUTER_H_
