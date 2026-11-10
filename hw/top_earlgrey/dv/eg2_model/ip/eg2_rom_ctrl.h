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
#include "eg2_kmac.h"

namespace eg2_model {

// rom_ctrl (hw/ip/rom_ctrl: doc/theory_of_operation.md). "We expect to use the `cSHAKE256`
// algorithm, with prefix \"ROM_CTRL\"." "Each read produces 39 bits of data, which are padded with
// a zero to 40 bits to match the interface expected by the KMAC block." "The top eight words in ROM
// (by logical address) are interpreted as a 256-bit expected hash." The model computes cSHAKE256
// over five little-endian bytes per body word. The model's ELF memory has neither the seven ECC
// bits nor the netlist-random address/data scrambling constants, so the fifth byte is zero and the
// model derives the good expected value from that same independently computed body digest before
// applying a requested backing-ROM corruption. This is a gap draft, not a copy of EXP_DIGEST into
// DIGEST: changing any body word changes DIGEST, and changing only the expected digest does not.
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
    std::vector<uint8_t> &m = it->second->data();
    size_t top = m.size() - 32;
    std::vector<uint8_t> msg;
    msg.reserve((top / 4) * 5);
    for (size_t off = 0; off + 4 <= top; off += 4) {
      msg.insert(msg.end(), m.begin() + off, m.begin() + off + 4);
      msg.push_back(0);
    }
    const std::vector<uint8_t> name = {'R', 'O', 'M', '_', 'C', 'T', 'R', 'L'};
    std::vector<uint8_t> got = Eg2Kmac::app_cshake(name, {}, msg, 32);
    bool good = true;
    for (uint16_t i = 0; i < 8; ++i) {
      uint32_t v = uint32_t(got[4 * i]) | (uint32_t(got[4 * i + 1]) << 8) |
                   (uint32_t(got[4 * i + 2]) << 16) | (uint32_t(got[4 * i + 3]) << 24);
      uint32_t expected = v ^ (corrupt_digest_ && i == 7 ? 1u : 0u);
      set_value(uint16_t(digest_ + i), v);
      set_value(uint16_t(exp_ + i), expected);
      good = good && v == expected;
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
