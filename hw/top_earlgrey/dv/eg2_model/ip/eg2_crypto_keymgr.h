// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// keymgr_dpe and otbn of the EG2 SystemC model (Phase B, crypto group).
//
// keymgr_dpe  From hw/ip/keymgr_dpe/doc/theory_of_operation.md and keymgr_dpe.hjson: the working
//             states Reset / Available / Disabled / Invalid, the slots (valid, boot_stage,
//             max_key_version, key_policy), and the commands Advance, Erase Slot, Generate SW
//             Output, Generate HW Output, Disable and Load Root Key with the validity rules of the
//             theory of operation. Every operation completes at once: OP_STATUS reports
//             DONE_SUCCESS or DONE_ERROR (ERR_CODE.INVALID_OP, alert recov_operation_err) and
//             op_done fires.
//             The KDF is KMAC256 (Eg2Kmac::app_kmac) with the parent's key as key and the message
//             of the theory of operation ("Advance", "Versioned Key Generation"). Its inputs come
//             from the models the top connects: the UDS is otp_ctrl's creator root key
//             (Eg2OtpCtrl::creator_root_key; not valid while SECRET2 is unlocked, and then the
//             latching advance moves the key manager to Invalid: "If the OTP creator root key is
//             not valid during the latching cycle, keymgr_dpe moves to Invalid"), device_identifier
//             otp_ctrl's DEVICE_ID, rom_descriptors rom_ctrl's DIGEST, creator_seed and owner_seed
//             rram_ctrl's seeds (the top wires them from rram_ctrl, the doc names SECRET2/SECRET3;
//             an invalid seed counts as zero). lc_ctrl's keymgr_en deactivates the key manager
//             (Invalid, everything wiped: "Life Cycle Connection"). Netlist constants stay
//             stand-ins, SHAKE256("EG2 model keymgr_dpe <name>"): hw_revision_seed, dest_seed,
//             output_key, and health_st_measurement, lc_ctrl's 128-bit diversification constant per
//             life-cycle group (invalid, TEST_UNLOCKED*, DEV, PROD/PROD_END, RMA; interfaces.md
//             RndCnstLcKeymgrDiv*). The padding to AdvDataWidth / GenDataWidth is not reproduced
//             and the OTP shares are not descrambled, so the keys are not the chip's keys.
//             Generated keys: share 0 holds the key, share 1 zero.
//             NumBootStages = 3 (Earl Grey: keymgr_dpe_earlgrey).
// otbn        Not modelled (open decision 6: OTBN's reference model is the Python ISS). The
//             register interface is: the secure wipe after reset (STATUS busy until edn0 gives
//             URND, then idle), CMD SEC_WIPE_DMEM / SEC_WIPE_IMEM (clear the memory, done), and
//             CMD EXECUTE stops the run with that reason. IMEM and DMEM are plain memory;
//             LOAD_CHECKSUM is not computed.

#ifndef EG2_MODEL_IP_EG2_CRYPTO_KEYMGR_H_
#define EG2_MODEL_IP_EG2_CRYPTO_KEYMGR_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_crypto_aes.h"
#include "eg2_crypto_entropy.h"
#include "eg2_crypto_hash.h"
#include "eg2_mem.h"

namespace eg2_model {

class Eg2Otbn : public Eg2IpModel {
 public:
  enum Cmd { kExecute = 0xd8, kSecWipeDmem = 0xc3, kSecWipeImem = 0x1e };
  enum Status { kIdle = 0, kBusyExecute = 1, kBusySecWipeInt = 4 };

  Eg2Otbn(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    cmd_ = reg("CMD");
    status_ = reg("STATUS");
    set_modelled(status_);
    imem_ = uint16_t(win_index("IMEM"));
    dmem_ = uint16_t(win_index("DMEM"));
    sys_.schedule(this, kRetryPs);
  }

  // Sideloaded key from keymgr: accepted, unused (OTBN does not execute).
  void set_sideload_key(const uint32_t *, const uint32_t *, bool) {}

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    urnd_words_ = 0;  // STATUS resets to busy_sec_wipe_int
    sys_.schedule(this, sys_.now_ps() + kRetryPs);
  }
  // The start-up secure wipe runs on its own once edn0 delivers URND.
  void on_time(uint64_t now) override {
    finish_wipe();
    if (value(status_) == kBusySecWipeInt) sys_.schedule(this, now + kRetryPs);
  }
  static constexpr uint64_t kRetryPs = 1000000;  // 1 us

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r != cmd_ || value(status_) != kIdle) return;
    switch ((wdata & wmask) & 0xff) {
      case kExecute:
        Eg2ModelStop::request(std::string(desc().window) +
                              ": software starts an OTBN program; OTBN execution is not "
                              "modelled (open decision 6: its model is the Python ISS)");
        break;
      case kSecWipeDmem:
        wipe(dmem_);
        break;
      case kSecWipeImem:
        wipe(imem_);
        break;
      default:
        break;
    }
  }
  void reg_before_read(uint16_t r) override {
    if (r == status_) finish_wipe();
  }
  void finish_wipe() {
    if (value(status_) != kBusySecWipeInt) return;
    auto *edn = dynamic_cast<Eg2Edn *>(sys_.model("edn0"));
    uint32_t w;
    while (urnd_words_ < 8 && edn && edn->get(&w)) ++urnd_words_;
    if (urnd_words_ == 8) set_value(status_, kIdle);
  }

 private:
  void wipe(uint16_t win) {
    std::vector<uint8_t> &m = win_mem(win);
    std::fill(m.begin(), m.end(), 0);
    set_intr("done");
  }
  uint16_t cmd_, status_, imem_, dmem_;

 public:
  bool idle() const override { return value(status_) == kIdle; }
  unsigned urnd_words_ = 0;
};

// ---------------------------------------------------------------------------------------------
class Eg2KeymgrDpe : public Eg2IpModel {
 public:
  static constexpr unsigned kSlots = 4, kNumBootStages = 3;
  enum State { kReset = 0, kAvailable = 1, kDisabled = 2, kInvalid = 3 };
  enum Op { kAdvance = 0, kErase = 1, kGenSw = 2, kGenHw = 3, kDisable = 4, kLoadRoot = 5 };
  enum Dest { kNone = 0, kAes = 1, kKmac = 2, kOtbn = 3, kHmac = 4 };

  Eg2KeymgrDpe(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    start_ = reg("START");
    control_ = reg("CONTROL_SHADOWED");
    slot_policy_ = reg("SLOT_POLICY");
    key_version_ = reg("KEY_VERSION");
    max_key_ver_ = reg("MAX_KEY_VER_SHADOWED");
    working_state_ = reg("WORKING_STATE");
    op_status_ = reg("OP_STATUS");
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
    for (uint16_t r : {start_, working_state_, op_status_, err_code_}) set_modelled(r);
    update();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    state_ = kReset;
    root_loaded_ = false;
    for (auto &s : slots_) s = Slot();
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == start_ && (value(start_) & 1)) {
      bool ok = operate();
      // A successful advance unlocks SW_BINDING and SLOT_POLICY again (keymgr_dpe.hjson: "unlocked
      // by hardware upon a successful advance call", "After a successful advance operation, this
      // register is unlocked again"); for MAX_KEY_VER the hjson is silent, the RTL does the same
      // (keymgr_dpe.sv: all three prim_flop_2sync set by unlock_after_advance).
      if (ok && op() == kAdvance && state_ == kAvailable) {
        for (uint16_t rw : regwen_) set_value(rw, 1);
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
      // VAL selects which sideload slot reads as cleared (a value other than 0-4 clears all).
      uint32_t v = value(sideload_clear_) & 7;
      if (v == 0 || v > 4) {
        for (unsigned dst = kAes; dst <= kHmac; ++dst) sideload(dst, nullptr);
      } else {
        sideload(v, nullptr);
      }
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

  static std::vector<uint8_t> standin(const char *name, size_t n = 32) {
    digestpp::shake256 h;
    std::string tag = std::string("EG2 model keymgr_dpe ") + name;
    h.absorb(reinterpret_cast<const unsigned char *>(tag.data()), tag.size());
    std::vector<uint8_t> out(n);
    h.squeeze(out.data(), n);
    return out;
  }
  std::vector<uint8_t> device_id() {
    auto *otp = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    return otp ? otp->device_id() : std::vector<uint8_t>(32, 0);
  }
  std::vector<uint8_t> health() {
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    uint32_t s = lc ? lc->state() : kLcInvalid;
    const char *g = eg2_lc_test_unlocked(s) ? "health_test_unlocked"
                    : s == kLcDev                          ? "health_dev"
                    : s == kLcProd || s == kLcProdEnd      ? "health_production"
                    : s == kLcRma                          ? "health_rma"
                                                           : "health_invalid";
    return standin(g, 16);
  }
  std::vector<uint8_t> rom_digest() {
    std::vector<uint8_t> d(32, 0);
    Eg2IpModel *rom = sys_.model("rom_ctrl.regs");
    int r = rom ? rom->reg_index("DIGEST_0") : -1;
    for (unsigned i = 0; r >= 0 && i < 8; ++i) {
      uint32_t w = rom->value(uint16_t(r + int(i)));
      for (unsigned b = 0; b < 4; ++b) d[4 * i + b] = uint8_t(w >> (8 * b));
    }
    return d;
  }
  std::vector<uint8_t> seed(bool owner) {
    std::vector<uint8_t> s(32, 0);
    auto *rram = dynamic_cast<Eg2RramCtrl *>(sys_.model("rram_ctrl.core"));
    if (rram && !rram->keymgr_seed(owner, &s)) s.assign(32, 0);
    return s;
  }
  static void append(std::vector<uint8_t> &m, const std::vector<uint8_t> &x) {
    m.insert(m.end(), x.begin(), x.end());
  }
  void append_words(std::vector<uint8_t> &m, const uint16_t *regs, unsigned n) const {
    for (unsigned i = 0; i < n; ++i) {
      for (unsigned b = 0; b < 4; ++b) m.push_back(uint8_t(value(regs[i]) >> (8 * b)));
    }
  }

  unsigned op() const { return (value(control_) >> 4) & 7; }
  unsigned dest() const { return (value(control_) >> 11) & 7; }
  unsigned src() const { return (value(control_) >> 14) & 7; }
  unsigned dst() const { return (value(control_) >> 18) & 7; }
  bool sw_binding_only() const { return (value(control_) >> 22) & 1; }

  // lc_ctrl's keymgr_en: off moves the key manager to Invalid and wipes it.
  bool lc_check() {
    auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    if (lc && !lc->keymgr_en() && state_ != kInvalid) invalidate();
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
    s.max_key_version = value(max_key_ver_);
    s.policy = 1;
    s.key = uds;
    state_ = kAvailable;
    root_loaded_ = true;
    return true;
  }
  bool advance() {
    if (src() >= kSlots || dst() >= kSlots) return false;
    Slot &p = slots_[src()];
    // No boot-stage limit. theory_of_operation.md lists "boot_stage + 1 < NumBootStages" as an
    // advance condition, but also has advances "from parents with boot_stage > 2" (message =
    // SW_CDI_INPUT) and keymgr_dpe_key_derivation_test advances its stage-3 OwnerKey; the RTL
    // (keymgr_dpe_ctrl.sv next_boot_stage) saturates boot_stage at BootStageRuntime (3).
    if (!p.valid || !(p.policy & 1)) return false;
    bool retain = p.policy & 4;
    if (retain && src() == dst()) return false;
    if (slots_[dst()].valid && dst() != src() && (slots_[dst()].policy & 4)) return false;
    bool enforce = mubi4_true(value(enforce_sw_binding_));  // a MuBi4 (keymgr_dpe.hjson)
    if (enforce && p.boot_stage <= 2 && !sw_binding_only()) return false;
    // message = SW_CDI_INPUT || stage inputs (NumBootStages = 3).
    std::vector<uint8_t> msg;
    append_words(msg, sw_binding_, 8);
    if (!sw_binding_only()) {
      if (p.boot_stage == 0) {
        append(msg, device_id());
        append(msg, health());
        append(msg, rom_digest());
        append(msg, standin("hw_revision_seed"));
      } else if (p.boot_stage == 1) {
        append(msg, seed(false));
      } else if (p.boot_stage == 2) {
        append(msg, seed(true));
      }
    }
    Slot child;
    child.valid = true;
    child.boot_stage = p.boot_stage < kNumBootStages ? p.boot_stage + 1 : kNumBootStages;
    child.max_key_version = value(max_key_ver_);
    child.policy = value(slot_policy_) & 0xf;
    child.key = kdf(p.key, msg);
    if (!retain) p = Slot();
    slots_[dst()] = child;
    return true;
  }
  bool generate(bool hw) {
    if (src() >= kSlots) return false;
    const Slot &s = slots_[src()];
    if (!s.valid || !(s.policy & 8) || value(key_version_) > s.max_key_version) return false;
    if (hw && (dest() == kNone || dest() > kHmac)) return false;
    static const char *kDest[] = {"dest_none", "dest_aes", "dest_kmac", "dest_otbn", "dest_hmac"};
    std::vector<uint8_t> msg;
    append_words(msg, &key_version_, 1);
    append_words(msg, salt_, 8);
    append(msg, standin(kDest[dest() <= kHmac ? dest() : 0]));
    append(msg, standin(hw ? "output_hw" : "output_sw"));
    std::vector<uint8_t> key = kdf(s.key, msg);
    if (hw) {
      // HMAC's sideload interface is wide (512 bits, wide_hw_key_req): the whole KDF output.
      wide_key_ = Eg2Kmac::app_kmac(s.key, {}, msg, 64);
      sideload(dest(), &key);
    } else {
      for (unsigned i = 0; i < 8; ++i) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; ++b) w |= uint32_t(key[4 * i + b]) << (8 * b);
        set_value(out0_[i], w);
        set_value(out1_[i], 0);
      }
    }
    return true;
  }
  static std::vector<uint8_t> kdf(const std::vector<uint8_t> &key,
                                  const std::vector<uint8_t> &msg) {
    std::vector<uint8_t> d = Eg2Kmac::app_kmac(key, {}, msg, 64);
    d.resize(32);
    return d;
  }
  // Load (key != nullptr) or clear a sideload slot.
  void sideload(unsigned dst, const std::vector<uint8_t> *key) {
    uint32_t s0[8] = {}, s1[8] = {};
    for (unsigned i = 0; key && i < 8; ++i) {
      for (unsigned b = 0; b < 4; ++b) s0[i] |= uint32_t((*key)[4 * i + b]) << (8 * b);
    }
    bool valid = key != nullptr;
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
          for (unsigned i = 0; valid && i < 16 && 4 * i + 3 < wide_key_.size(); ++i) {
            for (unsigned b = 0; b < 4; ++b) w[i] |= uint32_t(wide_key_[4 * i + b]) << (8 * b);
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
    for (unsigned i = 0; i < 8; ++i) {
      const Slot *s = i < kSlots ? &slots_[i] : nullptr;
      set_value(meta_lo_[i], s ? s->max_key_version : 0);
      // METADATA_HIGH: VALID (0), BOOT_STAGE (2:1), ALLOW_CHILD (3), EXPORTABLE (4),
      // RETAIN_PARENT (5), ALLOW_KEYGEN (6).
      uint32_t hi = 0;
      if (s && s->valid) hi = 1u | ((s->boot_stage & 3) << 1) | ((s->policy & 0xf) << 3);
      set_value(meta_hi_[i], hi);
    }
  }

  uint16_t start_, control_, slot_policy_, key_version_, max_key_ver_, working_state_, op_status_,
      err_code_, sideload_clear_, load_root_lock_, enforce_sw_binding_, regwen_[3];
  uint16_t sw_binding_[8], salt_[8], out0_[8], out1_[8], meta_lo_[8], meta_hi_[8];
  std::array<Slot, kSlots> slots_;
  State state_ = kReset;
  bool root_loaded_ = false;
  std::vector<uint8_t> wide_key_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CRYPTO_KEYMGR_H_
