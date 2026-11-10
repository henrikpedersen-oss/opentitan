// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_LC_CTRL_H_
#define EG2_MODEL_IP_EG2_LC_CTRL_H_

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../eg2_ip_model.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"
#include "eg2_otp_ctrl.h"

namespace eg2_model {

// ---- lc_ctrl (hw/ip/lc_ctrl: programmer's guide, theory of operation; doc/security/specs/
// device_life_cycle "allowed state transitions") ----
//
// At reset the state and the transition count are decoded from OTP's LIFE_CYCLE partition: a
// word vector that matches no state, or a count that matches no count, reads as INVALID. 24
// attempts make the state SCRAP. LC_ID_STATE is PERSONALIZED once SECRET2 is locked. HW_REVISION*
// are the top's lc_ctrl parameters (SiliconCreatorId 0x4001, ProductId 0x0010, RevisionId 0x01),
// DEVICE_ID and MANUF_STATE come from HW_CFG0. STATUS.INITIALIZED and READY are set.
//
// CLAIM_TRANSITION_IF: writing MuBi8True claims the mutex (the TAP never holds it here) and sets
// TRANSITION_REGWEN; anything else releases it. TRANSITION_CMD.START: the count is incremented in
// OTP first (TRANSITION_COUNT_ERROR at 24). The transition is then checked against the allowed
// transitions of the life-cycle specification and against OTP (a state can only move A -> B word
// by word): an illegal one sets TRANSITION_ERROR. A conditional one (RAW_UNLOCK, TEST_UNLOCK,
// TEST_EXIT, RMA_UNLOCK) needs its token's partition locked (TEST_UNLOCK and TEST_EXIT: SECRET0,
// RMA_UNLOCK: SECRET2; TOKEN_ERROR otherwise), and the cSHAKE128 hash of the supplied token must
// equal the descrambled hash in OTP. A legal one is programmed into OTP
// (TRANSITION_SUCCESSFUL). Either way the controller is then in POST_TRANSITION, the state and
// count read 21 and 31, and the CPU is disabled until a separately driven reset. A volatile RAW
// unlock is available when a test-chip RAW token is supplied as stimulus; it changes the decoded
// state without writing OTP, incrementing the counter or requiring a reset. The life-cycle broadcast signals
// are accessors (lc_ctrl_function_signals_table.md, lc_ctrl_access_signals_table.md). The OTP
// vendor test interface is storage only.
//
// Escalation (theory_of_operation.md "Security Escalation"): the alert handler's severities 1 and 2
// (the top's esc_scrap_state0 and esc_scrap_state1 receivers) each move the state to ESCALATE, a
// virtual SCRAP not written to OTP: LC_STATE reads ESCALATE, lc_escalate_en
// is on and lc_cpu_en off, which disables the core's fetch (rv_core_ibex "Fetch Enable"). The same
// document's "Escalation Handling" section calls the first channel esc_wipe_secrets, which would
// only assert lc_escalate_en; the model follows "Security Escalation" and the top's names
// (status/rtl_tickets.md). Assumption: the state clears on any modeled chip reset. The document
// instead says decoded outputs remain disabled "until the next power cycle" and does not define
// whether an internal reset is such a power cycle; that conflict is a specification gap.
class Eg2LcCtrl : public Eg2IpModel {
 public:
  Eg2LcCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    status_ = reg("STATUS");
    claim_regwen_ = reg("CLAIM_TRANSITION_IF_REGWEN");
    claim_ = reg("CLAIM_TRANSITION_IF");
    trans_regwen_ = reg("TRANSITION_REGWEN");
    cmd_ = reg("TRANSITION_CMD");
    ctrl_ = reg("TRANSITION_CTRL");
    token_ = reg("TRANSITION_TOKEN_0");
    target_ = reg("TRANSITION_TARGET");
    state_ = reg("LC_STATE");
    cnt_ = reg("LC_TRANSITION_CNT");
    id_ = reg("LC_ID_STATE");
    rev0_ = reg("HW_REVISION0");
    rev1_ = reg("HW_REVISION1");
    dev_id_ = reg("DEVICE_ID_0");
    manuf_ = reg("MANUF_STATE_0");
    for (uint16_t r : {status_, claim_, trans_regwen_, state_, cnt_, id_, rev0_, rev1_}) {
      set_modelled(r);
    }
    for (uint16_t i = 0; i < 8; ++i) {
      set_modelled(uint16_t(dev_id_ + i));
      set_modelled(uint16_t(manuf_ + i));
    }
    for (unsigned sev : {1u, 2u}) {
      sys_.esc_receiver(sev, [this, sev](bool level) {
        if (level) escalate(sev);
      });
    }
    sys_.lc_escalate_en = [this] { return escalate_en(); };
    // The rram_ctrl_lc_rw_en chip sequence observes these five life-cycle broadcasts directly
    // while the CPU is disabled. Their state table is documented in lc_ctrl's function/access
    // signal tables; "off" checks all five are deasserted.
    sys_.stim_handlers["lc_ctrl.rram_rw_expect"] = [this](const std::string &v) {
      if (v != "off") return false;
      bool ok = !creator_seed_sw_rw_en() && !owner_seed_sw_rw_en() && !iso_part_sw_wr_en() &&
                !iso_part_sw_rd_en() && !seed_hw_rd_en();
      if (!ok) {
        sys_.finish(false, "lc_ctrl.rram_rw_expect mismatch: an RRAM life-cycle access signal was on");
      } else {
        std::printf("eg2sim: lc_ctrl.rram_rw_expect: all five access signals are off\n");
      }
      return true;
    };
    // The SRAM execution chip sequence covers four reset-sampled combinations. Software derives
    // its expected fetch result from these two modeled inputs, so also check that the bench drove
    // the intended combination; otherwise a duplicated combination could pass without coverage.
    sys_.stim_handlers["lc_ctrl.sram_exec_expect"] = [this](const std::string &v) {
      size_t colon = v.find(':');
      if (colon == std::string::npos) return false;
      uint32_t want_state = v.substr(0, colon) == "Rma"    ? uint32_t(kLcRma)
                            : v.substr(0, colon) == "Prod" ? uint32_t(kLcProd)
                                                           : uint32_t(kLcInvalid);
      bool want_ifetch = v.substr(colon + 1) == "on";
      if (want_state == kLcInvalid || (v.substr(colon + 1) != "on" &&
                                       v.substr(colon + 1) != "off")) {
        return false;
      }
      Eg2OtpCtrl *o = otp();
      bool got_ifetch = o && o->en_sram_ifetch();
      if (state() != want_state || got_ifetch != want_ifetch) {
        char why[180];
        std::snprintf(why, sizeof why,
                      "lc_ctrl.sram_exec_expect mismatch: expected state %u, SRAM ifetch %s; "
                      "modeled state %u, SRAM ifetch %s",
                      want_state, want_ifetch ? "on" : "off", state(),
                      got_ifetch ? "on" : "off");
        sys_.finish(false, why);
      }
      return true;
    };
    sys_.lc_hw_debug_en = [this] { return hw_debug_en(); };
    sys_.stim_handlers["lc_ctrl.otp_state"] = [this](const std::string &v) {
      return write_otp_state(uint32_t(std::stoul(v, nullptr, 0)));
    };
    sys_.stim_handlers["lc_ctrl.otp_count"] = [this](const std::string &v) {
      return write_otp_count(uint32_t(std::stoul(v, nullptr, 0)));
    };
    sys_.stim_handlers["lc_ctrl.clear_secret2"] = [this](const std::string &v) {
      Eg2OtpCtrl *o = otp();
      if (!o || std::stoul(v, nullptr, 0) == 0) return false;
      o->clear_partition(kEg2OtpSecret2);
      return true;
    };
    sys_.stim_handlers["lc_ctrl.clear_secret0"] = [this](const std::string &v) {
      Eg2OtpCtrl *o = otp();
      if (!o || std::stoul(v, nullptr, 0) == 0) return false;
      o->clear_partition(kEg2OtpSecret0);
      return true;
    };
    sys_.stim_handlers["lc_ctrl.hash_symbol"] = [this](const std::string &v) {
      size_t c = v.find(':');
      if (c == std::string::npos || !sys_.read_test_symbol || !sys_.write_test_symbol) return false;
      std::vector<uint8_t> token(16);
      if (!sys_.read_test_symbol(v.substr(0, c), token.data(), token.size())) return false;
      std::vector<uint8_t> hash = hash_token(token);
      return sys_.write_test_symbol(v.substr(c + 1), hash.data(), hash.size());
    };
    sys_.stim_handlers["lc_ctrl.hash_to_symbol"] = [this](const std::string &v) {
      size_t c = v.find(':');
      std::vector<uint8_t> token;
      if (c == std::string::npos || !parse_token(v.substr(0, c), &token) ||
          !sys_.write_test_symbol) return false;
      std::vector<uint8_t> hash = hash_token(token);
      return sys_.write_test_symbol(v.substr(c + 1), hash.data(), hash.size());
    };
    sys_.stim_handlers["lc_ctrl.sw_destination"] = [this](const std::string &v) {
      if (!sys_.write_test_symbol) return false;
      uint8_t state = uint8_t(std::stoul(v, nullptr, 0));
      return sys_.write_test_symbol("kDestState", &state, 1);
    };
    sys_.stim_handlers["lc_ctrl.otp_word"] = [this](const std::string &v) {
      size_t c = v.find(':');
      Eg2OtpCtrl *o = otp();
      if (!o || c == std::string::npos) return false;
      uint32_t off = uint32_t(std::stoul(v.substr(0, c), nullptr, 0));
      uint32_t word = uint32_t(std::stoul(v.substr(c + 1), nullptr, 0));
      if (off > o->otp().size() || o->otp().size() - off < 4) return false;
      for (unsigned i = 0; i < 4; ++i) o->otp()[off + i] = uint8_t(word >> (8 * i));
      return true;
    };
    sys_.stim_handlers["lc_ctrl.otp_test_unlock"] = [this](const std::string &v) {
      return provision_token(kEg2OtpParts[kEg2OtpSecret0].off, v);
    };
    sys_.stim_handlers["lc_ctrl.otp_test_exit"] = [this](const std::string &v) {
      return provision_token(kEg2OtpParts[kEg2OtpSecret0].off + 16, v);
    };
    sys_.stim_handlers["lc_ctrl.otp_rma"] = [this](const std::string &v) {
      return provision_token(kEg2OtpParts[kEg2OtpSecret2].off, v);
    };
    sys_.stim_handlers["lc_ctrl.raw_unlock"] = [this](const std::string &v) {
      std::vector<uint8_t> token;
      if (!parse_token(v, &token)) return false;
      raw_unlock_hash_ = hash_token(token);
      return true;
    };
    sys_.stim_handlers["lc_ctrl.auto_test_unlock"] = [this](const std::string &v) {
      return parse_token(v, &auto_test_unlock_token_);
    };
    sys_.stim_handlers["lc_ctrl.external_clock"] = [this](const std::string &v) {
      set_value(ctrl_, std::stoul(v, nullptr, 0) != 0 ? value(ctrl_) | 1u : value(ctrl_) & ~1u);
      return true;
    };
    sys_.stim_handlers["lc_ctrl.jtag_transition"] = [this](const std::string &v) {
      size_t c = v.find(':');
      uint32_t target = uint32_t(std::stoul(v.substr(0, c), nullptr, 0));
      std::vector<uint8_t> token(16, 0);
      if (c != std::string::npos && !parse_token(v.substr(c + 1), &token)) return false;
      transition(rep5(target), token, false);
      return true;
    };
    sys_.stim_handlers["lc_ctrl.volatile_raw_unlock"] = [this](const std::string &v) {
      std::vector<uint8_t> token;
      if (!parse_token(v, &token)) return false;
      transition(kLcTestUnlocked0, token, true);
      return true;
    };
    sys_.stim_handlers["lc_ctrl.program_error"] = [this](const std::string &v) {
      program_error_ = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
    sys_.stim_handlers["lc_ctrl.expect_state"] = [this](const std::string &v) {
      uint32_t expected = uint32_t(std::stoul(v, nullptr, 0));
      bool ok = state() == expected;
      sys_.finish(ok, ok ? "life cycle state decoded from OTP as expected"
                         : "life cycle state decoded from OTP did not match the bench check");
      return true;
    };
  }

  // The decoded state (Eg2LcState) the chip's broadcast signals follow.
  uint32_t state() const {
    return escalated_          ? uint32_t(kLcEscalate)
           : post_transition_ ? uint32_t(kLcPostTransition)
                               : state_dec_;
  }

  // An escalation on severity sev (1: esc_scrap_state0, 2: esc_scrap_state1).
  void escalate(unsigned sev) {
    if (escalated_) return;
    escalated_ = true;
    publish();
    sys_.note("lc_ctrl: escalation (esc_scrap_state" + std::to_string(sev - 1) +
              "): life cycle state ESCALATE until the model resets, lc_escalate_en on, lc_cpu_en off");
    sys_.halt_core("lc_ctrl", "lc_cpu_en off in the ESCALATE state");
  }
  uint32_t count() const { return count_; }
  // The broadcast signals (doc tables; "Y*" entries follow the states that table marks).
  bool cpu_en() const {
    uint32_t s = state();
    return eg2_lc_test_unlocked(s) || s == kLcDev || s == kLcProd || s == kLcProdEnd ||
           s == kLcRma;
  }
  bool hw_debug_en() const {
    uint32_t s = state();
    return eg2_lc_test_unlocked(s) || s == kLcDev || s == kLcRma;
  }
  bool dft_en() const {
    uint32_t s = state();
    return eg2_lc_test_unlocked(s) || s == kLcRma;
  }
  bool nvm_debug_en() const { return dft_en(); }
  bool keymgr_en() const {
    uint32_t s = state();
    return s == kLcDev || s == kLcProd || s == kLcProdEnd || s == kLcRma;
  }
  // "Y*" in DEV, PROD and PROD_END: the creator seed is software-accessible only until SECRET2 is
  // provisioned (locked), and readable by hardware only from then on (rram_ctrl_lc_rw_en_test:
  // written in DEV unprovisioned, refused in PROD provisioned). RMA is a plain "Y" for both.
  bool creator_seed_sw_rw_en() const {
    return state() == kLcRma || (keymgr_en() && !secret2_locked());
  }
  bool owner_seed_sw_rw_en() const { return keymgr_en(); }
  bool seed_hw_rd_en() const { return state() == kLcRma || (keymgr_en() && secret2_locked()); }
  bool iso_part_sw_rd_en() const {
    uint32_t s = state();
    return s == kLcProd || s == kLcProdEnd || s == kLcRma;
  }
  bool iso_part_sw_wr_en() const { return eg2_lc_test_unlocked(state()) || keymgr_en(); }
  bool rma_state() const { return state() == kLcRma; }
  bool escalate_en() const {
    uint32_t s = state();
    return s == kLcScrap || s == kLcInvalid || s == kLcEscalate;
  }

  void start_of_simulation() override { power_up(); }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    power_up();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == claim_) {
      if (!(value(claim_regwen_) & 1)) return;
      bool claim = ((wdata & wmask) & 0xff) == 0x96;
      set_value(claim_, claim ? 0x96 : 0x69);
      if (!post_transition_) set_value(trans_regwen_, claim ? 1 : 0);
    } else if (r == ctrl_) {
      // registers.md, TRANSITION_CTRL.VOLATILE_RAW_UNLOCK: "Software can check whether
      // VOLATILE_RAW_UNLOCK is available by writing 1 and reading back the register value."
      uint32_t next = (value(ctrl_) & ~wmask) | (wdata & wmask);
      set_value(ctrl_, (next & ~2u) | (!raw_unlock_hash_.empty() ? (next & 2u) : 0));
    } else if (r == cmd_ && (wdata & wmask & 1) && (value(trans_regwen_) & 1)) {
      std::vector<uint8_t> token(16);
      for (unsigned w = 0; w < 4; ++w) {
        uint32_t x = value(uint16_t(token_ + w));
        for (unsigned i = 0; i < 4; ++i) token[4 * w + i] = uint8_t(x >> (8 * i));
      }
      transition(value(target_), token, (value(ctrl_) & 2) != 0);
    }
  }

 private:
  Eg2OtpCtrl *otp() const { return dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core")); }
  bool secret2_locked() const {
    Eg2OtpCtrl *o = otp();
    return o && o->partition_locked(kEg2OtpSecret2);
  }

  static uint32_t rep5(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 6; ++i) r |= (v & 0x1f) << (5 * i);
    return r;
  }

  void power_up() {
    post_transition_ = false;
    escalated_ = false;
    Eg2OtpCtrl *o = otp();
    state_dec_ = kLcInvalid;
    count_ = 31;
    if (o) {
      decode(o);
      bool personalized = o->partition_locked(kEg2OtpSecret2);
      set_value(id_, personalized ? 0x55555555u : 0u);
      for (uint16_t i = 0; i < 8; ++i) {
        set_value(uint16_t(dev_id_ + i), o->word(kEg2OtpDeviceId + 4 * i));
        set_value(uint16_t(manuf_ + i), o->word(kEg2OtpManufState + 4 * i));
      }
    }
    set_value(rev0_, 0x40010010u);
    set_value(rev1_, 0x01u);
    uint32_t st = 0x1;  // INITIALIZED
    if (state_dec_ != kLcInvalid) st |= 0x2;  // READY
    else st |= 1u << 9;                       // STATE_ERROR
    set_value(status_, st);
    publish();
    if (!cpu_en()) {
      sys_.halt_core("lc_ctrl", "lc_cpu_en off in life cycle state " + std::to_string(state()));
    }
    if (eg2_lc_test_locked(state_dec_) && !auto_test_unlock_token_.empty()) {
      // chip_sw_lc_walkthrough_testunlocks_vseq asks the LC TAP for the next TEST_UNLOCKED state
      // after each reset. The stimulus supplies only its token; the destination is derived from
      // the state decoded from OTP, so it cannot supply the software test's expected answer.
      uint32_t next = state_dec_ + 1;
      sys_.at(sys_.now_ps() + 1000000,
              [this, next] { transition(rep5(next), auto_test_unlock_token_, false); });
    }
  }

  void decode(Eg2OtpCtrl *o) {
    // The transition count.
    int k = -1;
    bool zero = true;
    for (int j = 0; j < 24; ++j) zero = zero && o->half(kEg2OtpLcTransitionCnt + 2 * j) == 0;
    if (zero) {
      k = 0;
    } else {
      for (int n = 1; n <= 24 && k < 0; ++n) {
        bool ok = true;
        for (int j = 0; j < 24 && ok; ++j) {
          ok = o->half(kEg2OtpLcTransitionCnt + 2 * j) == (j < n ? kLcD[j] : kLcC[j]);
        }
        if (ok) k = n;
      }
    }
    // The state.
    int s = -1;
    for (int t = 0; t < 21 && s < 0; ++t) {
      bool ok = true;
      for (int i = 0; i < 20 && ok; ++i) {
        uint16_t w = o->half(kEg2OtpLcState + 2 * i);
        char c = kLcStateWords[t][i];
        ok = w == (c == '0' ? 0 : c == 'A' ? kLcA[i] : kLcB[i]);
      }
      if (ok) s = t;
    }
    if (k < 0 || s < 0) return;
    count_ = uint32_t(k);
    state_dec_ = count_ >= 24 ? uint32_t(kLcScrap) : uint32_t(s);
  }

  void publish() {
    set_value(state_, rep5(state()));
    set_value(cnt_, post_transition_ ? 31u : count_);
  }

  // The allowed transitions; 0: illegal, 1: unconditional, 2..5: the token's index.
  enum Tok { kIllegal = 0, kUncond, kRawUnlock, kTestUnlock, kTestExit, kRmaUnlock };
  static int rule(uint32_t from, uint32_t to) {
    bool tu_to = eg2_lc_test_unlocked(to), tl_to = eg2_lc_test_locked(to);
    bool mission_to = to == kLcDev || to == kLcProd || to == kLcProdEnd;
    if (to == kLcScrap) return from == kLcScrap ? kIllegal : kUncond;
    if (from == kLcRaw) return tu_to ? kRawUnlock : kIllegal;
    if (eg2_lc_test_locked(from)) {
      if (tu_to && to > from) return kTestUnlock;
      return mission_to ? kTestExit : kIllegal;
    }
    if (eg2_lc_test_unlocked(from)) {
      if (tl_to && to > from) return kUncond;
      if (mission_to) return kTestExit;
      return to == kLcRma ? kUncond : kIllegal;
    }
    if (from == kLcDev || from == kLcProd) return to == kLcRma ? kRmaUnlock : kIllegal;
    return kIllegal;
  }

  static std::vector<uint8_t> hash_token(const std::vector<uint8_t> &token) {
    // theory_of_operation.md, "Token Hashing Mechanism": "a 128bit cSHAKE hash with the
    // function name \"\" (empty string) and customization string \"LC_CTRL\"".
    digestpp::cshake128 shake;
    shake.set_function_name("", 0);
    shake.set_customization("LC_CTRL", 7);
    shake.absorb(token.data(), token.size());
    std::vector<uint8_t> out(16);
    shake.squeeze(out.data(), out.size());
    return out;
  }

  bool parse_token(const std::string &v, std::vector<uint8_t> *token) {
    token->assign(16, 0);
    if (v.rfind("sym.", 0) == 0) {
      return sys_.read_test_symbol && sys_.read_test_symbol(v.substr(4), token->data(), 16);
    }
    std::string h = v.rfind("0x", 0) == 0 ? v.substr(2) : v;
    if (h.size() != 32 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
      return false;
    }
    for (unsigned i = 0; i < 16; ++i) {
      (*token)[i] = uint8_t(std::stoul(h.substr(2 * i, 2), nullptr, 16));
    }
    return true;
  }

  bool provision_token(uint32_t off, const std::string &v) {
    Eg2OtpCtrl *o = otp();
    std::vector<uint8_t> token;
    if (!o || !parse_token(v, &token)) return false;
    o->provision_lc_token(off, hash_token(token));
    return true;
  }

  bool write_otp_state(uint32_t s) {
    Eg2OtpCtrl *o = otp();
    if (!o || s > kLcScrap) return false;
    for (int i = 0; i < 20; ++i) {
      char t = kLcStateWords[s][i];
      o->set_half(kEg2OtpLcState + 2 * i, t == '0' ? 0 : t == 'A' ? kLcA[i] : kLcB[i]);
    }
    return true;
  }

  bool write_otp_count(uint32_t count) {
    Eg2OtpCtrl *o = otp();
    if (!o || count > 24) return false;
    for (int j = 0; j < 24; ++j) {
      o->set_half(kEg2OtpLcTransitionCnt + 2 * j,
                  count == 0 ? 0 : j < int(count) ? kLcD[j] : kLcC[j]);
    }
    return true;
  }

  void transition(uint32_t tgt_raw, const std::vector<uint8_t> &token, bool volatile_unlock) {
    Eg2OtpCtrl *o = otp();
    uint32_t st = value(status_) & 0x3;
    uint32_t to = tgt_raw & 0x1f;
    if (volatile_unlock) {
      // programmers_guide.md, "Volatile RAW -> TEST_UNLOCKED0 Transition": "The
      // LC_TRANSITION_CNT will not be incremented and the life cycle state will not be moved to
      // the temporary POST_TRANSITION state."
      bool ok = state_dec_ == kLcRaw && to == kLcTestUnlocked0 && !raw_unlock_hash_.empty() &&
                hash_token(token) == raw_unlock_hash_;
      if (ok) {
        state_dec_ = kLcTestUnlocked0;
        set_value(status_, st | (1u << 3));
        publish();
        sys_.release_halt("lc_ctrl");
      } else {
        set_value(status_, st | (1u << 6));
      }
      return;
    }
    if (!o || state_dec_ == kLcInvalid || state_dec_ > kLcScrap) {
      st |= 1u << 5;  // TRANSITION_ERROR
    } else if (count_ >= 24) {
      st |= 1u << 4;  // TRANSITION_COUNT_ERROR
    } else {
      // theory_of_operation.md, "Life Cycle Transition Count": "Upon each life cycle transition
      // attempt, the life cycle controller FIRST increments the transition counter before
      // initiating any token hashing and comparison operations."
      ++count_;
      for (int j = 0; j < 24; ++j) {
        o->set_half(kEg2OtpLcTransitionCnt + 2 * j, j < int(count_) ? kLcD[j] : kLcC[j]);
      }
      int tok = (rep5(to) == tgt_raw && to <= kLcScrap) ? rule(state_dec_, to) : int(kIllegal);
      bool otp_ok = true;
      // device_life_cycle/README.md, "Life Cycle Encoding": "when OTP states are transitioned
      // from A to B, we cannot go back to A from B."
      for (int i = 0; tok != kIllegal && i < 20; ++i) {
        char f = kLcStateWords[state_dec_][i], t = kLcStateWords[to][i];
        otp_ok = otp_ok && (f == t || f == '0' || (f == 'A' && t == 'B'));
      }
      if (tok == kIllegal || !otp_ok) {
        st |= 1u << 5;
        sys_.note("lc_ctrl: rejected illegal transition " + std::to_string(state_dec_) + " -> " +
                  std::to_string(to));
      } else if ((tok == kTestUnlock || tok == kTestExit) &&
                 !o->partition_locked(kEg2OtpSecret0)) {
        st |= 1u << 6;  // TOKEN_ERROR
        sys_.note("lc_ctrl: rejected transition because SECRET0 is not locked");
      } else if (tok == kRmaUnlock && !o->partition_locked(kEg2OtpSecret2)) {
        st |= 1u << 6;
        sys_.note("lc_ctrl: rejected transition because SECRET2 is not locked");
      } else {
        std::vector<uint8_t> expected;
        if (tok == kRawUnlock) expected = raw_unlock_hash_;
        else if (tok == kTestUnlock) expected = o->lc_token(kEg2OtpParts[kEg2OtpSecret0].off);
        else if (tok == kTestExit) {
          expected = o->lc_token(kEg2OtpParts[kEg2OtpSecret0].off + 16);
        } else if (tok == kRmaUnlock) {
          expected = o->lc_token(kEg2OtpParts[kEg2OtpSecret2].off);
        }
        if (tok != kUncond && (expected.empty() || hash_token(token) != expected)) {
          st |= 1u << 6;
          sys_.note("lc_ctrl: rejected transition " + std::to_string(state_dec_) + " -> " +
                    std::to_string(to) + " because its token hash did not match OTP");
        } else if (program_error_) {
          // registers.md, STATUS.OTP_ERROR: "This bit is set to 1 if an error occurred during an
          // OTP programming operation." It raises fatal_prog_error and leaves the old state in OTP.
          st |= 1u << 8;
          raise_alert("fatal_prog_error");
          program_error_ = false;
        } else if (to == kLcRma && sys_.nvm_rma && !sys_.nvm_rma()) {
          st |= 1u << 7;
          sys_.note("lc_ctrl: RMA transition failed because the NVM wipe failed");
        } else {
          write_otp_state(to);
          st |= 1u << 3;
          sys_.note("lc_ctrl: programmed OTP life-cycle state " + std::to_string(to));
        }
      }
    }
    if (value(ctrl_) & 1) st |= 1u << 2;  // EXT_CLOCK_SWITCHED
    post_transition_ = true;
    set_value(status_, st);
    set_value(trans_regwen_, 0);
    publish();
    // programmers_guide.md, "Regular Life Cycle Transitions": "all life cycle state transition
    // increments the LC_TRANSITION_CNT and moves the life cycle state into the temporary
    // POST_TRANSITION state - even if the transition was unsuccessful."
    sys_.halt_core("lc_ctrl", "lc_cpu_en off in POST_TRANSITION until reset");
    sys_.chip_event("lc_transition_" + std::to_string(to));
    sys_.chip_event("lc_transition");
  }

  uint16_t status_, claim_regwen_, claim_, trans_regwen_, cmd_, ctrl_, token_, target_, state_, cnt_,
      id_, rev0_, rev1_, dev_id_, manuf_;
  uint32_t state_dec_ = kLcInvalid, count_ = 31;
  bool post_transition_ = false, escalated_ = false, program_error_ = false;
  std::vector<uint8_t> raw_unlock_hash_, auto_test_unlock_token_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_LC_CTRL_H_
