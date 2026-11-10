// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_CSRNG_H_
#define EG2_MODEL_IP_EG2_CSRNG_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_entropy_common.h"
#include "eg2_entropy_src.h"

namespace eg2_model {

// The csrng application command (theory of operation "Command Description").
struct Eg2CsrngCmd {
  enum Acmd { kInstantiate = 1, kReseed = 2, kGenerate = 3, kUpdate = 4, kUninstantiate = 5 };
  enum Sts {
    kSuccess = 0,
    kInvalidAcmd = 1,
    kInvalidGenCmd = 2,
    kInvalidCmdSeq = 3,
    kReseedCntExceeded = 4,
  };
  uint32_t header = 0;
  std::vector<uint32_t> data;
  unsigned acmd() const { return header & 0xf; }
  unsigned clen() const { return (header >> 4) & 0xf; }
  bool flag0() const { return mubi4_true(header >> 8); }
  unsigned glen() const { return (header >> 12) & 0xfff; }
  bool complete() const { return data.size() >= clen(); }
};

class Eg2Csrng : public Eg2IpModel {
 public:
  static constexpr unsigned kInstances = 3, kSwInstance = 2;

  Eg2Csrng(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    cmd_req_ = reg("CMD_REQ");
    sw_cmd_sts_ = reg("SW_CMD_STS");
    genbits_vld_ = reg("GENBITS_VLD");
    genbits_ = reg("GENBITS");
    int_state_num_ = reg("INT_STATE_NUM");
    int_state_val_ = reg("INT_STATE_VAL");
    int_state_read_enable_ = reg("INT_STATE_READ_ENABLE");
    for (uint16_t r : {sw_cmd_sts_, genbits_vld_, genbits_, int_state_val_}) set_modelled(r);
    for (unsigned i = 0; i < kInstances; ++i) {
      reseed_counter_[i] = reg(("RESEED_COUNTER_" + std::to_string(i)).c_str());
      set_modelled(reseed_counter_[i]);
    }
    recov_alert_sts_ = reg("RECOV_ALERT_STS");
    set_modelled(recov_alert_sts_);
    // The generated random constants are not published. Enable the documented two-domain
    // abstraction only for a bench sequence that compares the domains, so ordinary deterministic
    // model runs retain their established entropy stream.
    sys_.stim_handlers["csrng.lifecycle_diversification"] = [this](const std::string &v) {
      diversify_lc_ = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
    update();
  }

  bool enabled() const { return mubi4_true(value(ctrl_)); }

  // ---- The hardware application interfaces (edn0, edn1) ----
  // Run a command on instance inst; false while it waits for entropy (call again later).
  // For a generate, the blocks are produced one by one with next_block().
  bool run(unsigned inst, const Eg2CsrngCmd &cmd, unsigned *sts) {
    if (!enabled()) return false;
    return execute(inst, cmd, sts);
  }
  // The next 128-bit block of the generate in progress on inst (four words, least significant
  // first); false when none is in progress.
  bool next_block(unsigned inst, uint32_t *words) {
    Gen &g = gen_[inst];
    if (g.remaining == 0) return false;
    uint8_t out[16];
    drbg_[inst].block(out);
    for (unsigned i = 0; i < 4; ++i) words[i] = nist_word(out, 16, i);
    // CS_BUS_CMP_ALERT: the same block twice in a row on the software application port
    // (RECOV_ALERT_STS), forgotten when csrng is disabled. The hardware compares 64 bits of each
    // block (csrng_core.sv); the model compares all 128.
    if (inst == kSwInstance && have_last_ && std::equal(out, out + 16, last_out_)) {
      set_value(recov_alert_sts_, value(recov_alert_sts_) | (1u << 12));
      raise_alert("recov_alert");
    }
    if (inst == kSwInstance) std::copy(out, out + 16, last_out_), have_last_ = true;
    if (--g.remaining == 0) finish_generate(inst);
    return true;
  }
  unsigned remaining(unsigned inst) const { return gen_[inst].remaining; }
  bool instantiated(unsigned inst) const { return drbg_[inst].instantiated; }
  bool fips(unsigned inst) const { return drbg_[inst].fips; }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    for (auto &d : drbg_) d.uninstantiate();
    for (auto &g : gen_) g = Gen();
    sw_cmd_ = Eg2CsrngCmd();
    sw_have_header_ = false;
    sw_pending_ = false;
    sw_words_.clear();
    sw_ack_ = false;
    sw_sts_ = 0;
    state_ptr_ = 0;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == cmd_req_ && enabled()) {
      uint32_t w = wdata & wmask;
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
    } else if (r == int_state_num_) {
      state_ptr_ = 0;
    } else if (r == ctrl_ && !enabled()) {
      // Disabled, csrng drops every instance's working state and any command in progress.
      for (auto &d : drbg_) d.uninstantiate();
      have_last_ = false;
      for (auto &g : gen_) g = Gen();
      sw_have_header_ = sw_pending_ = sw_gen_running_ = false;
      sw_words_.clear();
    }
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == sw_cmd_sts_ || r == genbits_vld_) try_sw();
    if (r == genbits_) {
      fill_sw_words();
      bool ok = mubi4_true(value(ctrl_) >> 4) && !sw_words_.empty();
      set_value(genbits_, ok ? sw_words_.front() : 0);
    }
    if (r == int_state_val_) set_value(int_state_val_, int_state_word());
    update();
  }
  void reg_after_read(uint16_t r) override {
    if (r == genbits_ && mubi4_true(value(ctrl_) >> 4) && !sw_words_.empty()) {
      sw_words_.pop_front();
      if (sw_words_.empty() && gen_[kSwInstance].remaining == 0 && sw_gen_running_) {
        sw_gen_running_ = false;
        acknowledge(Eg2CsrngCmd::kSuccess);
      }
      update();
    } else if (r == int_state_val_) {
      ++state_ptr_;
    }
  }

 private:
  struct Gen {
    unsigned remaining = 0;
    std::vector<uint8_t> adata;  // the generate's additional input (NIST order), empty if none
  };

  // Seed material for instantiate / reseed (theory of operation, the flag0 / clen table).
  bool seed_material(const Eg2CsrngCmd &cmd, uint8_t *seed, bool *fips_seed) {
    uint8_t adata[48];
    nist_from_words(cmd.data.data(), unsigned(cmd.data.size()), adata, 48);
    if (cmd.flag0()) {
      std::memcpy(seed, adata, 48);  // clen 0: a seed of zero
      *fips_seed = false;
      return true;
    }
    // "cs_entropy_req asserts whenever the instance requests entropy from ENTROPY_SRC"
    // (programmer's guide).
    if (!entropy_requested_) {
      entropy_requested_ = true;
      set_intr("cs_entropy_req");
    }
    auto *es = dynamic_cast<Eg2EntropySrc *>(sys_.model("entropy_src"));
    if (!es || !es->seed_for_csrng(seed, fips_seed)) return false;
    entropy_requested_ = false;
    bool hw_debug = sys_.lc_hw_debug_en && sys_.lc_hw_debug_en();
    for (unsigned i = 0; i < 48; ++i) {
      // interfaces.md, "Other CSRNG signals": lc_hw_debug_en_i is "A life-cycle that will
      // select which diversification value is used for xoring with the seed from ENTROPY_SRC."
      // The documents do not publish either random constant. Keep the pre-existing model stream
      // as the debug-enabled value used by the default RMA image, and use 0x5a when debug is
      // disabled; the constants remain a gap draft.
      seed[i] ^= adata[i] ^ (diversify_lc_ && !hw_debug ? 0x5a : 0);
    }
    return true;
  }

  bool execute(unsigned inst, const Eg2CsrngCmd &cmd, unsigned *sts) {
    Eg2CtrDrbg &d = drbg_[inst];
    *sts = Eg2CsrngCmd::kSuccess;
    switch (cmd.acmd()) {
      case Eg2CsrngCmd::kInstantiate:
      case Eg2CsrngCmd::kReseed: {
        bool inst_cmd = cmd.acmd() == Eg2CsrngCmd::kInstantiate;
        if (inst_cmd == d.instantiated) {
          *sts = Eg2CsrngCmd::kInvalidCmdSeq;
          return true;
        }
        uint8_t seed[48];
        bool fips_seed = false;
        if (!seed_material(cmd, seed, &fips_seed)) return false;
        if (inst_cmd) d.instantiate(seed, fips_seed);
        else d.reseed(seed, fips_seed);
        return true;
      }
      case Eg2CsrngCmd::kGenerate: {
        if (!d.instantiated) {
          *sts = Eg2CsrngCmd::kInvalidCmdSeq;
          return true;
        }
        if (cmd.glen() == 0) {
          *sts = Eg2CsrngCmd::kInvalidGenCmd;
          return true;
        }
        Gen &g = gen_[inst];
        g.adata.clear();
        if (cmd.clen()) {
          g.adata.resize(48);
          nist_from_words(cmd.data.data(), unsigned(cmd.data.size()), g.adata.data(), 48);
          d.update(g.adata.data());
        }
        g.remaining = cmd.glen();
        return true;
      }
      case Eg2CsrngCmd::kUpdate: {
        if (!d.instantiated) {
          *sts = Eg2CsrngCmd::kInvalidCmdSeq;
          return true;
        }
        uint8_t adata[48];
        nist_from_words(cmd.data.data(), unsigned(cmd.data.size()), adata, 48);
        d.update(adata);
        return true;
      }
      case Eg2CsrngCmd::kUninstantiate:
        d.uninstantiate();
        gen_[inst] = Gen();
        return true;
      default:
        *sts = Eg2CsrngCmd::kInvalidAcmd;
        return true;
    }
  }

  void finish_generate(unsigned inst) {
    Gen &g = gen_[inst];
    uint8_t zero[48] = {};
    drbg_[inst].update(g.adata.empty() ? zero : g.adata.data());
    ++drbg_[inst].reseed_counter;
  }

  void try_sw() {
    if (!sw_pending_ || !enabled()) return;
    unsigned sts;
    if (!execute(kSwInstance, sw_cmd_, &sts)) return;  // waits for entropy
    sw_pending_ = false;
    if (sw_cmd_.acmd() == Eg2CsrngCmd::kGenerate && sts == Eg2CsrngCmd::kSuccess) {
      sw_gen_running_ = true;  // acknowledged once software has read every block
      fill_sw_words();
    } else {
      acknowledge(sts);
    }
  }
  void fill_sw_words() {
    if (!sw_words_.empty() || gen_[kSwInstance].remaining == 0) return;
    uint32_t w[4];
    if (next_block(kSwInstance, w)) sw_words_.assign(w, w + 4);
  }
  void acknowledge(unsigned sts) {
    sw_ack_ = true;
    sw_sts_ = sts;
    set_intr("cs_cmd_req_done");
  }

  // INT_STATE_VAL (dif_csrng_get_internal_state): reseed counter, V (4 words), key (8 words),
  // flags (0 instantiated, 1 FIPS); zero unless CTRL.READ_INT_STATE and the instance's
  // INT_STATE_READ_ENABLE allow it.
  uint32_t int_state_word() const {
    unsigned inst = value(int_state_num_) & 0xf;
    if (inst >= kInstances || !mubi4_true(value(ctrl_) >> 8) ||
        !((value(int_state_read_enable_) >> inst) & 1)) {
      return 0;
    }
    const Eg2CtrDrbg &d = drbg_[inst];
    unsigned p = state_ptr_;
    if (p == 0) return d.reseed_counter;
    if (p <= 4) return nist_word(d.v, 16, p - 1);
    if (p <= 12) return nist_word(d.key, 32, p - 5);
    if (p == 13) return (d.instantiated ? 1u : 0u) | (d.fips ? 2u : 0u);
    return 0;
  }

  void update() {
    // SW_CMD_STS: CMD_RDY (1), CMD_ACK (2), CMD_STS (5:3).
    bool rdy = enabled() && !sw_pending_ && !sw_gen_running_;
    set_value(sw_cmd_sts_, (rdy ? 2u : 0u) | (sw_ack_ ? 4u : 0u) | ((sw_sts_ & 7) << 3));
    bool vld = !sw_words_.empty() || gen_[kSwInstance].remaining != 0;
    set_value(genbits_vld_, (vld ? 1u : 0u) | (drbg_[kSwInstance].fips ? 2u : 0u));
    for (unsigned i = 0; i < kInstances; ++i) set_value(reseed_counter_[i], drbg_[i].reseed_counter);
  }

  uint16_t ctrl_, cmd_req_, sw_cmd_sts_, genbits_vld_, genbits_, int_state_num_, int_state_val_,
      int_state_read_enable_, reseed_counter_[kInstances], recov_alert_sts_;
  uint8_t last_out_[16] = {};
  bool have_last_ = false;
  std::array<Eg2CtrDrbg, kInstances> drbg_;
  std::array<Gen, kInstances> gen_;
  Eg2CsrngCmd sw_cmd_;
  bool sw_have_header_ = false, sw_pending_ = false, sw_gen_running_ = false, sw_ack_ = false;
  unsigned sw_sts_ = 0;
  std::deque<uint32_t> sw_words_;
  unsigned state_ptr_ = 0;
  bool entropy_requested_ = false;
  bool diversify_lc_ = false;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CSRNG_H_
