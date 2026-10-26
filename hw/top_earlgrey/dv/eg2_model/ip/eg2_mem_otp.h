// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// otp_ctrl and lc_ctrl of the EG2 SystemC model (Phase B group: memories). Included by
// ip/eg2_mem.h, which registers them.
//
// The OTP contents are otp_ctrl.core's SW_CFG_WINDOW memory: eg2sim --otp loads the image there
// (one byte address per OTP byte, the whole 2064-byte array of otp_ctrl_mmap.md), DAI writes and
// life-cycle programming change it, and lc_ctrl reads its partition from it. Read from it raw: the
// image holds the SECRET* partitions scrambled (PRESENT, netlist constants), which the model does
// not undo, so a DAI read of an unlocked secret partition returns the scrambled words where the chip
// returns them descrambled. Not modelled: ECC, the background integrity and consistency checks
// (CHECK_TRIGGER, the periods), zeroization (no partition of this top is zeroizable), the key
// derivation interfaces' scrambling.

#ifndef EG2_MODEL_IP_EG2_MEM_OTP_H_
#define EG2_MODEL_IP_EG2_MEM_OTP_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"

namespace eg2_model {

// ---- The OTP partitions (otp_ctrl doc: otp_ctrl_mmap.md, otp_ctrl_partitions.md) ----
struct Eg2OtpPart {
  const char *name;
  uint32_t off, size;  // byte offset and size of the whole partition, its digest included
  bool secret;         // 64-bit granule, read-locked by its digest
  bool hw_digest;      // the digest is computed by the DAI DIGEST command, never written
  bool has_digest;     // LIFE_CYCLE has none
  const char *read_lock;  // the READ_LOCK CSR, or nullptr
};

inline constexpr Eg2OtpPart kEg2OtpParts[] = {
    {"VENDOR_TEST", 0x000, 64, false, false, true, "VENDOR_TEST_READ_LOCK"},
    {"CREATOR_SW_CFG", 0x040, 416, false, false, true, "CREATOR_SW_CFG_READ_LOCK"},
    {"OWNER_SW_CFG", 0x1E0, 680, false, false, true, "OWNER_SW_CFG_READ_LOCK"},
    {"ROT_CREATOR_AUTH_CODESIGN", 0x488, 472, false, false, true,
     "ROT_CREATOR_AUTH_CODESIGN_READ_LOCK"},
    {"ROT_CREATOR_AUTH_STATE", 0x660, 40, false, false, true, "ROT_CREATOR_AUTH_STATE_READ_LOCK"},
    {"HW_CFG0", 0x688, 72, false, true, true, nullptr},
    {"HW_CFG1", 0x6D0, 16, false, true, true, nullptr},
    {"SECRET0", 0x6E0, 40, true, true, true, nullptr},
    {"SECRET1", 0x708, 88, true, true, true, nullptr},
    {"SECRET2", 0x760, 88, true, true, true, nullptr},
    {"LIFE_CYCLE", 0x7B8, 88, false, false, false, nullptr},
};
inline constexpr int kEg2OtpNumParts = 11;
inline constexpr int kEg2OtpSecret0 = 7, kEg2OtpSecret1 = 8, kEg2OtpSecret2 = 9;
inline constexpr int kEg2OtpLifeCycle = 10;
inline constexpr uint32_t kEg2OtpSize = 0x810;
// Items other models read (otp_ctrl_mmap.md).
inline constexpr uint32_t kEg2OtpDeviceId = 0x688, kEg2OtpManufState = 0x6A8;
inline constexpr uint32_t kEg2OtpEnSramIfetch = 0x6D0;
inline constexpr uint32_t kEg2OtpLcTransitionCnt = 0x7B8, kEg2OtpLcState = 0x7E8;

// ---- Life-cycle encodings ----
// The 16-bit OTP words of the life-cycle state (20 words) and transition count (24 words). These
// are netlist constants: the values are copied from the generated hw/ip/lc_ctrl/rtl/
// lc_ctrl_state_pkg.sv (A0..A19, B0..B19, C0..C23, D0..D23), the state and count vectors from its
// lc_state_e / lc_cnt_e (word 0 first). Copy them again if the package is regenerated.
inline constexpr uint16_t kLcA[20] = {0xa352, 0x7181, 0x39ca, 0x406b, 0x7070, 0x6a88, 0x960e,
                                      0x79e0, 0x5cd4, 0x9489, 0xc6e5, 0x97e4, 0xb403, 0x5e90,
                                      0x8c7c, 0xaab0, 0x24cd, 0x5618, 0xdb21, 0xc949};
inline constexpr uint16_t kLcB[20] = {0xb35a, 0xfbd1, 0x3ddb, 0x40ff, 0xfcf2, 0xfb9e, 0xf6ff,
                                      0x7df5, 0xfdd4, 0xb5a9, 0xc7fd, 0xbfed, 0xb763, 0xfe9d,
                                      0xeefc, 0xaffc, 0x75ef, 0x5ebd, 0xdf3f, 0xf97d};
inline constexpr uint16_t kLcC[24] = {0x3399, 0x2e0d, 0x10f5, 0x386a, 0xf4b0, 0x463d,
                                      0x6612, 0x2945, 0x3230, 0x418c, 0x7bb0, 0x11d8,
                                      0x3c40, 0xe92a, 0x14ec, 0x5622, 0xa506, 0x131b,
                                      0x6883, 0x3938, 0x28d2, 0x07c8, 0x4c44, 0x13e9};
inline constexpr uint16_t kLcD[24] = {0x73fb, 0xaf6f, 0xb1fd, 0x387f, 0xfef7, 0xe6bd,
                                      0x7732, 0xe97f, 0xfaf4, 0x59fc, 0x7ff8, 0xd5db,
                                      0xfd68, 0xef7b, 0xb6fe, 0x567b, 0xafc6, 0x9bbf,
                                      0x6bdf, 0x3f3e, 0x3ed7, 0x57cd, 0x6ccc, 0x5fff};
// Decoded states (dec_lc_state_e, the values LC_STATE and TRANSITION_TARGET use).
enum Eg2LcState : uint32_t {
  kLcRaw = 0,
  kLcTestUnlocked0 = 1,  // TEST_UNLOCKEDn = 1 + 2n, TEST_LOCKEDn = 2 + 2n
  kLcTestUnlocked7 = 15,
  kLcDev = 16,
  kLcProd = 17,
  kLcProdEnd = 18,
  kLcRma = 19,
  kLcScrap = 20,
  kLcPostTransition = 21,
  kLcEscalate = 22,
  kLcInvalid = 23,
};
// Each state's 20 words, word 0 first: 0 = zero, A = An, B = Bn.
inline constexpr const char *kLcStateWords[21] = {
    "00000000000000000000", "BAAAAAAAAAAAAAAAAAAA", "BBAAAAAAAAAAAAAAAAAA",
    "BBBAAAAAAAAAAAAAAAAA", "BBBBAAAAAAAAAAAAAAAA", "BBBBBAAAAAAAAAAAAAAA",
    "BBBBBBAAAAAAAAAAAAAA", "BBBBBBBAAAAAAAAAAAAA", "BBBBBBBBAAAAAAAAAAAA",
    "BBBBBBBBBAAAAAAAAAAA", "BBBBBBBBBBAAAAAAAAAA", "BBBBBBBBBBBAAAAAAAAA",
    "BBBBBBBBBBBBAAAAAAAA", "BBBBBBBBBBBBBAAAAAAA", "BBBBBBBBBBBBBBAAAAAA",
    "BBBBBBBBBBBBBBBAAAAA", "BBBBBBBBBBBBBBBBAAAA", "BBBBBBBBBBBBBBBABAAA",
    "BBBBBBBBBBBBBBBAABAA", "BBBBBBBBBBBBBBBBBABB", "BBBBBBBBBBBBBBBBBBBB"};

inline bool eg2_lc_test_unlocked(uint32_t s) { return s >= 1 && s <= 15 && (s & 1); }
inline bool eg2_lc_test_locked(uint32_t s) { return s >= 2 && s <= 14 && !(s & 1); }

// ---- otp_ctrl (hw/top_earlgrey/ip_autogen/otp_ctrl: programmer's guide, theory of operation) ----
//
// DAI: DIRECT_ACCESS_CMD RD (1), WR (2), DIGEST (4), ZEROIZE (8) complete at once; STATUS.DAI_IDLE
// stays 1. The granule is 64 bits in the secret partitions and for every digest, 32 bits otherwise
// (the address's low bits are ignored). AccessError (5): an address in no partition or in
// LIFE_CYCLE (only lc_ctrl reaches it); a read of a partition read-locked by its READ_LOCK CSR
// (data and digest) or of a secret partition locked by its digest (its digest stays readable); a
// write to a partition locked by its digest, or to the digest of a hardware-digest partition; a
// write to SECRET2 outside DEV, PROD, PROD_END and RMA ("RMA_TOKEN and CREATOR_ROOT_KEY_SHARE* can
// only be programmed when the device is in the DEV, PROD, PROD_END and RMA stages"); a DIGEST on a
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

  // A partition is locked by its digest as the OTP held it at the last reset.
  bool partition_locked(int p) const { return locked_[p]; }
  // The SRAM scrambling key seeds are valid once SECRET1 is locked.
  bool sram_key_seed_valid() const { return locked_[kEg2OtpSecret1]; }
  bool en_sram_ifetch() { return byte(kEg2OtpEnSramIfetch) == 0x96; }  // MuBi8True
  // keymgr_dpe's creator root key (top: otp_ctrl.keymgr_creator_root_key): SECRET2's
  // CREATOR_ROOT_KEY_SHARE0 ^ SHARE1 (otp_ctrl_mmap.md), valid once SECRET2 is locked by its digest
  // (a secret partition's key outputs are only valid once it is locked). The shares are taken as
  // the image holds them: the model does not descramble SECRET2.
  bool creator_root_key(std::vector<uint8_t> *key) {
    key->assign(32, 0);
    for (unsigned i = 0; i < 32; ++i) (*key)[i] = byte(0x770 + i) ^ byte(0x790 + i);
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
      uint64_t dg = pt.has_digest ? dword(pt.off + pt.size - 8) : 0;
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
    return kEg2OtpParts[p].has_digest && a >= kEg2OtpParts[p].off + kEg2OtpParts[p].size - 8;
  }
  bool wide(int p, uint32_t a) const { return kEg2OtpParts[p].secret || is_digest(p, a); }
  bool read_locked_csr(int p) const {
    return read_lock_reg_[p] >= 0 && !(value(uint16_t(read_lock_reg_[p])) & 1);
  }

  uint32_t dai_read() {
    uint32_t a = value(addr_) & 0xfff;
    int p = part_of(a);
    if (p < 0 || p == kEg2OtpLifeCycle) return kAccessError;
    bool dg = is_digest(p, a);
    if (read_locked_csr(p)) return kAccessError;
    if (kEg2OtpParts[p].secret && locked_[p] && !dg) return kAccessError;
    if (wide(p, a)) {
      a &= ~7u;
      set_value(rdata0_, word(a));
      set_value(rdata1_, word(a + 4));
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
    uint32_t da = pt.off + pt.size - 8;
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
// RMA_UNLOCK: SECRET2; TOKEN_ERROR otherwise); the token value itself is not checked: that needs
// the cSHAKE128 hash and the descrambled token from OTP. A legal one is programmed into OTP
// (TRANSITION_SUCCESSFUL). Either way the controller is then in POST_TRANSITION, the state and
// count read 21 and 31, and the CPU is disabled until the next reset: the run stops there, saying
// so, as the chip's core stops. The volatile RAW unlock is not available (SecVolatileRawUnlockEn = 0
// in top_pkg): TRANSITION_CTRL.VOLATILE_RAW_UNLOCK reads back 0. The life-cycle broadcast signals
// are accessors (lc_ctrl_function_signals_table.md, lc_ctrl_access_signals_table.md). The OTP
// vendor test interface is storage only.
//
// Escalation (theory_of_operation.md "Security Escalation"): the alert handler's severities 1 and 2
// (the top's esc_scrap_state0 and esc_scrap_state1 receivers) each move the state to ESCALATE, a
// virtual SCRAP not written to OTP, until the next reset: LC_STATE reads ESCALATE, lc_escalate_en
// is on and lc_cpu_en off, which disables the core's fetch (rv_core_ibex "Fetch Enable"). The same
// document's "Escalation Handling" section calls the first channel esc_wipe_secrets, which would
// only assert lc_escalate_en; the model follows "Security Escalation" and the top's names
// (status/rtl_tickets.md).
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
              "): life cycle state ESCALATE until reset, lc_escalate_en on, lc_cpu_en off");
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
      set_value(ctrl_, value(ctrl_) & ~2u);  // no volatile RAW unlock in this top
    } else if (r == cmd_ && (wdata & wmask & 1) && (value(trans_regwen_) & 1)) {
      transition();
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

  void transition() {
    Eg2OtpCtrl *o = otp();
    uint32_t st = value(status_) & 0x3;
    uint32_t tgt_raw = value(target_);
    uint32_t to = tgt_raw & 0x1f;
    if (!o || state_dec_ == kLcInvalid || state_dec_ > kLcScrap) {
      st |= 1u << 5;  // TRANSITION_ERROR
    } else if (count_ >= 24) {
      st |= 1u << 4;  // TRANSITION_COUNT_ERROR
    } else {
      // The count first.
      ++count_;
      for (int j = 0; j < 24; ++j) {
        o->set_half(kEg2OtpLcTransitionCnt + 2 * j, j < int(count_) ? kLcD[j] : kLcC[j]);
      }
      int tok = (rep5(to) == tgt_raw && to <= kLcScrap) ? rule(state_dec_, to) : int(kIllegal);
      bool otp_ok = true;  // A -> B only, word by word
      for (int i = 0; tok != kIllegal && i < 20; ++i) {
        char f = kLcStateWords[state_dec_][i], t = kLcStateWords[to][i];
        otp_ok = otp_ok && (f == t || f == '0' || (f == 'A' && t == 'B'));
      }
      if (tok == kIllegal || !otp_ok) {
        st |= 1u << 5;
      } else if ((tok == kTestUnlock || tok == kTestExit) &&
                 !o->partition_locked(kEg2OtpSecret0)) {
        st |= 1u << 6;  // TOKEN_ERROR
      } else if (tok == kRmaUnlock && !o->partition_locked(kEg2OtpSecret2)) {
        st |= 1u << 6;
      } else {
        for (int i = 0; i < 20; ++i) {
          char t = kLcStateWords[to][i];
          o->set_half(kEg2OtpLcState + 2 * i, t == '0' ? 0 : t == 'A' ? kLcA[i] : kLcB[i]);
        }
        st |= 1u << 3;  // TRANSITION_SUCCESSFUL
      }
    }
    if (value(ctrl_) & 1) st |= 1u << 2;  // EXT_CLOCK_SWITCHED
    post_transition_ = true;
    set_value(status_, st);
    set_value(trans_regwen_, 0);
    publish();
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "lc_ctrl: transition to state %u %s; the core is disabled in POST_TRANSITION "
                  "until the next reset",
                  to, (st & (1u << 3)) ? "successful" : "failed");
    Eg2ModelStop::request(buf);
  }

  uint16_t status_, claim_regwen_, claim_, trans_regwen_, cmd_, ctrl_, target_, state_, cnt_, id_,
      rev0_, rev1_, dev_id_, manuf_;
  uint32_t state_dec_ = kLcInvalid, count_ = 31;
  bool post_transition_ = false, escalated_ = false;
};

inline bool Eg2OtpCtrl::creator_seed_writable() {
  auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
  return !lc || lc->creator_seed_sw_rw_en();
}

inline bool Eg2OtpCtrl::rma() {
  Eg2LcCtrl *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
  return lc && lc->state() == kLcRma;
}

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_MEM_OTP_H_
