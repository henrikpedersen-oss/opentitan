// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// keymgr_dpe  From hw/ip/keymgr_dpe/doc/theory_of_operation.md and keymgr_dpe.hjson: the working
//             states Reset / Available / Disabled / Invalid, the slots (valid, boot_stage,
//             max_key_version, key_policy), and the commands Advance, Erase Slot, Generate SW
//             Output, Generate HW Output, Disable and Load Root Key with the validity rules of the
//             theory of operation. Every operation completes at once: OP_STATUS reports
//             DONE_SUCCESS or DONE_ERROR (ERR_CODE.INVALID_OP, alert recov_operation_err) and
//             op_done fires.
//             Root loading uses otp_ctrl's descrambled creator root shares. A root
//             slot starts at stage 0, policy allow_child, max_key_version 0 (ToO,
//             "Load Root Key"). Lifecycle enable and CFG_REGWEN gating always
//             apply; there is no stimulus switch to disable them. Operations are
//             synchronous, so the busy lock is observable only during a command.
//
//             Derivation is deliberately stopped as undocumented. The ToO defines
//             KMAC256, a 256-bit key and 512-bit result; advance messages concatenate
//             SW_CDI_INPUT with device ID, lifecycle measurement, ROM descriptors
//             and revision seed at Creator, then field entropy / creator seed /
//             owner seed at the three following stages, or SW_CDI_INPUT alone.
//             Generate concatenates KEY_VERSION, SALT, destination and output seeds.
//             It requires zero-padding to AdvDataWidth / GenDataWidth but neither
//             the documents nor integration HJSON define those widths, serialization
//             or padding placement. The docs also name OTP seed partitions that
//             differ from the former model's RRAM binding. See ticket KEYMGR-1.
//             No value is imported from RTL and no approximate key is returned.
//             The old SHAKE-derived netlist stand-ins and unpadded KDF are removed.
//             Derivation can resume only after that input contract is documented.
//
// Shadowed configuration uses the shared per-field two-write protocol, with
// reads/reset cancelling staging (util/reggen/README.md, "Shadow registers").
// A mismatch sets ERR_CODE.INVALID_SHADOW_UPDATE and raises recov_operation_err
// (keymgr_dpe.hjson). This asynchronous error does not complete an operation;
// software explicitly clears ERR_CODE with RW1C. Storage faults are not modelled.
//
#ifndef EG2_MODEL_IP_EG2_KEYMGR_DPE_H_
#define EG2_MODEL_IP_EG2_KEYMGR_DPE_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_crypto_aes.h"
#include "eg2_edn.h"
#include "eg2_hmac.h"
#include "eg2_kmac.h"
#include "eg2_mem_otp.h"
#include "eg2_rram_ctrl.h"
#include "eg2_otbn_ctrl.h"

namespace eg2_model {

class Eg2KeymgrDpe : public Eg2IpModel {
 public:
  // kNumBootStages is the highest stage, Runtime (4), at which an advance saturates.
  static constexpr unsigned kSlots = 4, kNumBootStages = 4;
  enum State { kReset = 0, kAvailable = 1, kDisabled = 2, kInvalid = 3 };
  enum Op { kAdvance = 0, kErase = 1, kGenSw = 2, kGenHw = 3, kDisable = 4, kLoadRoot = 5 };
  enum Dest { kNone = 0, kAes = 1, kKmac = 2, kOtbn = 3, kHmac = 4 };

  Eg2KeymgrDpe(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    enable_shadowed_registers();
    start_ = reg("START");
    control_ = reg("CONTROL_SHADOWED");
    slot_policy_ = reg("SLOT_POLICY");
    key_version_ = reg("KEY_VERSION");
    max_key_ver_ = reg("MAX_KEY_VER_SHADOWED");
    working_state_ = reg("WORKING_STATE");
    op_status_ = reg("OP_STATUS");
    cfg_regwen_ = reg("CFG_REGWEN");
    err_code_ = reg("ERR_CODE");
    sideload_clear_ = reg("SIDELOAD_CLEAR");
    load_root_lock_ = reg("LOAD_ROOT_KEY_LOCK");
    enforce_sw_binding_ = reg("ENFORCE_SW_BINDING");
    regwen_[0] = reg("SW_BINDING_REGWEN");
    regwen_[1] = reg("SLOT_POLICY_REGWEN");
    regwen_[2] = reg("MAX_KEY_VER_REGWEN");
    for (unsigned i = 0; i < 8; ++i) {
      sw_binding_[i] = reg(("SW_BINDING_" + std::to_string(i)).c_str());
      salt_[i] = reg(("SALT_" + std::to_string(i)).c_str());
      out0_[i] = reg(("SW_SHARE0_OUTPUT_" + std::to_string(i)).c_str());
      out1_[i] = reg(("SW_SHARE1_OUTPUT_" + std::to_string(i)).c_str());
      meta_lo_[i] = reg(("METADATA_LOW_" + std::to_string(i)).c_str());
      meta_hi_[i] = reg(("METADATA_HIGH_" + std::to_string(i)).c_str());
      for (uint16_t r : {out0_[i], out1_[i], meta_lo_[i], meta_hi_[i]}) set_modelled(r);
    }
    for (uint16_t r : {start_, working_state_, op_status_, err_code_, cfg_regwen_}) set_modelled(r);
    // Compatibility for old stimulus files: accepting 1 is harmless; 0 cannot
    // disable a documented access policy. Configuration is always enforced.
    sys_.stim_handlers["keymgr.lifecycle_gating"] = [](const std::string &v) {
      return v == "1";
    };
    update();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    state_ = kReset;
    root_loaded_ = false;
    for (auto &s : slots_) s = Slot();
    busy_ = false;
    clearing_ = 0;  // SIDELOAD_CLEAR resets to None
    update();
  }

 protected:
  bool access_frozen() override {
    if (Eg2IpModel::access_frozen()) return true;
    lc_check();
    update();  // refresh REGWEN before the generic block checks it, even without a preceding read
    return false;
  }
  void shadow_update(uint16_t, bool error) override {
    if (!error) return;
    set_value(err_code_, value(err_code_) | (1u << 2));
    raise_alert("recov_operation_err");
  }
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == start_ && (value(start_) & 1)) {
      busy_ = true;
      update();
      bool ok = operate();
      if (!Eg2ModelStop::reason().empty()) return;  // no invented hardware completion/error
      busy_ = false;
      // Only these two locks have documented hardware unlocking after advance.
      // MAX_KEY_VER_REGWEN's missing unlock rule is part of KEYMGR-1.
      if (ok && op() == kAdvance && state_ == kAvailable) {
        set_value(regwen_[0], 1);
        set_value(regwen_[1], 1);
      }
      set_value(start_, 0);
      set_value(op_status_, ok ? 2 : 3);  // DONE_SUCCESS / DONE_ERROR
      if (!ok) {
        // INVALID_OP, a recoverable error, signalled by recov_operation_err ("Alert for key
        // manager operation errors. These errors could have been caused by software",
        // interfaces.md).
        set_value(err_code_, value(err_code_) | 1);
        raise_alert("recov_operation_err");
      }
      set_intr("op_done");
    } else if (r == sideload_clear_) {
      // SIDELOAD_CLEAR.VAL (keymgr_dpe.hjson): None (0) clears nothing; AES, KMAC, OTBN, HMAC
      // (1-4): that slot "is continuously cleared with entropy" until software selects another
      // value; any other value clears all of them. The documents do not say what the destination
      // sees meanwhile; OpenTitan's chip tests do (EG2 spec GAP-RTLREQ-12): while a slot is being
      // cleared its key is not valid (keymgr_dpe_sideload_otbn_test expects OTBN's KEY_INVALID).
      // Each selected slot is filled from edn0 but remains invalid, and retains that data after
      // clearing stops (EG2 spec GAP-RTLREQ-12).
      uint32_t v = value(sideload_clear_) & 7;
      unsigned now_clearing = v == 0 ? 0u : v > 4 ? 0x1eu : (1u << v);
      for (unsigned dst = kAes; dst <= kHmac; ++dst) {
        bool was = (clearing_ >> dst) & 1, is = (now_clearing >> dst) & 1;
        if (is && !was) clear_with_entropy(dst);
      }
      clearing_ = now_clearing;
    }
    update();
  }

 private:
  struct Slot {
    bool valid = false;
    unsigned boot_stage = 0;
    uint32_t max_key_version = 0;
    unsigned policy = 0;  // allow_child (0), exportable (1), retain_parent (2), allow_keygen (3)
    std::vector<uint8_t> key;
  };

  unsigned op() const { return (value(control_) >> 4) & 7; }
  unsigned dest() const { return (value(control_) >> 11) & 7; }
  unsigned src() const { return (value(control_) >> 14) & 7; }
  unsigned dst() const { return (value(control_) >> 18) & 7; }
  bool sw_binding_only() const { return (value(control_) >> 22) & 1; }

  bool lc_enabled() const {
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    return !lc || lc->keymgr_en();
  }

  // theory_of_operation.md, "Life Cycle Connection": "Until the life cycle controller activates
  // the key manager, the key manager does not accept any software commands." "When the life cycle
  // controller deactivates the key manager, the key manager transitions to the `Invalid` state."
  bool lc_check() {
    if (!lc_enabled()) {
      if (state_ != kReset && state_ != kInvalid) invalidate();
      return false;
    }
    return state_ != kInvalid;
  }
  void invalidate() {
    state_ = kInvalid;
    for (auto &s : slots_) s = Slot();
    for (unsigned dst = kAes; dst <= kHmac; ++dst) sideload(dst, nullptr);
    for (unsigned i = 0; i < 8; ++i) {
      set_value(out0_[i], 0);
      set_value(out1_[i], 0);
    }
  }
  void reg_before_read(uint16_t r) override {
    if (r == working_state_) {
      lc_check();
      update();
    } else if (r == cfg_regwen_) {
      set_value(cfg_regwen_, lc_enabled() && !busy_ ? 1 : 0);
    }
  }

  bool operate() {
    if (!lc_check() || state_ == kDisabled) return false;
    switch (op()) {
      case kAdvance: return state_ == kReset ? load_root() : advance();
      case kLoadRoot: return state_ == kAvailable && !(value(load_root_lock_) & 1) && load_root();
      case kErase:
        if (state_ != kAvailable || dst() >= kSlots || !slots_[dst()].valid) return false;
        slots_[dst()] = Slot();
        return true;
      case kGenSw:
      case kGenHw: return state_ == kAvailable && generate(op() == kGenHw);
      case kDisable:
        if (state_ != kAvailable) return false;
        state_ = kDisabled;
        for (auto &s : slots_) s = Slot();  // slots wiped, outputs kept
        return true;
      default: return false;
    }
  }
  // The first advance, or Load Root Key: the UDS into SLOT_DST_SEL, boot stage 0, the fixed UDS
  // policy (allow_child only).
  bool load_root() {
    if (dst() >= kSlots) return false;
    std::vector<uint8_t> uds;
    auto *otp = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    if (!otp || !otp->creator_root_key(&uds)) {
      invalidate();  // silently: the operation itself completes (DONE_SUCCESS)
      return true;
    }
    Slot &s = slots_[dst()];
    s.valid = true;
    s.boot_stage = 0;
    s.max_key_version = 0;  // fixed UDS default, independent of MAX_KEY_VER_SHADOWED
    s.policy = 1;
    s.key = uds;
    state_ = kAvailable;
    root_loaded_ = true;
    return true;
  }
  bool advance() {
    if (src() >= kSlots || dst() >= kSlots) return false;
    Slot &p = slots_[src()];
    if (!p.valid || !(p.policy & 1)) return false;
    bool retain = p.policy & 4;
    if (retain ? (src() == dst() || slots_[dst()].valid) : src() != dst()) return false;
    bool enforce = mubi4_true(value(enforce_sw_binding_));  // a MuBi4 (keymgr_dpe.hjson)
    if (enforce && p.boot_stage <= 3 && !sw_binding_only()) return false;
    return stop_derivation("advance: AdvDataWidth and padded input layout");
  }
  bool generate(bool hw) {
    if (src() >= kSlots) return false;
    const Slot &s = slots_[src()];
    if (!s.valid || !(s.policy & 8) || value(key_version_) > s.max_key_version) return false;
    if (dest() > kHmac || (hw && dest() == kNone)) return false;
    return stop_derivation("generate: GenDataWidth and padded input layout");
  }
  static bool stop_derivation(const char *missing) {
    Eg2ModelStop::request(std::string("undocumented: keymgr_dpe ") + missing +
                          " (docs name the widths but do not define them; KEYMGR-1)");
    return false;
  }
  // SIDELOAD_CLEAR replaces a selected slot with entropy from edn0 and marks it invalid. Without
  // enough edn0 words the slot is still invalidated.
  void clear_with_entropy(unsigned dst) {
    sideload(dst, nullptr);
    auto *edn = dynamic_cast<Eg2Edn *>(sys_.model("edn0"));
    std::vector<uint8_t> rnd(64);
    for (size_t i = 0; i < rnd.size(); i += 4) {
      uint32_t w;
      if (!edn || !edn->get(&w)) return;
      for (unsigned b = 0; b < 4; ++b) rnd[i + b] = uint8_t(w >> (8 * b));
    }
    sideload(dst, &rnd, false);
  }

  // Load a sideload slot, or clear it when key is null. Data and validity are independent so a
  // slot cleared with entropy can retain that data while invalid.
  void sideload(unsigned dst, const std::vector<uint8_t> *key, bool valid = true) {
    uint32_t s0[8] = {}, s1[8] = {};
    for (unsigned i = 0; key && i < 8; ++i) {
      for (unsigned b = 0; b < 4; ++b) s0[i] |= uint32_t((*key)[4 * i + b]) << (8 * b);
    }
    valid = valid && key != nullptr;
    switch (dst) {
      case kAes:
        if (auto *m = dynamic_cast<Eg2Aes *>(sys_.model("aes"))) m->set_sideload_key(s0, s1, valid);
        break;
      case kKmac:
        if (auto *m = dynamic_cast<Eg2Kmac *>(sys_.model("kmac"))) m->set_sideload_key(s0, s1, valid);
        break;
      case kOtbn:
        if (auto *m = dynamic_cast<Eg2Otbn *>(sys_.model("otbn"))) m->set_sideload_key(s0, s1, valid);
        break;
      case kHmac:
        if (auto *m = dynamic_cast<Eg2Hmac *>(sys_.model("hmac"))) {
          uint32_t w[16] = {};
          for (unsigned i = 0; key && i < 16 && 4 * i + 3 < key->size(); ++i) {
            for (unsigned b = 0; b < 4; ++b) w[i] |= uint32_t((*key)[4 * i + b]) << (8 * b);
          }
          m->set_sideload_key(w, valid);
        }
        break;
      default:
        break;
    }
  }

  void update() {
    set_value(working_state_, state_);
    set_value(cfg_regwen_, lc_enabled() && !busy_ ? 1 : 0);
    for (unsigned i = 0; i < 8; ++i) {
      const Slot *s = i < kSlots ? &slots_[i] : nullptr;
      set_value(meta_lo_[i], s ? s->max_key_version : 0);
      // METADATA_HIGH: VALID (0), BOOT_STAGE (3:1), ALLOW_CHILD (4), EXPORTABLE (5),
      // RETAIN_PARENT (6), ALLOW_KEYGEN (7) (registers.md, since the four-stage merge).
      uint32_t hi = 0;
      if (s && s->valid) hi = 1u | ((s->boot_stage & 7) << 1) | ((s->policy & 0xf) << 4);
      set_value(meta_hi_[i], hi);
    }
  }

  uint16_t start_, control_, slot_policy_, key_version_, max_key_ver_, working_state_, op_status_,
      cfg_regwen_, err_code_, sideload_clear_, load_root_lock_, enforce_sw_binding_, regwen_[3];
  uint16_t sw_binding_[8], salt_[8], out0_[8], out1_[8], meta_lo_[8], meta_hi_[8];
  std::array<Slot, kSlots> slots_;
  State state_ = kReset;
  bool busy_ = false;
  bool root_loaded_ = false;
  unsigned clearing_ = 0;  // the sideload slots SIDELOAD_CLEAR is clearing (bit = destination)
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_KEYMGR_DPE_H_
