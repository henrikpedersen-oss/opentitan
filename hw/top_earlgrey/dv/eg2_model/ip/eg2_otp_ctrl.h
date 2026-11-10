// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_OTP_CTRL_H_
#define EG2_MODEL_IP_EG2_OTP_CTRL_H_

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../eg2_ip_model.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"
#include "eg2_otp_data.h"

namespace eg2_model {

// ---- otp_ctrl (hw/top_earlgrey/ip_autogen/otp_ctrl: programmer's guide, theory of operation) ----
//
// DAI: DIRECT_ACCESS_CMD RD (1), WR (2), DIGEST (4), ZEROIZE (8) complete at once; STATUS.DAI_IDLE
// stays 1. The granule is 64 bits in the secret partitions and for every digest, 32 bits otherwise
// (the address's low bits are ignored). AccessError (5): an address in no partition or in
// LIFE_CYCLE (only lc_ctrl reaches it); a read of a partition read-locked by its READ_LOCK CSR
// (data and digest) or of a secret partition locked by its digest (its digest stays readable); a
// write to a partition locked by its digest, or to the digest of a hardware-digest partition; a
// read or write of SECRET2 data while lc_creator_seed_sw_rw_en is off (interfaces.md: it "enables
// SW read / write access to the RMA_TOKEN and CREATOR_ROOT_KEY_SHARE*"; lc_ctrl's OTP accessibility
// table requires it to be on); a DIGEST on a
// partition without a hardware digest or already locked; ZEROIZE (no zeroizable partition).
// MacroWriteBlankError (4): a write that would clear a programmed bit; the model keeps the old bits
// and sets the new ones. Each command sets otp_operation_done; an error sets otp_error,
// STATUS.DAI_ERROR and ERR_CODE_11 (the DAI's error code). The digest the DIGEST command writes is
// a model value (FNV-1a over the partition, never zero), not the chip's PRESENT-based digest.
// Locks: a partition with a non-zero digest is write-locked, and a secret one read-locked, from the
// next reset on ("the effect of locking a partition via the digest only takes effect after the
// next system reset"); at reset the *_DIGEST CSRs take the digests from OTP. SW_CFG_WINDOW reads
// return the OTP bytes, except in a partition read-locked by its READ_LOCK CSR (outside RMA:
// interfaces.md, lc_rma_state "enable[s] SW read access to (read-locked) partitions in the RMA
// state") or in a secret partition locked by its digest: those answer a bus error. The doc says the
// partition is then not readable but not how a refused read answers; the error is from the RTL
// (otp_ctrl_part_unbuf.sv ReadSt: "if the partition is locked, signal back a bus error").
class Eg2OtpCtrl : public Eg2IpModel {
 public:
  enum Err : uint32_t { kNoError = 0, kWriteBlankError = 4, kAccessError = 5 };

  Eg2OtpCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    status_ = reg("STATUS");
    regwen_ = reg("DIRECT_ACCESS_REGWEN");
    cmd_ = reg("DIRECT_ACCESS_CMD");
    addr_ = reg("DIRECT_ACCESS_ADDRESS");
    wdata0_ = reg("DIRECT_ACCESS_WDATA_0");
    wdata1_ = reg("DIRECT_ACCESS_WDATA_1");
    rdata0_ = reg("DIRECT_ACCESS_RDATA_0");
    rdata1_ = reg("DIRECT_ACCESS_RDATA_1");
    err_dai_ = reg("ERR_CODE_11");
    set_modelled(status_);
    set_modelled(rdata0_);
    set_modelled(rdata1_);
    set_modelled(err_dai_);
    for (int p = 0; p < kEg2OtpNumParts; ++p) {
      const Eg2OtpPart &pt = kEg2OtpParts[p];
      digest_reg_[p] = pt.has_digest ? reg_index(std::string(pt.name) + "_DIGEST_0") : -1;
      read_lock_reg_[p] = pt.read_lock ? reg_index(pt.read_lock) : -1;
      if (digest_reg_[p] >= 0) {
        set_modelled(uint16_t(digest_reg_[p]));
        set_modelled(uint16_t(digest_reg_[p] + 1));
      }
    }
    // The chip bench writes the OTP array between resets. The values take effect when the reset
    // makes otp_ctrl and lc_ctrl sample the array again (otp_ctrl theory_of_operation.md,
    // "Initialization"). otp.write is <byte offset>:<little-endian value>:<byte count>.
    sys_.stim_handlers["otp.write"] = [this](const std::string &v) { return backdoor_write(v); };
    sys_.stim_handlers["otp.lc_state"] = [this](const std::string &v) {
      return backdoor_lc_state(v);
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
    set_dword(eg2_otp_digest_off(pt), 1);
  }

  std::vector<uint8_t> lc_token(uint32_t off) {
    int p = part_of(off);
    std::vector<uint8_t> out(16, 0);
    if (p != kEg2OtpSecret0 && p != kEg2OtpSecret2) return out;
    for (unsigned w = 0; w < 2; ++w) {
      uint64_t v = descramble(dword(off + 8 * w), p);
      for (unsigned i = 0; i < 8; ++i) out[8 * w + i] = uint8_t(v >> (8 * i));
    }
    return out;
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

  // A partition is locked by its digest as the OTP held it at the last reset.
  bool partition_locked(int p) const { return locked_[p]; }
  // The SRAM scrambling key seeds are valid once SECRET1 is locked.
  bool sram_key_seed_valid() const { return locked_[kEg2OtpSecret1]; }
  bool en_sram_ifetch() { return byte(kEg2OtpEnSramIfetch) == 0x96; }  // MuBi8True
  // keymgr_dpe's creator root key (top: otp_ctrl.keymgr_creator_root_key): SECRET2's
  // CREATOR_ROOT_KEY_SHARE0 ^ SHARE1 (otp_ctrl_mmap.md), valid once SECRET2 is locked by its digest
  // (a secret partition's key outputs are only valid once it is locked). otp_ctrl descrambles the
  // shares before they leave for keymgr (theory_of_operation.md, "descrambles it on the fly"), as the
  // DAI does on a read: each 64-bit word of a share is descrambled with SECRET2's key.
  bool creator_root_key(std::vector<uint8_t> *key) {
    key->assign(32, 0);
    for (unsigned w = 0; w < 4; ++w) {
      uint64_t s0 = descramble(dword(kEg2OtpCreatorRootKeyShare0 + 8 * w), kEg2OtpSecret2);
      uint64_t s1 = descramble(dword(kEg2OtpCreatorRootKeyShare1 + 8 * w), kEg2OtpSecret2);
      for (unsigned b = 0; b < 8; ++b) (*key)[8 * w + b] = uint8_t((s0 ^ s1) >> (8 * b));
    }
    return locked_[kEg2OtpSecret2];
  }
  // keymgr_dpe's device_identifier (top: otp_ctrl's device id, HW_CFG0 DEVICE_ID, 256 bits).
  std::vector<uint8_t> device_id() {
    std::vector<uint8_t> d(32);
    for (unsigned i = 0; i < 32; ++i) d[i] = byte(kEg2OtpDeviceId + i);
    return d;
  }

  void start_of_simulation() override { power_up(); }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    power_up();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r != cmd_ || !(value(regwen_) & 1)) return;
    uint32_t c = wdata & wmask & 0xf;
    if (!c) return;
    uint32_t err = c == 1 ? dai_read() : c == 2 ? dai_write() : c == 4 ? dai_digest()
                                                                       : uint32_t(kAccessError);
    set_value(err_dai_, err);
    set_value(status_, (value(status_) & ~(1u << 1)) | (err ? (1u << 1) : 0));
    set_intr("otp_operation_done");
    if (err) set_intr("otp_error");
  }

 private:
  void power_up() {
    if (otp().size() < kEg2OtpSize) otp().resize(kEg2OtpSize, 0);
    for (int p = 0; p < kEg2OtpNumParts; ++p) {
      const Eg2OtpPart &pt = kEg2OtpParts[p];
      uint64_t dg = pt.has_digest ? dword(eg2_otp_digest_off(pt)) : 0;
      locked_[p] = dg != 0;
      if (digest_reg_[p] >= 0) {
        set_value(uint16_t(digest_reg_[p]), uint32_t(dg));
        set_value(uint16_t(digest_reg_[p] + 1), uint32_t(dg >> 32));
      }
    }
    set_value(status_, 1u << 8);  // DAI_IDLE
  }

  int part_of(uint32_t a) const {
    for (int p = 0; p < kEg2OtpNumParts; ++p) {
      if (a >= kEg2OtpParts[p].off && a < kEg2OtpParts[p].off + kEg2OtpParts[p].size) return p;
    }
    return -1;
  }
  bool is_digest(int p, uint32_t a) const {
    return kEg2OtpParts[p].has_digest && a >= eg2_otp_digest_off(kEg2OtpParts[p]);
  }
  bool wide(int p, uint32_t a) const { return kEg2OtpParts[p].secret || is_digest(p, a); }
  bool read_locked_csr(int p) const {
    return read_lock_reg_[p] >= 0 && !(value(uint16_t(read_lock_reg_[p])) & 1);
  }

  struct PresentKey {
    uint64_t hi, lo;
  };
  static PresentKey present_next_key(PresentKey k, unsigned round) {
    static constexpr uint8_t kSbox[16] = {0xc, 0x5, 0x6, 0xb, 0x9, 0x0, 0xa, 0xd,
                                          0x3, 0xe, 0xf, 0x8, 0x4, 0x7, 0x1, 0x2};
    uint64_t hi = ((k.hi & 7) << 61) | (k.lo >> 3);
    uint64_t lo = ((k.lo & 7) << 61) | (k.hi >> 3);
    hi = (hi & 0x00ffffffffffffffull) | (uint64_t(kSbox[hi >> 60]) << 60) |
         (uint64_t(kSbox[(hi >> 56) & 0xf]) << 56);
    hi ^= uint64_t(round) >> 2;
    lo ^= uint64_t(round) << 62;
    return {hi, lo};
  }
  static uint64_t present_inv_sbox(uint64_t data) {
    static constexpr uint8_t kInv[16] = {0x5, 0xe, 0xf, 0x8, 0xc, 0x1, 0x2, 0xd,
                                         0xb, 0x4, 0x6, 0x3, 0x0, 0x7, 0x9, 0xa};
    uint64_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= uint64_t(kInv[(data >> (4 * i)) & 0xf]) << (4 * i);
    return out;
  }
  static uint64_t present_inv_perm(uint64_t data) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; ++i) {
      unsigned to = i == 63 ? 63 : (4 * i) % 63;
      out |= ((data >> i) & 1) << to;
    }
    return out;
  }
  static uint64_t present_sbox(uint64_t data) {
    static constexpr uint8_t kSbox[16] = {0xc, 0x5, 0x6, 0xb, 0x9, 0x0, 0xa, 0xd,
                                          0x3, 0xe, 0xf, 0x8, 0x4, 0x7, 0x1, 0x2};
    uint64_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= uint64_t(kSbox[(data >> (4 * i)) & 0xf]) << (4 * i);
    return out;
  }
  static uint64_t present_perm(uint64_t data) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; ++i) {
      unsigned to = i == 63 ? 63 : (16 * i) % 63;
      out |= ((data >> i) & 1) << to;
    }
    return out;
  }
  // otp_ctrl theory of operation "Scrambling" uses standard 31-round PRESENT-128 with a distinct
  // netlist key for each secret partition. These are the testing keys published in
  // top_earlgrey.secrets.testing.gen.hjson, not values inferred from RTL. The DAI scrambles a
  // secret partition's 64-bit words on write and descrambles them on read.
  static void present_round_keys(int p, PresentKey keys[32]) {
    static constexpr PresentKey kKeys[] = {
        {0xf9089cf2787c6c14ull, 0xb7d1a6c0618e831full},
        {0xc8129e7a4149ba28ull, 0x31fe2e9fb3b2a1d2ull},
        {0x5254f861ba80f82bull, 0x303da30635aa3d3aull},
    };
    keys[0] = kKeys[p - kEg2OtpSecret0];
    for (unsigned i = 1; i < 32; ++i) keys[i] = present_next_key(keys[i - 1], i);
  }
  static uint64_t scramble(uint64_t data, int p) {
    PresentKey keys[32];
    present_round_keys(p, keys);
    for (unsigned i = 0; i < 31; ++i) data = present_perm(present_sbox(data ^ keys[i].hi));
    return data ^ keys[31].hi;
  }
  static uint64_t descramble(uint64_t data, int p) {
    PresentKey keys[32];
    present_round_keys(p, keys);
    data ^= keys[31].hi;
    for (int i = 30; i >= 0; --i) {
      data = present_inv_perm(data);
      data = present_inv_sbox(data);
      data ^= keys[i].hi;
    }
    return data;
  }

  uint32_t dai_read() {
    // DIRECT_ACCESS_RDATA reports the data read by this command. A refused command reads no data,
    // so both words are set to their reset value (0) rather than keep an earlier read's data. A model
    // choice: no document says what DIRECT_ACCESS_RDATA holds after a refused read.
    set_value(rdata0_, 0);
    set_value(rdata1_, 0);
    uint32_t a = value(addr_) & 0xfff;
    int p = part_of(a);
    if (p < 0 || p == kEg2OtpLifeCycle) return kAccessError;
    bool dg = is_digest(p, a);
    if (read_locked_csr(p)) return kAccessError;
    if (p == kEg2OtpSecret2 && !dg && !creator_seed_writable()) return kAccessError;
    if (kEg2OtpParts[p].secret && locked_[p] && !dg) return kAccessError;
    if (wide(p, a)) {
      a &= ~7u;
      uint64_t data = dword(a);
      if (kEg2OtpParts[p].secret && !dg) data = descramble(data, p);
      set_value(rdata0_, uint32_t(data));
      set_value(rdata1_, uint32_t(data >> 32));
    } else {
      a &= ~3u;
      set_value(rdata0_, word(a));
      set_value(rdata1_, 0);
    }
    return kNoError;
  }

  uint32_t dai_write() {
    uint32_t a = value(addr_) & 0xfff;
    int p = part_of(a);
    if (p < 0 || p == kEg2OtpLifeCycle || locked_[p]) return kAccessError;
    if (kEg2OtpParts[p].hw_digest && is_digest(p, a)) return kAccessError;
    if (p == kEg2OtpSecret2 && !creator_seed_writable()) return kAccessError;
    unsigned n = wide(p, a) ? 8 : 4;
    a &= ~(n - 1);
    uint64_t w = uint64_t(value(wdata0_)) | (n == 8 ? uint64_t(value(wdata1_)) << 32 : 0);
    if (kEg2OtpParts[p].secret && !is_digest(p, a)) w = scramble(w, p);
    uint32_t err = kNoError;
    for (unsigned i = 0; i < n; ++i) {
      uint8_t nb = uint8_t(w >> (8 * i)), &ob = otp()[a + i];
      if (ob & ~nb) err = kWriteBlankError;
      ob = uint8_t(ob | nb);
    }
    return err;
  }

  uint32_t dai_digest() {
    uint32_t a = value(addr_) & 0xfff;
    int p = part_of(a);
    if (p < 0 || !kEg2OtpParts[p].hw_digest || locked_[p]) return kAccessError;
    const Eg2OtpPart &pt = kEg2OtpParts[p];
    uint32_t da = eg2_otp_digest_off(pt);
    if (dword(da)) return kAccessError;  // already computed; locks at the next reset
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = pt.off; i < da; ++i) h = (h ^ otp()[i]) * 0x100000001b3ull;
    if (!h) h = 1;
    for (unsigned i = 0; i < 8; ++i) otp()[da + i] = uint8_t(h >> (8 * i));
    return kNoError;
  }

  bool creator_seed_writable();  // below, after Eg2LcCtrl
  bool rma();                    // below, after Eg2LcCtrl

  bool win_read(uint16_t, uint32_t off, uint8_t *data, unsigned len) override {
    int p = part_of(off);
    if (p >= 0 && ((read_locked_csr(p) && !rma()) ||
                   (kEg2OtpParts[p].secret && locked_[p] && !is_digest(p, off)))) {
      std::memset(data, 0, len);
      fail_access();
      return true;
    }
    return false;
  }

  uint16_t status_, regwen_, cmd_, addr_, wdata0_, wdata1_, rdata0_, rdata1_, err_dai_;
  int digest_reg_[kEg2OtpNumParts], read_lock_reg_[kEg2OtpNumParts];
  bool locked_[kEg2OtpNumParts] = {};
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTP_CTRL_H_
