// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

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

#ifndef EG2_MODEL_IP_EG2_HMAC_H_
#define EG2_MODEL_IP_EG2_HMAC_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha256.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha384.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha512.h"

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
    register_sideload_expect();
  }

  // What the chip bench does for chip_sw_keymgr_dpe_sideload_hmac_vseq: it reads the key
  // keymgr sideloaded, computes HMAC-SHA256 over the test's 142-byte message with CryptoC, and
  // backdoor-writes the digest in dif_hmac_digest_t's word order. Here: --stim
  // hmac.sideload_expect=<message symbol>:<expected symbol>, the same CryptoC SHA-256 reference
  // the model uses and the key loaded by set_sideload_key(). The bench reverses the digest bytes
  // because dif_hmac_finish() reads DIGEST_7 first and byte-swaps each word.
  void register_sideload_expect() {
    sys_.stim_handlers["hmac.sideload_expect"] = [this](const std::string &v) {
      size_t c = v.find(':');
      if (c == std::string::npos || !side_valid_ || !sys_.read_test_symbol) return false;
      uint8_t msg[142], key[64], inner[32], out[32], expected[32];
      if (!sys_.read_test_symbol(v.substr(0, c), msg, sizeof msg)) return false;
      for (unsigned i = 0; i < 16; ++i) {
        for (unsigned b = 0; b < 4; ++b) key[4 * i + b] = uint8_t(side_key_[i] >> (8 * b));
      }
      HASH_CTX ctx;
      SHA256_init(&ctx);
      for (auto &b : key) b ^= 0x36;
      HASH_update(&ctx, key, sizeof key);
      HASH_update(&ctx, msg, sizeof msg);
      std::memcpy(inner, HASH_final(&ctx), sizeof inner);
      SHA256_init(&ctx);
      for (auto &b : key) b ^= 0x36 ^ 0x5c;
      HASH_update(&ctx, key, sizeof key);
      HASH_update(&ctx, inner, sizeof inner);
      std::memcpy(out, HASH_final(&ctx), sizeof out);
      std::reverse_copy(out, out + sizeof out, expected);
      return sys_.write_test_symbol(v.substr(c + 1), expected, sizeof expected);
    };
  }

  // The sideloaded key: keymgr_dpe's hmac_key is a wide_hw_key_req, 512 bits (16 words). A key
  // that becomes invalid aborts a running sideloaded operation (digest cleared, SwInvalidConfig,
  // no hmac_done) and otherwise clears the digest silently (programmer's guide "Using a
  // sideloaded key").
  void set_sideload_key(const uint32_t *key16, bool valid) {
    bool revoked = side_valid_ && !valid;
    side_valid_ = valid;
    for (unsigned i = 0; i < 16; ++i) side_key_[i] = key16[i];
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
      // The running hash state (the intermediate results an operation in progress continues
      // from) is overwritten too. Whether an operation in progress then stops is not documented:
      // it continues, from the wiped state, so it cannot produce the digest it would have.
      uint32_t v = wdata & wmask;
      for (unsigned i = 0; i < 32; ++i) set_value(key_[i], v);
      for (unsigned i = 0; i < 16; ++i) set_value(digest_[i], v);
      for (auto &s : ctx_.state) s = (uint64_t(v) << 32) | v;
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

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_HMAC_H_
