// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_ROM_CTRL_H_
#define EG2_MODEL_IP_EG2_ROM_CTRL_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_mem_otp.h"

namespace eg2_model {

// rom_ctrl (hw/ip/rom_ctrl: doc/theory_of_operation.md, "The startup ROM check").
// The checker consumes stored 39-bit words, including their seven integrity bits,
// in logical address order. Each is zero-padded to five little-endian bytes for
// cSHAKE256. The final eight stored words supply EXP_DIGEST independently; only
// their low 32 bits participate in the comparison. DIGEST is the computed hash.
//
// Eg2System::rom_image carries these words separately from the executable ELF.
// tools/prepare_rom_check.py extracts them from an existing scrambled VMEM and
// verifies its decoded body against the ELF. The checker does not manufacture
// ECC, recompute expected words or use observed DUT values. Missing input stops
// the run as not modelled. The cSHAKE N/S split is explicit image configuration:
// the docs name the ROM_CTRL prefix but do not define that split (DV ticket).
//
// Not modelled: cycle timing, checker FSM fault encodings, and bus-side ROM ECC
// faults. A bad checker digest still follows the documented lifecycle bypass.
// rom_ctrl.corrupt_digest flips a bit of the supplied expected word as a fault
// input; it never substitutes a freshly computed expected digest.
//
// top_earlgrey/doc/design/README.md, "ROM Controller": "It compares the digest with the expected
// digest stored in the top words of the ROM and reports the result to the power manager, which
// stops chip startup on a mismatch." "The power manager ignores the result when both DFT and
// hardware debug are enabled, i.e., in the `TEST_UNLOCKED*` and `RMA` life cycle states."
class Eg2RomCtrl : public Eg2IpModel {
 public:
  Eg2RomCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    digest_ = reg("DIGEST_0");
    exp_ = reg("EXP_DIGEST_0");
    for (uint16_t i = 0; i < 8; ++i) {
      set_modelled(uint16_t(digest_ + i));
      set_modelled(uint16_t(exp_ + i));
    }
    sys_.stim_handlers["rom_ctrl.corrupt_digest"] = [this](const std::string &v) {
      corrupt_digest_ = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
    sys_.stim_handlers["rom_ctrl.expect_bad_boot"] = [this](const std::string &v) {
      expect_bad_boot_ = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
  }
  void start_of_simulation() override { check(); }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    check();
  }

 private:
  void check() {
    auto it = sys_.memories.find("rom_ctrl.rom");
    if (it == sys_.memories.end()) return;
    const auto &image = sys_.rom_image;
    if (image.words.size() != it->second->size() / 4 || image.words.size() < 8) {
      Eg2ModelStop::request(image.error.empty()
          ? "not modelled: ROM checker needs the stored 39-bit words and expected digest"
          : image.error);
      return;
    }
    const auto digest = image.computed_digest();
    bool good = true;
    for (uint16_t i = 0; i < 8; ++i) {
      uint32_t expected = uint32_t(image.words[image.words.size() - 8 + i]);
      if (corrupt_digest_ && i == 7) expected ^= 1;
      set_value(uint16_t(digest_ + i), digest[i]);
      set_value(uint16_t(exp_ + i), expected);
      good = good && digest[i] == expected;
    }
    if (good) return;
    sys_.chip_event("rom_integrity_bad");
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    if (lc && lc->dft_en() && lc->hw_debug_en()) return;
    sys_.halt_core("rom_ctrl", "ROM digest mismatch stopped chip startup");
    if (expect_bad_boot_) {
      sys_.finish(true, "ROM digest mismatch stopped production startup as expected");
    }
  }
  uint16_t digest_, exp_;
  bool corrupt_digest_ = false, expect_bad_boot_ = false;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_ROM_CTRL_H_
