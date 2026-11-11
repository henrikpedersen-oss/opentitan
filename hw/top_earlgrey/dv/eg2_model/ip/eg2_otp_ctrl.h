// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// SystemC binding for the single Eg2OtpCtrlCore controller. DAI commands,
// partition locks, PRESENT scrambling/digests, initial buffer validation,
// integrity/consistency checks and zeroization all use the same core as the
// standalone unit tests. Behavioral sources and limitations are detailed in
// eg2_otp_ctrl_core.h and the OTP controller's theory/programmer documents.
//
// Hardware outputs (device ID, config bits, seeds, root shares and lifecycle
// state/tokens) come from checked, reset-sampled buffers. Backdoor provisioning
// modifies only the raw array until reset. Runtime lifecycle programming uses
// the core's LCI interface, which updates both OTP and its lifecycle buffer.
// The adapter maps core CSR names to generated register indices, and forwards
// interrupts and persistent fatal alerts. No controller algorithms live here.
//
// Untimed: commands/check triggers complete synchronously; randomized periodic
// checks, macro ECC and physical programming failures are not modelled. Partial
// zeroization markers, conflicting ZEROIZE readback and unspecified failed-read
// data stop explicitly (OTP-1). Raw image access helpers are for provisioning,
// never hardware output predictions.
#ifndef EG2_MODEL_IP_EG2_OTP_CTRL_H_
#define EG2_MODEL_IP_EG2_OTP_CTRL_H_
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "../eg2_ip_model.h"
#include "eg2_otp_data.h"
#include "eg2_otp_ctrl_core.h"
namespace eg2_model {
class Eg2OtpCtrl : public Eg2IpModel, private Eg2OtpCtrlPort {
 public:
  Eg2OtpCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), core_(this, &win_mem(0)) {
    for (unsigned i = 0; i < Eg2OtpCtrlCore::kNumRegs; ++i) {
      int index = reg_index(Eg2OtpCtrlCore::reg_name(i));
      if (index < 0) throw std::runtime_error("OTP core CSR missing from register description");
      registers_.push_back(uint16_t(index));
      set_modelled(uint16_t(index));
    }
    sys_.stim_handlers["otp.write"] = [this](const std::string &v) { return backdoor_write(v); };
    sys_.stim_handlers["otp.lc_state"] = [this](const std::string &v) { return backdoor_lc_state(v); };
    sys_.stim_handlers["otp.buffer_fault"] = [this](const std::string &v) {
      unsigned p, off, mask;
      if (std::sscanf(v.c_str(), "%u:%x:%x", &p, &off, &mask) != 3 ||
          p >= kEg2OtpNumParts || off >= kEg2OtpParts[p].size || mask > 255) return false;
      core_.inject_buffer_fault(int(p), off, uint8_t(mask));
      return true;
    };
  }
  // The OTP array (also what SW_CFG_WINDOW shows).
  std::vector<uint8_t> &otp() { return win_mem(0); }
  uint8_t byte(uint32_t off) { return off < otp().size() ? otp()[off] : 0; }
  uint16_t half(uint32_t off) { return uint16_t(byte(off) | (byte(off + 1) << 8)); }
  uint32_t word(uint32_t off) { return uint32_t(half(off)) | (uint32_t(half(off + 2)) << 16); }
  uint64_t dword(uint32_t off) { return uint64_t(word(off)) | (uint64_t(word(off + 4)) << 32); }
  void set_half(uint32_t off, uint16_t v) {
    if (off + 1 < otp().size()) {
      otp()[off] = uint8_t(v);
      otp()[off + 1] = uint8_t(v >> 8);
    }
  }
  void set_dword(uint32_t off, uint64_t v) {
    for (unsigned i = 0; i < 8 && off + i < otp().size(); ++i) {
      otp()[off + i] = uint8_t(v >> (8 * i));
    }
  }

  // Backdoor provisioning done by a chip-bench sequence before reset. Token hashes in SECRET0
  // and SECRET2 are stored scrambled (theory_of_operation.md, "Conditional Transitions":
  // "unlock tokens are considered secret" and are wrapped with PRESENT-based scrambling).
  void provision_lc_token(uint32_t off, const std::vector<uint8_t> &hash) {
    int p = part_of(off);
    if ((p != kEg2OtpSecret0 && p != kEg2OtpSecret2) || hash.size() != 16) return;
    for (unsigned w = 0; w < 2; ++w) {
      uint64_t v = 0;
      for (unsigned i = 0; i < 8; ++i) v |= uint64_t(hash[8 * w + i]) << (8 * i);
      set_dword(off + 8 * w, scramble(v, p));
    }
    const Eg2OtpPart &pt = kEg2OtpParts[p];
    const uint32_t digest_offset = eg2_otp_digest_off(pt);
    set_dword(digest_offset, OtpCrypto::digest(otp().data() + pt.off, digest_offset - pt.off));
  }

  std::vector<uint8_t> lc_token(uint32_t off) {
    sync_inputs();
    auto data = core_.lc_data();
    const uint8_t *token = off == kEg2OtpTestUnlockToken && data.test_tokens_valid ? data.test_unlock_token
                         : off == kEg2OtpTestExitToken && data.test_tokens_valid ? data.test_exit_token
                         : off == kEg2OtpRmaToken && data.rma_token_valid ? data.rma_token : nullptr;
    return token ? std::vector<uint8_t>(token, token + 16) : std::vector<uint8_t>();
  }

  void clear_partition(int p) {
    if (p < 0 || p >= kEg2OtpNumParts) return;
    const Eg2OtpPart &pt = kEg2OtpParts[p];
    for (uint32_t i = 0; i < pt.size; ++i) otp()[pt.off + i] = 0;
  }

  bool backdoor_write(const std::string &arg) {
    unsigned off = 0, bytes = 0;
    unsigned long long data = 0;
    if (std::sscanf(arg.c_str(), "%x:%llx:%u", &off, &data, &bytes) != 3 || bytes == 0 ||
        bytes > 8 || uint64_t(off) + bytes > otp().size()) {
      return false;
    }
    for (unsigned i = 0; i < bytes; ++i) otp()[off + i] = uint8_t(data >> (8 * i));
    std::printf("eg2sim: OTP backdoor write offset 0x%03x, %u bytes\n", off, bytes);
    return true;
  }

  bool backdoor_lc_state(const std::string &arg) {
    uint32_t state;
    static const std::map<std::string, uint32_t> kStates = {
        {"Raw", kLcRaw},   {"TestLocked2", 6}, {"Dev", kLcDev},
        {"Prod", kLcProd}, {"Rma", kLcRma},    {"Scrap", kLcScrap},
    };
    auto it = kStates.find(arg);
    if (it != kStates.end()) state = it->second;
    else {
      char *end = nullptr;
      state = uint32_t(std::strtoul(arg.c_str(), &end, 0));
      if (!end || *end) return false;
    }
    if (state > kLcScrap) return false;
    for (unsigned i = 0; i < 20; ++i) {
      char c = kLcStateWords[state][i];
      set_half(kEg2OtpLcState + 2 * i, c == '0' ? 0 : c == 'A' ? kLcA[i] : kLcB[i]);
    }
    std::printf("eg2sim: OTP life-cycle state backdoor-written to %u\n", state);
    return true;
  }


  bool partition_locked(int p) const { return core_.partition_locked(p); }
  bool sram_key_seed_valid() {
    sync_inputs(); uint8_t seed[16]; return core_.sram_key_seed(seed);
  }
  bool en_sram_ifetch() { sync_inputs(); return core_.en_sram_ifetch(); }
  bool creator_root_key(std::vector<uint8_t> *key) {
    sync_inputs();
    uint8_t a[32], b[32]; bool valid = core_.creator_root_key(a, b);
    key->resize(32);
    for (unsigned i = 0; i < 32; ++i) (*key)[i] = a[i] ^ b[i];
    return valid;
  }
  std::vector<uint8_t> device_id() {
    sync_inputs(); std::vector<uint8_t> out(32);
    for (unsigned i = 0; i < 32; ++i) out[i] = core_.hw_cfg_byte(kEg2OtpDeviceId + i);
    return out;
  }
  uint32_t hw_cfg_word(uint32_t off) {
    sync_inputs(); uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) word |= uint32_t(core_.hw_cfg_byte(off + i)) << (8 * i);
    return word;
  }
  Eg2OtpCtrlCore::LcData lc_data() { sync_inputs(); return core_.lc_data(); }
  bool lci_program(const uint16_t words[44]) {
    sync_inputs(); bool ok = core_.lci_program(words); mirror(); return ok;
  }
  void start_of_simulation() override { power_up(); }
  void on_reset(Eg2ResetCause cause) override { Eg2IpModel::on_reset(cause); power_up(); }
 protected:
  void reg_before_read(uint16_t reg) override {
    sync_inputs();
    for (unsigned i = 0; i < registers_.size(); ++i)
      if (registers_[i] == reg) { set_value(reg, core_.csr_read(i)); break; }
  }
  void reg_write(uint16_t reg, uint32_t, uint32_t data, uint32_t mask) override {
    sync_inputs();
    for (unsigned i = 0; i < registers_.size(); ++i)
      if (registers_[i] == reg) { core_.csr_write(i, data, mask); break; }
    mirror();
  }
  bool win_read(uint16_t, uint32_t off, uint8_t *data, unsigned len) override {
    sync_inputs();
    if (!core_.window_read(off, len, data)) fail_access();
    return true;
  }
 private:
  void power_up() { initialized_ = true; core_.power_up(); mirror(); }
  void sync_inputs() {
    if (!initialized_) power_up();
    if (sys_.lc_escalate_en && sys_.lc_escalate_en()) core_.lc_escalate();
    mirror();
  }
  void mirror() {
    for (unsigned i = 0; i < registers_.size(); ++i)
      set_value(registers_[i], core_.csr_stored(i));
  }
  void intr(unsigned bit) override { set_intr(bit); }
  void fatal_alert(const char *name) override { Eg2IpModel::fatal_fault(name); }
  void note(const std::string &message) override { sys_.note(message); }
  void unmodelled(const std::string &message) override { Eg2ModelStop::request(message); }
  bool lc(Eg2OtpLc signal) override;  // integration with Eg2LcCtrl in eg2_otp_ctrl.cc
  static int part_of(uint32_t off) { return Eg2OtpCtrlCore::part_of(off); }
  static uint64_t scramble(uint64_t value, int p) { return Eg2OtpCtrlCore::scramble(value, p); }
  Eg2OtpCtrlCore core_;
  std::vector<uint16_t> registers_;
  bool initialized_ = false;
};
}  // namespace eg2_model
#endif  // EG2_MODEL_IP_EG2_OTP_CTRL_H_
