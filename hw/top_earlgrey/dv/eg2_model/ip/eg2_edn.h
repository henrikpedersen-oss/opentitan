// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_EDN_H_
#define EG2_MODEL_IP_EG2_EDN_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_csrng.h"

namespace eg2_model {

class Eg2Edn : public Eg2IpModel {
 public:
  // MAIN_SM_STATE encodings (the documentation lists none; from hw/ip/edn/rtl/edn_pkg.sv).
  static constexpr uint32_t kStateIdle = 0x0c1, kStateSwPortMode = 0x095,
                            kStateBootDone = 0x0f0, kStateAutoAckWait = 0x092;

  Eg2Edn(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    inst_ = std::string(d.window) == "edn1" ? 1 : 0;
    ctrl_ = reg("CTRL");
    boot_ins_cmd_ = reg("BOOT_INS_CMD");
    boot_gen_cmd_ = reg("BOOT_GEN_CMD");
    sw_cmd_req_ = reg("SW_CMD_REQ");
    sw_cmd_sts_ = reg("SW_CMD_STS");
    reseed_cmd_ = reg("RESEED_CMD");
    generate_cmd_ = reg("GENERATE_CMD");
    max_reqs_ = reg("MAX_NUM_REQS_BETWEEN_RESEEDS");
    main_sm_state_ = reg("MAIN_SM_STATE");
    recov_alert_sts_ = reg("RECOV_ALERT_STS");
    set_modelled(recov_alert_sts_);
    set_modelled(sw_cmd_sts_);
    set_modelled(main_sm_state_);
    update();
  }

  bool enabled() const { return mubi4_true(value(ctrl_)); }
  bool boot_mode() const { return mubi4_true(value(ctrl_) >> 4); }
  bool auto_mode() const { return mubi4_true(value(ctrl_) >> 8); }

  // An endpoint's request for one 32-bit word of entropy; false while EDN has none to give.
  bool get(uint32_t *word, bool *fips = nullptr) {
    if (!enabled()) return false;
    if (words_.empty() && !produce()) return false;
    *word = words_.front();
    words_.pop_front();
    if (fips) *fips = fips_words_.front();
    fips_words_.pop_front();
    // A requesting endpoint keeps req asserted. Refill as soon as its acknowledged word is consumed,
    // leaving the next ack/data pending just as the interface description requires.
    if (words_.empty() && enabled()) produce();
    return true;
  }

  // rv_core_ibex is a distinct, continuously requesting EDN0 endpoint. Keep its acknowledged
  // word, and that word's fips flag, separate from the stream words consumed by AES, KMAC, keymgr
  // and OTBN endpoints.
  bool get_core(uint32_t *word, bool *fips = nullptr) {
    if (!core_pending_ && enabled()) prefetch_core();
    if (!core_pending_) return false;
    *word = core_word_;
    if (fips) *fips = core_fips_;
    core_pending_ = false;
    if (enabled()) prefetch_core();
    return true;
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    words_.clear();
    fips_words_.clear();
    core_pending_ = false;
    sw_cmd_ = Eg2CsrngCmd();
    sw_have_header_ = sw_pending_ = sw_ack_ = false;
    sw_sts_ = 0;
    boot_state_ = 0;
    have_last_ = false;
    reseed_fifo_.clear();
    gen_fifo_.clear();
    reqs_since_reseed_ = 0;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == ctrl_) {
      // CMD_FIFO_RST (15:12) clears the auto mode's command FIFOs.
      if (mubi4_true(value(ctrl_) >> 12)) {
        reseed_fifo_.clear();
        gen_fifo_.clear();
      }
      bool was_enabled = mubi4_true(old);
      bool was_boot = mubi4_true(old >> 4) && was_enabled;
      if (was_enabled && !enabled() && words_.empty()) {
        // Resolve the continuously asserted endpoint request against the old enabled state before
        // applying the disable. This is the word whose ack may remain asserted until consumption.
        uint32_t disabled = value(ctrl_);
        set_value(ctrl_, old);
        produce();
        set_value(ctrl_, disabled);
      }
      if (was_boot && !(boot_mode() && enabled()) && boot_state_) {
        // Leaving boot-time request mode uninstantiates the boot instance.
        unsigned sts;
        Eg2CsrngCmd un;
        un.header = Eg2CsrngCmd::kUninstantiate;
        if (auto *cs = csrng()) cs->run(inst_, un, &sts);
        boot_state_ = 0;
      }
      if (!enabled()) {
        // Disabling stops EDN "immediately" (theory of operation): auto mode starts afresh, its
        // first reseed after MAX_NUM_REQS_BETWEEN_RESEEDS generates of the new configuration.
        // theory_of_operation.md, "Peripheral Hardware Interface - Req/Ack": "Whenever new
        // values are placed on the bus, the ack is asserted until the values are consumed by the
        // endpoint". Keep a word already acknowledged to a continuously requesting endpoint (and
        // its fips flag with it).
        boot_state_ = 0;
        have_last_ = false;
        reqs_since_reseed_ = 0;
      } else if (!was_enabled && words_.empty()) {
        // The core's RND endpoint continuously requests entropy. Materialize its pending response
        // when EDN starts, rather than waiting for software to read RND_STATUS after EDN stops.
        produce();
        prefetch_core();
      }
    } else if (r == sw_cmd_req_ && enabled()) {
      if (!sw_have_header_) {
        sw_cmd_ = Eg2CsrngCmd();
        sw_cmd_.header = w;
        sw_have_header_ = true;
        sw_ack_ = false;
      } else {
        sw_cmd_.data.push_back(w);
      }
      if (sw_cmd_.complete()) {
        sw_have_header_ = false;
        sw_pending_ = true;
        try_sw();
      }
    } else if (r == reseed_cmd_) {
      reseed_fifo_.push_back(w);
    } else if (r == generate_cmd_) {
      gen_fifo_.push_back(w);
    }
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == sw_cmd_sts_) try_sw();
    update();
  }

 private:
  void prefetch_core() {
    if (core_pending_) return;
    if (words_.empty() && !produce()) return;
    core_word_ = words_.front();
    words_.pop_front();
    core_fips_ = fips_words_.front();
    fips_words_.pop_front();
    core_pending_ = true;
  }

  Eg2Csrng *csrng() { return dynamic_cast<Eg2Csrng *>(sys_.model("csrng")); }

  // Fill the word queue from the csrng instance: the generate in progress, or a new one in
  // boot-time or auto request mode.
  bool produce() {
    Eg2Csrng *cs = csrng();
    if (!cs) return false;
    if (cs->remaining(inst_) == 0) {
      if (boot_mode()) {
        if (!boot_command()) return false;
      } else if (auto_mode()) {
        if (!auto_command()) return false;
      } else {
        return false;
      }
    }
    uint32_t blk[4];
    if (!cs->next_block(inst_, blk)) return false;
    // EDN_BUS_CMP_ALERT: the same genbits block twice in a row from csrng (RECOV_ALERT_STS).
    if (have_last_ && std::equal(blk, blk + 4, last_blk_)) {
      set_value(recov_alert_sts_, value(recov_alert_sts_) | (1u << 12));
      raise_alert("recov_alert");
    }
    std::copy(blk, blk + 4, last_blk_);
    have_last_ = true;
    words_.insert(words_.end(), blk, blk + 4);
    fips_words_.insert(fips_words_.end(), 4, cs->fips(inst_));
    return true;
  }
  // Boot-time request mode: instantiate (BOOT_INS_CMD), then one generate (BOOT_GEN_CMD). A
  // further generate needs firmware to leave the mode.
  bool boot_command() {
    Eg2Csrng *cs = csrng();
    unsigned sts;
    if (boot_state_ == 0) {
      Eg2CsrngCmd c;
      c.header = value(boot_ins_cmd_);
      if (!cs->instantiated(inst_) && !cs->run(inst_, c, &sts)) return false;
      boot_state_ = 1;
    }
    if (boot_state_ == 1) {
      Eg2CsrngCmd c;
      c.header = value(boot_gen_cmd_);
      if (!cs->run(inst_, c, &sts)) return false;
      boot_state_ = 2;
      return sts == Eg2CsrngCmd::kSuccess;
    }
    return false;
  }
  // Auto request mode: the next GENERATE_CMD from its FIFO, and a RESEED_CMD after
  // MAX_NUM_REQS_BETWEEN_RESEEDS generates. Both FIFOs replay their commands.
  bool auto_command() {
    Eg2Csrng *cs = csrng();
    unsigned sts;
    if (gen_fifo_.empty()) return false;
    if (value(max_reqs_) && reqs_since_reseed_ >= value(max_reqs_) && !reseed_fifo_.empty()) {
      Eg2CsrngCmd c = parse(reseed_fifo_);
      if (!cs->run(inst_, c, &sts)) return false;
      reqs_since_reseed_ = 0;
    }
    Eg2CsrngCmd c = parse(gen_fifo_);
    if (!cs->run(inst_, c, &sts)) return false;
    ++reqs_since_reseed_;
    return sts == Eg2CsrngCmd::kSuccess;
  }
  static Eg2CsrngCmd parse(const std::vector<uint32_t> &fifo) {
    Eg2CsrngCmd c;
    c.header = fifo[0];
    for (unsigned i = 1; i < fifo.size() && c.data.size() < c.clen(); ++i) {
      c.data.push_back(fifo[i]);
    }
    return c;
  }

  void try_sw() {
    if (!sw_pending_) return;
    Eg2Csrng *cs = csrng();
    unsigned sts;
    if (!cs || !cs->run(inst_, sw_cmd_, &sts)) return;
    sw_pending_ = false;
    sw_ack_ = true;
    sw_sts_ = sts;
    set_intr("edn_cmd_req_done");
  }

  void update() {
    // SW_CMD_STS: CMD_REG_RDY (0), CMD_RDY (1), CMD_ACK (2), CMD_STS (5:3).
    bool rdy = enabled() && !boot_mode() && !sw_pending_;
    uint32_t st = (rdy ? 1u : 0u) | (rdy && !sw_have_header_ ? 2u : 0u) | (sw_ack_ ? 4u : 0u) |
                  ((sw_sts_ & 7) << 3);
    set_value(sw_cmd_sts_, st);
    uint32_t sm = !enabled() ? kStateIdle
                  : boot_mode() ? kStateBootDone
                  : auto_mode() ? kStateAutoAckWait
                                : kStateSwPortMode;
    set_value(main_sm_state_, sm);
  }

  unsigned inst_;
  uint16_t ctrl_, boot_ins_cmd_, boot_gen_cmd_, sw_cmd_req_, sw_cmd_sts_, reseed_cmd_,
      generate_cmd_, max_reqs_, main_sm_state_, recov_alert_sts_;
  uint32_t last_blk_[4] = {};
  bool have_last_ = false;
  bool core_pending_ = false;
  uint32_t core_word_ = 0;
  bool core_fips_ = false;
  std::deque<uint32_t> words_;
  std::deque<bool> fips_words_;
  Eg2CsrngCmd sw_cmd_;
  bool sw_have_header_ = false, sw_pending_ = false, sw_ack_ = false;
  unsigned sw_sts_ = 0;
  unsigned boot_state_ = 0;
  std::vector<uint32_t> reseed_fifo_, gen_fifo_;
  uint32_t reqs_since_reseed_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_EDN_H_
