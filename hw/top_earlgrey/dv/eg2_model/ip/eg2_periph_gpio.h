// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// gpio of the EG2 SystemC model (hw/top_earlgrey/ip_autogen/gpio: data/gpio.hjson,
// doc/theory_of_operation.md, doc/programmers_guide.md).
//
// State: DATA_OUT, DATA_OE, the four interrupt enables and the filter enable, one bit per GPIO.
// Every register that views them is kept coherent after every write:
//   DIRECT_OUT / DIRECT_OE        the whole word
//   MASKED_OUT_* / MASKED_OE_*    "upper 16 bits ... are used as mask"; a read returns the 16 data
//                                 bits of its half, the mask reads 0
//   PER_PIN_IO_i                  data_out (bit 0) is DATA_OUT[i], data_in (bit 8) DATA_IN[i]
//   PER_PIN_OE_i                  DATA_OE[i]
//   PER_PIN_INTR_CTRL_i           bits 0-3 the rising, falling, level-high and level-low enables, bit 4
//                                 the filter enable, of GPIO[i]
// DATA_IN: the chip routes each GPIO through pinmux to a pad and back. The model does not model the
// pads: a GPIO whose output is enabled reads its own output, one whose output is disabled reads
// what outside drives (--stim gpio.in=<hex>, default 0). The input filter (16 stable cycles) is a
// pass-through: no model time passes between a write and the next read anyway.
// Interrupts: "the interrupt for GPIO[i] is raised on a rising edge if INTR_CTRL_EN_RISING[i] is
// set" and likewise for falling edges; a level interrupt is raised while the input has the level
// and its enable is set, so clearing it while the level holds raises it again.
// A write to PER_PIN_IO_i that does not cover its bytes 0 and 1 answers an error and changes nothing
// (programmer's guide: "A byte write returns an error and has no effect"). Not modelled: the straps
// (HW_STRAPS_DATA_IN reads its reset value).
// Output items: on every change of what the pads drive, one line in gpio.model.log,
// "<data_out & data_oe in hex> <data_oe in hex>".

#ifndef EG2_MODEL_IP_EG2_PERIPH_GPIO_H_
#define EG2_MODEL_IP_EG2_PERIPH_GPIO_H_

#include <cstdint>
#include <cstdio>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_periph_stim.h"

namespace eg2_model {

class Eg2Gpio : public Eg2IpModel {
 public:
  static constexpr unsigned kNumIos = 32;

  Eg2Gpio(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    data_in_ = reg("DATA_IN");
    direct_out_ = reg("DIRECT_OUT");
    mout_lo_ = reg("MASKED_OUT_LOWER");
    mout_hi_ = reg("MASKED_OUT_UPPER");
    direct_oe_ = reg("DIRECT_OE");
    moe_lo_ = reg("MASKED_OE_LOWER");
    moe_hi_ = reg("MASKED_OE_UPPER");
    en_[0] = reg("INTR_CTRL_EN_RISING");
    en_[1] = reg("INTR_CTRL_EN_FALLING");
    en_[2] = reg("INTR_CTRL_EN_LVLHIGH");
    en_[3] = reg("INTR_CTRL_EN_LVLLOW");
    en_[4] = reg("CTRL_EN_INPUT_FILTER");
    for (unsigned i = 0; i < kNumIos; ++i) {
      pin_io_[i] = reg(("PER_PIN_IO_" + std::to_string(i)).c_str());
      pin_oe_[i] = reg(("PER_PIN_OE_" + std::to_string(i)).c_str());
      pin_ctrl_[i] = reg(("PER_PIN_INTR_CTRL_" + std::to_string(i)).c_str());
      set_modelled(pin_io_[i]);
    }
    set_modelled(data_in_);
    reset_state();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    reset_state();
  }

 protected:
  bool reg_before_write(uint16_t r, uint32_t, uint32_t wmask) override {
    for (unsigned i = 0; i < kNumIos; ++i) {
      if (r == pin_io_[i] && (wmask & 0xffffu) != 0xffffu) {
        fail_access();
        return false;
      }
    }
    return true;
  }

  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == direct_out_) out_ = (out_ & ~wmask) | w;
    else if (r == direct_oe_) oe_ = (oe_ & ~wmask) | w;
    else if (r == mout_lo_ || r == mout_hi_) out_ = masked(out_, w, r == mout_hi_);
    else if (r == moe_lo_ || r == moe_hi_) oe_ = masked(oe_, w, r == moe_hi_);
    else {
      bool done = false;
      for (unsigned k = 0; k < 5 && !done; ++k) {
        if (r == en_[k]) {
          ctrl_[k] = (ctrl_[k] & ~wmask) | w;
          done = true;
        }
      }
      for (unsigned i = 0; i < kNumIos && !done; ++i) {
        if (r == pin_io_[i] && (wmask & 1)) {
          out_ = (out_ & ~(1u << i)) | ((w & 1) << i);
          done = true;
        } else if (r == pin_oe_[i] && (wmask & 1)) {
          oe_ = (oe_ & ~(1u << i)) | ((w & 1) << i);
          done = true;
        } else if (r == pin_ctrl_[i] && (wmask & 0x1f)) {
          for (unsigned k = 0; k < 5; ++k) {
            ctrl_[k] = (ctrl_[k] & ~(1u << i)) | (((w >> k) & 1) << i);
          }
          done = true;
        }
      }
    }
    update();
  }

 private:
  // A masked write: "upper 16 bits ... used as mask", the lower 16 the data, for half `upper`.
  static uint32_t masked(uint32_t cur, uint32_t w, bool upper) {
    uint32_t mask = w >> 16, data = w & 0xffff;
    unsigned sh = upper ? 16 : 0;
    return (cur & ~(mask << sh)) | ((data & mask) << sh);
  }
  void reset_state() {
    out_ = oe_ = 0;
    for (uint32_t &c : ctrl_) c = 0;
    in_ = input();
    driven_ = ~0u;
    update();
  }
  uint32_t input() const {
    return (out_ & oe_) | (Eg2PeriphStim::u32("gpio.in", 0) & ~oe_);
  }
  void update() {
    uint32_t in = input();
    uint32_t rise = ~in_ & in, fall = in_ & ~in;
    in_ = in;
    // Every view of the state.
    set_value(direct_out_, out_);
    set_value(direct_oe_, oe_);
    set_value(mout_lo_, out_ & 0xffff);
    set_value(mout_hi_, out_ >> 16);
    set_value(moe_lo_, oe_ & 0xffff);
    set_value(moe_hi_, oe_ >> 16);
    for (unsigned k = 0; k < 5; ++k) set_value(en_[k], ctrl_[k]);
    set_value(data_in_, in);
    for (unsigned i = 0; i < kNumIos; ++i) {
      set_value(pin_io_[i], ((out_ >> i) & 1) | (((in >> i) & 1) << 8));
      set_value(pin_oe_[i], (oe_ >> i) & 1);
      uint32_t c = 0;
      for (unsigned k = 0; k < 5; ++k) c |= ((ctrl_[k] >> i) & 1) << k;
      set_value(pin_ctrl_[i], c);
    }
    // Interrupts: edges, and levels while they hold.
    uint32_t raise = (rise & ctrl_[0]) | (fall & ctrl_[1]) | (in & ctrl_[2]) | (~in & ctrl_[3]);
    for (unsigned i = 0; i < kNumIos; ++i) {
      if ((raise >> i) & 1) set_intr(i);
    }
    // What the pads drive, as an output item when it changes.
    uint32_t drv = out_ & oe_;
    if (drv != driven_ || oe_ != driven_oe_) {
      if (driven_ != ~0u) {
        char item[32];
        std::snprintf(item, sizeof item, "%08x %08x", drv, oe_);
        sys_.emit(desc().window, item);
      }
      driven_ = drv;
      driven_oe_ = oe_;
    }
  }

  uint16_t data_in_, direct_out_, mout_lo_, mout_hi_, direct_oe_, moe_lo_, moe_hi_;
  uint16_t en_[5];
  uint16_t pin_io_[kNumIos], pin_oe_[kNumIos], pin_ctrl_[kNumIos];
  uint32_t out_ = 0, oe_ = 0, in_ = 0, ctrl_[5] = {};
  uint32_t driven_ = ~0u, driven_oe_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_GPIO_H_
