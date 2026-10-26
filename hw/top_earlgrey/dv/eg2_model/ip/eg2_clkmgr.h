// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// clkmgr of the EG2 SystemC model: the transactional clocks' hints (clkmgr.hjson CLK_HINTS,
// CLK_HINTS_STATUS). "When software provides a disable hint, the clock manager checks to see if the
// associated hardware block is idle. If the hardware block is idle, then the clock is disabled. If
// the hardware block is not idle, the clock is kept on. For the enable case, the software hint is
// immediately honored and the clock turned on." CLK_HINTS_STATUS "provides read feedback for the
// current clock state": bit i is 1 while the hint enables the clock or the block (AES, HMAC, KMAC,
// OTBN, in that order) is busy (Eg2IpModel::idle). The model clocks nothing: a block whose clock
// is off still works.

#ifndef EG2_MODEL_IP_EG2_CLKMGR_H_
#define EG2_MODEL_IP_EG2_CLKMGR_H_

#include <cstdint>
#include <string>

#include "../eg2_ip_model.h"

namespace eg2_model {

class Eg2Clkmgr : public Eg2IpModel {
 public:
  Eg2Clkmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    hints_ = reg("CLK_HINTS");
    status_ = reg("CLK_HINTS_STATUS");
    set_modelled(status_);
  }

 protected:
  void reg_before_read(uint16_t r) override {
    if (r != status_) return;
    static const char *const kBlocks[] = {"aes", "hmac", "kmac", "otbn"};
    uint32_t hints = value(hints_), st = 0;
    for (unsigned i = 0; i < 4; ++i) {
      bool busy = false;
      for (Eg2IpModel *m : sys_.models()) {
        if (std::string(m->module()) == kBlocks[i] && !m->idle()) busy = true;
      }
      if (((hints >> i) & 1) || busy) st |= 1u << i;
    }
    set_value(status_, st);
  }

 private:
  uint16_t hints_, status_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CLKMGR_H_
