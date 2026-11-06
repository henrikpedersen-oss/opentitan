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
// is off freezes an access to it (clock_off below): the core stalls until a reset.

#ifndef EG2_MODEL_IP_EG2_CLKMGR_H_
#define EG2_MODEL_IP_EG2_CLKMGR_H_

#include <cstdint>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_aon.h"
#include "eg2_mem_otp.h"

namespace eg2_model {

class Eg2Clkmgr : public Eg2IpModel {
 public:
  Eg2Clkmgr(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    hints_ = reg("CLK_HINTS");
    enables_ = reg("CLK_ENABLES");
    extclk_ctrl_ = reg("EXTCLK_CTRL");
    extclk_status_ = reg("EXTCLK_STATUS");
    sys_.clock_off = [this](const std::string &b) { return clock_off(b); };
    status_ = reg("CLK_HINTS_STATUS");
    set_modelled(status_);
    set_modelled(extclk_status_);
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
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r != extclk_ctrl_) return;
    // clkmgr theory of operation "Software Requested External Clocks" and EXTCLK_STATUS.ACK:
    // software can switch clocks only when life-cycle hardware debug is enabled; ACK says the
    // requested switch is complete. Model the AST handshake as completing at once.
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    bool selected = (value(extclk_ctrl_) & 0xf) == 0x6;  // MuBi4True
    set_value(extclk_status_, selected && lc && lc->hw_debug_en() ? 0x6 : 0x9);
  }

 private:
  // Whether the block's clock is off: a transactional block (AES, HMAC, KMAC, OTBN) whose hint is
  // clear while it is idle; a peripheral whose CLK_ENABLES bit is clear. The peripherals: those
  // clkmgr_off_peri_test names (bit 0 clk_io_div4_peri uart0, 2 clk_io_peri spi_host0,
  // 3 clk_usb_peri usbdev, 4 clk_io_spi_host1 spi_host1); the rest of each clock group is not
  // gated in the model.
  bool clock_off(const std::string &b) {
    static const char *const kHinted[] = {"aes", "hmac", "kmac", "otbn"};
    for (unsigned i = 0; i < 4; ++i) {
      if (b != kHinted[i] || ((value(hints_) >> i) & 1)) continue;
      for (Eg2IpModel *m : sys_.models()) {
        if (std::string(m->module()) == kHinted[i] && !m->idle()) return false;
      }
      return true;
    }
    static const struct { const char *block; unsigned bit; } kPeri[] = {
        {"uart0", 0}, {"spi_host0", 2}, {"usbdev", 3}, {"spi_host1", 4}};
    for (const auto &p : kPeri) {
      if (b != p.block) continue;
      if (!((value(enables_) >> p.bit) & 1)) return true;
      // The USB clock itself is off in the active state when pwrmgr's CONTROL.USB_CLK_EN_ACTIVE
      // is 0 (pwrmgr.hjson: "USB clock disabled during active power state").
      if (b == "usbdev") {
        auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"));
        return pm && !pm->usb_clk_active();
      }
      return false;
    }
    return false;
  }

  uint16_t hints_, status_, enables_ = 0, extclk_ctrl_, extclk_status_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CLKMGR_H_
