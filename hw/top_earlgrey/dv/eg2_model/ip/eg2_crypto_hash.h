// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// hmac and kmac of the EG2 SystemC model (Phase B, crypto group), from hw/ip/{hmac,kmac}/doc and
// their hjson.
//
// hmac   SHA-2 256/384/512 and HMAC with them, on Google CryptoC's SHA-2 (hw/ip/hmac/dv/cryptoc_dpi,
//        the block's DV reference, Apache 2.0). MSG_FIFO takes bytes in little-endian order of
//        each write, or swapped with CFG.endian_swap (theory of operation "SHA-2 message feed and
//        pad"); the FIFO drains at once, so it is never full and fifo_empty never fires (it fires
//        only after the FIFO was full). DIGEST_0..15 hold the digest as one big-endian number,
//        DIGEST_0 most significant (dif_hmac.c), each word byte-swapped with CFG.digest_swap; KEY_*
//        are big-endian unless CFG.key_swap. Save and restore (hash_stop / hash_continue): the
//        context is DIGEST_* and MSG_LENGTH_* at a block boundary, as the programmer's guide
//        describes; in HMAC mode the model counts MSG_LENGTH over the message only and keeps the
//        inner hash's key block internally. Sideloaded keys: set_sideload_key().
// kmac   SHA3, SHAKE, cSHAKE and KMAC as a Keccak-f[1600] sponge (the permutation of digestpp,
//        hw/ip/kmac/dv/dpi/vendor, public domain, the DPI reference of the block's DV). START
//        absorbs bytepad(PREFIX, rate) for cSHAKE and bytepad(encode_string(key), rate) in KMAC
//        mode (theory of operation; software writes encode_string(N) || encode_string(S) to PREFIX
//        and appends right_encode(L) itself); PROCESS pads and permutes; the rate part of the state
//        is readable in the STATE window while squeezing (share 1 reads zero: masking is not
//        modelled); RUN permutes again; DONE clears. STOP / STATE_WRITE / CONTINUE switch
//        contexts as the programmer's guide describes (the full state is exposed and restored).
//        Entropy (EDN, ENTROPY_SEED) only feeds
//        masking and is not used. The application interface for keymgr and rom_ctrl is
//        Eg2Kmac::app_kmac / app_cshake.

#ifndef EG2_MODEL_IP_EG2_CRYPTO_HASH_H_
#define EG2_MODEL_IP_EG2_CRYPTO_HASH_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha256.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha384.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha512.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"

namespace eg2_model {

inline uint32_t bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

// ---------------------------------------------------------------------------------------------
class Eg2Hmac : public Eg2IpModel {
 public:
  Eg2Hmac(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    cfg_ = reg("CFG");
    cmd_ = reg("CMD");
    status_ = reg("STATUS");
    err_code_ = reg("ERR_CODE");
    intr_ = reg("INTR_STATE");
    len_lo_ = reg("MSG_LENGTH_LOWER");
    len_hi_ = reg("MSG_LENGTH_UPPER");
    for (unsigned i = 0; i < 32; ++i) key_[i] = reg(("KEY_" + std::to_string(i)).c_str());
    wipe_ = reg("WIPE_SECRET");
    for (unsigned i = 0; i < 16; ++i) {
      digest_[i] = reg(("DIGEST_" + std::to_string(i)).c_str());
      set_modelled(digest_[i]);
    }
    fifo_ = uint16_t(win_index("MSG_FIFO"));
    for (uint16_t r : {status_, err_code_, len_lo_, len_hi_}) set_modelled(r);
    update();
  }

  // The sideloaded key: keymgr_dpe's hmac_key is a wide_hw_key_req, 512 bits (16 words). A key
  // that becomes invalid aborts a running sideloaded operation (digest cleared, SwInvalidConfig,
  // no hmac_done) and otherwise clears the digest silently (programmer's guide "Using a
  // sideloaded key").
  void set_sideload_key(const uint32_t *key16, bool valid) {
    bool revoked = side_valid_ && !valid;
    side_valid_ = valid;
    for (unsigned i = 0; i < 16; ++i) side_key_[i] = valid ? key16[i] : 0;
    if (revoked && sideloaded()) {
      for (unsigned i = 0; i < 16; ++i) set_value(digest_[i], 0);
      if (busy_) {
        busy_ = false;
        error(0x6);  // SwInvalidConfig
      }
      update();
    }
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    busy_ = false;
    msg_bytes_ = 0;
    update();
  }

 protected:
  // CFG.key_length while CFG.sideload: "the key length is forced to the width of the sideload
  // interface and reading this register returns that forced value" (hmac.hjson): Key_512.
  void reg_before_read(uint16_t r) override {
    if (r == cfg_ && ((value(cfg_) >> 15) & 1)) {
      cfg_saved_ = value(cfg_);
      set_value(cfg_, (cfg_saved_ & ~(0x3fu << 9)) | (8u << 9));
      forced_ = true;
    }
  }
  void reg_after_read(uint16_t r) override {
    if (r == cfg_ && forced_) {
      set_value(cfg_, cfg_saved_);
      forced_ = false;
    }
  }

  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    if (r == cfg_) {
      if (busy_) {
        set_value(cfg_, old);  // "updated value is discarded" while the engine computes
      } else if (!(value(cfg_) & 2) && (old & 2)) {
        for (unsigned i = 0; i < 16; ++i) set_value(digest_[i], 0);  // sha_en off clears it
      }
    } else if (r == wipe_) {
      // WIPE_SECRET: "the value is used to clear some internal variables such as the secret key,
      // intermediate hash results, digest" (hmac.hjson): the key and the digest take the value.
      uint32_t v = wdata & wmask;
      for (unsigned i = 0; i < 32; ++i) set_value(key_[i], v);
      for (unsigned i = 0; i < 16; ++i) set_value(digest_[i], v);
    } else if (r == cmd_) {
      uint32_t c = wdata & wmask & 0xf;
      // With a sideloaded key "CMD.hash_stop and CMD.hash_continue are rejected, and ERR_CODE
      // reports SwInvalidConfig" (programmer's guide).
      if (sideloaded() && (c & 0xc)) {
        error(0x6);
        c &= ~0xcu;
      }
      if (c & 1) start(false);
      if (c & 8) start(true);
      if (c & 2) finish(false);
      if (c & 4) finish(true);
    } else {
      for (unsigned i = 0; i < 16; ++i) {
        if (r == digest_[i] && ((value(cfg_) & 2) || busy_)) set_value(r, old);  // locked
      }
    }
    update();
  }
  void win_write(uint16_t w, uint32_t off, unsigned len) override {
    if (w != fifo_) return;
    if (!busy_) {
      error(0x5);  // SwPushMsgWhenDisallowed
      return;
    }
    const uint8_t *p = &win_mem(w)[off];
    uint8_t b[4];
    for (unsigned i = 0; i < len; ++i) b[i] = p[i];
    if (value(cfg_) & 4) {  // endian_swap: big-endian per access
      for (unsigned i = 0; i < len / 2; ++i) std::swap(b[i], b[len - 1 - i]);
    }
    HASH_update(&ctx_, b, len);
    msg_bytes_ += len;
    update();
  }

 private:
  unsigned digest_size() const {
    switch ((value(cfg_) >> 5) & 0xf) {
      case 1: return 256;
      case 2: return 384;
      case 4: return 512;
      default: return 0;
    }
  }
  unsigned block_bytes() const { return digest_size() == 256 ? 64 : 128; }
  unsigned key_bytes() const {
    if ((value(cfg_) >> 15) & 1) return 64;  // sideloaded: the interface's width, 512 bits
    switch ((value(cfg_) >> 9) & 0x3f) {
      case 1: return 16;
      case 2: return 32;
      case 4: return 48;
      case 8: return 64;
      case 16: return 128;
      default: return 0;
    }
  }
  bool hmac_mode() const { return value(cfg_) & 1; }
  // CFG.sideload "only takes effect for keyed HMAC operations" (theory of operation).
  bool sideloaded() const { return hmac_mode() && ((value(cfg_) >> 15) & 1); }

  void init_ctx() {
    switch (digest_size()) {
      case 256: SHA256_init(&ctx_); break;
      case 384: SHA384_init(&ctx_); break;
      default: SHA512_init(&ctx_); break;
    }
  }
  // The key block XOR pad (0x36 inner, 0x5c outer).
  std::vector<uint8_t> key_block(uint8_t pad) const {
    std::vector<uint8_t> k(block_bytes(), 0);
    bool side = (value(cfg_) >> 15) & 1;
    for (unsigned i = 0; i < key_bytes() / 4 && 4 * i < k.size(); ++i) {
      uint32_t w = side ? side_key_[i] : value(key_[i]);
      if (!side && (value(cfg_) & 16)) w = bswap32(w);  // key_swap
      if (side) w = bswap32(w);
      k[4 * i] = uint8_t(w >> 24);
      k[4 * i + 1] = uint8_t(w >> 16);
      k[4 * i + 2] = uint8_t(w >> 8);
      k[4 * i + 3] = uint8_t(w);
    }
    for (auto &b : k) b ^= pad;
    return k;
  }

  void start(bool resume) {
    if (!(value(cfg_) & 2)) {
      error(0x2);  // SwHashStartWhenShaDisabled
      return;
    }
    if (digest_size() == 0) {
      error(0x9);  // InvalidConfig
      return;
    }
    if (sideloaded() && !side_valid_) {
      error(0x6);  // SwInvalidConfig: "Starting while the key is invalid is blocked"
      return;
    }
    init_ctx();
    if (!resume) {
      if (hmac_mode()) {
        std::vector<uint8_t> k = key_block(0x36);
        HASH_update(&ctx_, k.data(), k.size());
      }
      msg_bytes_ = 0;
    } else {
      // Restore: the state from DIGEST_*, the length from MSG_LENGTH_* (programmer's guide).
      bool sw = value(cfg_) & 8;
      for (unsigned i = 0; i < 8; ++i) {
        if (digest_size() == 256) {
          uint32_t w = value(digest_[i]);
          ctx_.state[i] = sw ? bswap32(w) : w;
        } else {
          uint32_t hi = value(digest_[2 * i]), lo = value(digest_[2 * i + 1]);
          if (sw) hi = bswap32(hi), lo = bswap32(lo);
          ctx_.state[i] = (uint64_t(hi) << 32) | lo;
        }
      }
      msg_bytes_ = ((uint64_t(value(len_hi_)) << 32) | value(len_lo_)) / 8;
      ctx_.count = msg_bytes_ + (hmac_mode() ? block_bytes() : 0);
    }
    busy_ = true;
    for (unsigned i = 0; i < 16; ++i) set_value(digest_[i], 0);  // reads 0 while in progress
  }

  void finish(bool stop) {
    if (!busy_) return;
    busy_ = false;
    std::vector<uint8_t> dig;
    if (stop) {
      // At a block boundary: the intermediate state is the context.
      for (unsigned i = 0; i < 8; ++i) {
        if (digest_size() == 256) {
          uint32_t w = uint32_t(ctx_.state[i]);
          for (int b = 3; b >= 0; --b) dig.push_back(uint8_t(w >> (8 * b)));
        } else {
          for (int b = 7; b >= 0; --b) dig.push_back(uint8_t(ctx_.state[i] >> (8 * b)));
        }
      }
    } else {
      unsigned n = digest_size() / 8;
      const uint8_t *h = HASH_final(&ctx_);
      dig.assign(h, h + n);
      if (hmac_mode()) {
        init_ctx();
        std::vector<uint8_t> k = key_block(0x5c);
        HASH_update(&ctx_, k.data(), k.size());
        HASH_update(&ctx_, dig.data(), n);
        h = HASH_final(&ctx_);
        dig.assign(h, h + n);
      }
    }
    bool sw = value(cfg_) & 8;
    for (unsigned i = 0; i < 16; ++i) {
      uint32_t w = 0;
      if (4 * i + 3 < dig.size()) {
        w = (uint32_t(dig[4 * i]) << 24) | (uint32_t(dig[4 * i + 1]) << 16) |
            (uint32_t(dig[4 * i + 2]) << 8) | dig[4 * i + 3];
      }
      set_value(digest_[i], sw ? bswap32(w) : w);
    }
    uint64_t bits = msg_bytes_ * 8;
    set_value(len_lo_, uint32_t(bits));
    set_value(len_hi_, uint32_t(bits >> 32));
    set_intr("hmac_done");
  }

  void error(uint32_t code) {
    // "As long as it [INTR_STATE.hmac_err] is pending, no further error is reported in ERR_CODE"
    // (programmer's guide).
    if ((value(intr_) >> 2) & 1) return;
    set_value(err_code_, code);
    set_intr("hmac_err");
  }
  void update() {
    // STATUS: hmac_idle (0), fifo_empty (1), fifo_full (2), fifo_depth (9:4).
    set_value(status_, (busy_ ? 0u : 1u) | 2u);
    if (busy_) {
      uint64_t bits = msg_bytes_ * 8;
      set_value(len_lo_, uint32_t(bits));
      set_value(len_hi_, uint32_t(bits >> 32));
    }
  }

  uint16_t cfg_, cmd_, status_, err_code_, intr_, len_lo_, len_hi_, key_[32], digest_[16], fifo_,
      wipe_;
  HASH_CTX ctx_{};
  bool busy_ = false, side_valid_ = false, forced_ = false;
  uint32_t cfg_saved_ = 0;

 public:
  bool idle() const override { return !busy_; }
  uint32_t side_key_[16] = {};
  uint64_t msg_bytes_ = 0;
};

// ---------------------------------------------------------------------------------------------
// A Keccak-f[1600] sponge (FIPS 202) on digestpp's permutation. A partial block waits in a buffer
// until it is complete, as in the hardware's message FIFO, so the state never holds half a block.
class Eg2KeccakSponge {
 public:
  void reset(unsigned rate_bytes) {
    std::memset(a_, 0, sizeof a_);
    rate_ = rate_bytes;
    buf_.clear();
  }
  void absorb(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      buf_.push_back(p[i]);
      if (buf_.size() == rate_) {
        for (unsigned k = 0; k < rate_; ++k) xor_byte(k, buf_[k]);
        permute();
        buf_.clear();
      }
    }
  }
  // Absorb zeros up to the next block boundary (the end of bytepad).
  void pad_to_block() {
    if (!buf_.empty()) {
      std::vector<uint8_t> z(rate_ - buf_.size(), 0);
      absorb(z.data(), z.size());
    }
  }
  void finish(uint8_t domain) {
    std::vector<uint8_t> blk(rate_, 0);
    std::copy(buf_.begin(), buf_.end(), blk.begin());
    blk[buf_.size()] ^= domain;
    blk[rate_ - 1] ^= 0x80;
    buf_.clear();
    for (unsigned k = 0; k < rate_; ++k) xor_byte(k, blk[k]);
    permute();
  }
  bool block_aligned() const { return buf_.empty(); }
  void drop_partial() { buf_.clear(); }
  void permute() { digestpp::detail::sha3_functions::transform<24>(a_); }
  uint8_t byte(unsigned i) const { return uint8_t(a_[i / 8] >> (8 * (i % 8))); }
  void set_byte(unsigned i, uint8_t b) {
    a_[i / 8] = (a_[i / 8] & ~(uint64_t(0xff) << (8 * (i % 8)))) | (uint64_t(b) << (8 * (i % 8)));
  }
  unsigned rate() const { return rate_; }

 private:
  void xor_byte(unsigned i, uint8_t b) { a_[i / 8] ^= uint64_t(b) << (8 * (i % 8)); }
  uint64_t a_[25] = {};
  unsigned rate_ = 136;
  std::vector<uint8_t> buf_;
};

// left_encode / encode_string (NIST SP 800-185 2.3).
inline std::vector<uint8_t> left_encode(uint64_t x) {
  std::vector<uint8_t> b;
  do {
    b.insert(b.begin(), uint8_t(x));
    x >>= 8;
  } while (x);
  b.insert(b.begin(), uint8_t(b.size()));
  return b;
}
inline std::vector<uint8_t> right_encode(uint64_t x) {
  std::vector<uint8_t> b;
  do {
    b.insert(b.begin(), uint8_t(x));
    x >>= 8;
  } while (x);
  b.push_back(uint8_t(b.size()));
  return b;
}

class Eg2Kmac : public Eg2IpModel {
 public:
  enum Cmd { kStart = 7, kProcess = 50, kRun = 46, kDone = 53, kStop = 28, kStateWrite = 27,
             kContinue = 41 };

  Eg2Kmac(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    cfg_regwen_ = reg("CFG_REGWEN");
    cfg_ = reg("CFG_SHADOWED");
    cmd_ = reg("CMD");
    status_ = reg("STATUS");
    key_len_ = reg("KEY_LEN");
    err_code_ = reg("ERR_CODE");
    for (unsigned i = 0; i < 16; ++i) {
      key0_[i] = reg(("KEY_SHARE0_" + std::to_string(i)).c_str());
      key1_[i] = reg(("KEY_SHARE1_" + std::to_string(i)).c_str());
    }
    for (unsigned i = 0; i < 11; ++i) prefix_[i] = reg(("PREFIX_" + std::to_string(i)).c_str());
    state_win_ = uint16_t(win_index("STATE"));
    fifo_win_ = uint16_t(win_index("MSG_FIFO"));
    for (uint16_t r : {cfg_regwen_, status_, err_code_}) set_modelled(r);
    update();
  }

  // The application interface (keymgr, rom_ctrl): KMAC256 / cSHAKE256 of msg, out_len bytes.
  static std::vector<uint8_t> app_kmac(const std::vector<uint8_t> &key,
                                       const std::vector<uint8_t> &custom,
                                       const std::vector<uint8_t> &msg, size_t out_len) {
    Eg2KeccakSponge s;
    s.reset(136);
    const uint8_t name[] = {'K', 'M', 'A', 'C'};
    absorb_prefix(s, std::vector<uint8_t>(name, name + 4), custom);
    absorb_bytepad(s, encode_string(key));
    s.absorb(msg.data(), msg.size());
    std::vector<uint8_t> l = right_encode(out_len * 8);
    s.absorb(l.data(), l.size());
    s.finish(0x04);
    return squeeze(s, out_len);
  }
  static std::vector<uint8_t> app_cshake(const std::vector<uint8_t> &name,
                                         const std::vector<uint8_t> &custom,
                                         const std::vector<uint8_t> &msg, size_t out_len) {
    Eg2KeccakSponge s;
    s.reset(136);
    absorb_prefix(s, name, custom);
    s.absorb(msg.data(), msg.size());
    s.finish(0x04);
    return squeeze(s, out_len);
  }

  void set_sideload_key(const uint32_t *share0, const uint32_t *share1, bool valid) {
    side_valid_ = valid;
    for (unsigned i = 0; i < 8; ++i) side_key_[i] = valid ? share0[i] ^ share1[i] : 0;
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    phase_ = kIdle;
    clear_state_window();
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == cmd_) {
      switch ((wdata & wmask) & 0x3f) {
        case kStart: start(); break;
        case kProcess: process(); break;
        case kRun: run(); break;
        case kDone: done(); break;
        case kStop: stop(); break;
        case kStateWrite: state_write(); break;
        case kContinue: resume(); break;
        default: break;
      }
    }
    update();
  }
  void win_write(uint16_t w, uint32_t off, unsigned len) override {
    if (w != fifo_win_) return;
    if (phase_ != kAbsorb) {
      set_value(err_code_, 0x2u << 24);  // SwPushedMsgFifo
      set_intr("kmac_err");
      return;
    }
    uint8_t b[4];
    const uint8_t *p = &win_mem(w)[off];
    for (unsigned i = 0; i < len; ++i) b[i] = p[i];
    if ((value(cfg_) >> 8) & 1) {  // msg_endianness: big-endian per access
      for (unsigned i = 0; i < len / 2; ++i) std::swap(b[i], b[len - 1 - i]);
    }
    sponge_.absorb(b, len);
  }

 private:
  enum Phase { kIdle, kAbsorb, kSqueeze, kStopped, kStateWriting };

  static std::vector<uint8_t> encode_string(const std::vector<uint8_t> &x) {
    std::vector<uint8_t> e = left_encode(x.size() * 8);
    e.insert(e.end(), x.begin(), x.end());
    return e;
  }
  static void absorb_bytepad(Eg2KeccakSponge &s, const std::vector<uint8_t> &x) {
    std::vector<uint8_t> w = left_encode(s.rate());
    s.absorb(w.data(), w.size());
    s.absorb(x.data(), x.size());
    s.pad_to_block();
  }
  static void absorb_prefix(Eg2KeccakSponge &s, const std::vector<uint8_t> &n,
                            const std::vector<uint8_t> &c) {
    std::vector<uint8_t> x = encode_string(n), cs = encode_string(c);
    x.insert(x.end(), cs.begin(), cs.end());
    absorb_bytepad(s, x);
  }
  static std::vector<uint8_t> squeeze(Eg2KeccakSponge &s, size_t n) {
    std::vector<uint8_t> out;
    while (out.size() < n) {
      for (unsigned i = 0; i < s.rate() && out.size() < n; ++i) out.push_back(s.byte(i));
      if (out.size() < n) s.permute();
    }
    return out;
  }

  unsigned rate() const {
    switch ((value(cfg_) >> 1) & 7) {
      case 0: return 168;  // L128
      case 1: return 144;  // L224
      case 2: return 136;  // L256
      case 3: return 104;  // L384
      default: return 72;  // L512
    }
  }
  unsigned mode() const { return (value(cfg_) >> 4) & 3; }  // 0 SHA3, 2 SHAKE, 3 cSHAKE
  bool kmac_en() const { return value(cfg_) & 1; }

  void start() {
    if (phase_ != kIdle) return;
    sponge_.reset(rate());
    if (mode() == 3 || kmac_en()) {
      std::vector<uint8_t> pre;
      for (unsigned i = 0; i < 11; ++i) {
        for (unsigned b = 0; b < 4; ++b) pre.push_back(uint8_t(value(prefix_[i]) >> (8 * b)));
      }
      absorb_bytepad(sponge_, pre);
    }
    if (kmac_en()) {
      static const unsigned kKeyBytes[] = {16, 24, 32, 48, 64};
      unsigned kl = value(key_len_) & 7;
      unsigned nbytes = kl < 5 ? kKeyBytes[kl] : 32;
      bool side = (value(cfg_) >> 12) & 1;
      std::vector<uint8_t> key;
      for (unsigned i = 0; i < nbytes / 4; ++i) {
        uint32_t w = side ? side_key_[i % 8] : value(key0_[i]) ^ value(key1_[i]);
        for (unsigned b = 0; b < 4; ++b) key.push_back(uint8_t(w >> (8 * b)));
      }
      absorb_bytepad(sponge_, encode_string(key));
    }
    phase_ = kAbsorb;
  }
  void process() {
    if (phase_ != kAbsorb) return;
    static const uint8_t kDomain[] = {0x06, 0x06, 0x1f, 0x04};
    sponge_.finish(kDomain[mode()]);
    phase_ = kSqueeze;
    show_state();
    set_intr("kmac_done");
  }
  void run() {
    if (phase_ != kSqueeze) return;
    sponge_.permute();
    show_state();
    set_intr("kmac_done");
  }
  void done() {
    if (phase_ == kIdle) return;
    phase_ = kIdle;
    clear_state_window();
  }
  // Context switching (programmer's guide "Saving a context", "Restoring a context").
  bool sideloaded() const { return (value(cfg_) >> 12) & 1; }
  void stop() {
    if (phase_ != kAbsorb) return;
    if (sideloaded()) {
      set_intr("kmac_err");  // ErrSwSaveRestoreSideload: discarded
      return;
    }
    if (!sponge_.block_aligned()) {
      sponge_.drop_partial();  // ErrSwStopNotBlockAligned: the partial block is discarded
      set_intr("kmac_err");
    }
    phase_ = kStopped;
    show_state(200);
    set_intr("kmac_done");
  }
  void state_write() {
    if (phase_ != kIdle) return;
    if (sideloaded()) {
      set_intr("kmac_err");
      return;
    }
    clear_state_window();
    phase_ = kStateWriting;
  }
  void resume() {
    if (phase_ != kStateWriting) return;
    sponge_.reset(rate());
    const std::vector<uint8_t> &m = win_mem(state_win_);
    bool swap = (value(cfg_) >> 9) & 1;
    for (unsigned i = 0; i < 200; ++i) {
      unsigned at = swap ? (i & ~3u) + 3 - (i & 3) : i;
      sponge_.set_byte(i, m[at] ^ m[0x100 + at]);  // share 0 ^ share 1
    }
    clear_state_window();
    phase_ = kAbsorb;
  }
  void show_state(unsigned n = 200) {
    std::vector<uint8_t> &m = win_mem(state_win_);
    std::fill(m.begin(), m.end(), 0);
    bool swap = (value(cfg_) >> 9) & 1;  // state_endianness
    for (unsigned i = 0; i < n; ++i) {
      unsigned at = swap ? (i & ~3u) + 3 - (i & 3) : i;
      m[at] = sponge_.byte(i);
    }
  }
  void clear_state_window() {
    std::vector<uint8_t> &m = win_mem(state_win_);
    std::fill(m.begin(), m.end(), 0);
  }
  void update() {
    // STATUS: sha3_idle (0), sha3_absorb (1), sha3_squeeze (2), fifo_empty (14).
    uint32_t st = (1u << 14);
    switch (phase_) {
      case kIdle: st |= 1u; break;
      case kAbsorb: st |= 2u; break;
      case kSqueeze: st |= 4u; break;
      case kStopped: st |= 8u; break;
      case kStateWriting: st |= 16u; break;
    }
    set_value(status_, st);
    set_value(cfg_regwen_, phase_ == kIdle ? 1u : 0u);
    set_intr_level("fifo_empty", false);
  }

  uint16_t cfg_regwen_, cfg_, cmd_, status_, key_len_, err_code_, key0_[16], key1_[16],
      prefix_[11], state_win_, fifo_win_;
  Eg2KeccakSponge sponge_;
  Phase phase_ = kIdle;

 public:
  bool idle() const override { return phase_ == kIdle; }

 private:
  bool side_valid_ = false;
  uint32_t side_key_[8] = {};
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CRYPTO_HASH_H_
