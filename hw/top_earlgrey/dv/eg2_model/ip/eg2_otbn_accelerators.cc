// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_otbn.h"

namespace eg2_model {

void Eg2OtbnCore::kmac_reset() {
  kstate_ = kKIdle;
  kmac_cfg_ = kmac_strb_ = kmac_err_ = 0;
  kmac_rsp_valid_ = kmac_read_s0_ = kmac_read_s1_ = false;
  kmac_no_more_msg_ = kmac_rejected_ = false;
  kmac_msg_.clear();
}

bool Eg2OtbnCore::kmac_ready() const {
  return kstate_ == kKIdle || kstate_ == kKWaitForMsg || kstate_ == kKReceiving;
}

uint32_t Eg2OtbnCore::kmac_status() const {
  return uint32_t(kmac_ready()) | uint32_t(kmac_rsp_valid_) << 1 | kmac_err_;
}

bool Eg2OtbnCore::kmac_decode() {
  uint32_t c = kmac_cfg_;
  kx_ = c & 1;
  kstrength_ = (c >> 1) & 7;
  kmode_ = (c >> 4) & 3;
  if (((c >> 16) & 1) != (~c & 1) || ((c >> 17) & 7) != (~(c >> 1) & 7) ||
      ((c >> 20) & 3) != (~(c >> 4) & 3))
    return false;
  if (kstrength_ > 4) return false;
  if (kx_ && (kmode_ == 0 || kmode_ == 3)) return false;
  if (kmode_ == 0) return kstrength_ != 0;
  return kstrength_ == 0 || kstrength_ == 2;
}

void Eg2OtbnCore::kmac_cmd(uint32_t cmd) {
  if (!cmd) return;
  enum { kStart = 1, kSend = 2, kProcess = 4, kDone = 8, kClose = 16 };
  uint32_t allowed = kstate_ == kKIdle         ? kStart
                     : kstate_ == kKWaitForMsg ? (kmac_no_more_msg_ ? kProcess : kSend | kProcess)
                     : kstate_ == kKReceiving  ? kDone
                                               : kClose;
  if ((cmd & (cmd - 1)) || !(cmd & allowed)) {
    kmac_err_ |= kKmacCtrlError;  // the command is ignored
    return;
  }
  switch (cmd) {
    case kStart:
      kmac_rejected_ = !kmac_decode();
      kmac_msg_.clear();
      kmac_no_more_msg_ = false;
      kstate_ = kKWaitForMsg;
      break;
    case kSend:
      // Up to four 64-bit beats, least significant first, share 0 XOR share 1, KMAC_STRB a
      // byte strobe; an empty or partial beat ends the message.
      for (unsigned beat = 0; beat < 4; ++beat) {
        unsigned strb = (kmac_strb_ >> (8 * beat)) & 0xff;
        if (!strb) {
          kmac_no_more_msg_ = true;
          break;
        }
        uint64_t v = kmac_s0_.q[beat] ^ kmac_s1_.q[beat];
        unsigned n = 0;
        while (n < 8 && (strb >> n) & 1) ++n;
        for (unsigned i = 0; i < n; ++i) kmac_msg_.push_back(uint8_t(v >> (8 * i)));
        if (strb != 0xff) {
          kmac_no_more_msg_ = true;
          break;
        }
      }
      break;
    case kProcess:
      kstate_ = kKReceiving;
      if (kmac_rejected_) {
        kmac_err_ |= kKmacRspError;
        kmac_rsp_valid_ = true;
      } else {
        kmac_digest_start();
        kmac_next_beat();
      }
      break;
    case kDone:
      if (kmac_rejected_) kmac_err_ |= kKmacRspError;
      kstate_ = kKWaitForClose;
      kmac_rsp_valid_ = true;  // the final response
      break;
    case kClose:
      kmac_err_ = 0;
      kmac_rsp_valid_ = false;
      kstate_ = kKIdle;
      break;
  }
}

void Eg2OtbnCore::kmac_digest_start() {
  static const unsigned kRate[] = {168, 144, 136, 104, 72};
  static const unsigned kBytes[] = {16, 28, 32, 48, 64};
  rate_ = kRate[kstrength_];
  ksponge_.reset(rate_);
  if (kmode_ == 2 || kmode_ == 3) {
    // cSHAKE: bytepad(encode_string(N) || encode_string(S)) from the KMAC block's PREFIX; with
    // N and S empty it is SHAKE (NIST SP 800-185 3.3). KMAC: N = "KMAC", the key,
    // right_encode(L).
    std::vector<uint8_t> prefix;
    if (kmode_ == 3) {
      prefix = OtbnKeccak::encode_string({'K', 'M', 'A', 'C'});
      std::vector<uint8_t> s = OtbnKeccak::encode_string({});
      prefix.insert(prefix.end(), s.begin(), s.end());
    } else if (hooks.kmac_prefix) {
      prefix = hooks.kmac_prefix();
    }
    bool empty = kmode_ == 2 && (prefix.size() < 4 || (prefix[0] == 1 && prefix[1] == 0 &&
                                                       prefix[2] == 1 && prefix[3] == 0));
    if (!empty) {
      ksponge_.absorb_bytepad(prefix);
      kdomain_ = 0x04;
    } else {
      kdomain_ = 0x1f;
    }
    if (kmode_ == 3) {
      std::vector<uint8_t> key = hooks.kmac_key ? hooks.kmac_key() : std::vector<uint8_t>(32, 0);
      ksponge_.absorb_bytepad(OtbnKeccak::encode_string(key));
    }
  } else {
    kdomain_ = kmode_ == 0 ? 0x06 : 0x1f;
  }
  ksponge_.absorb(kmac_msg_);
  if (kmode_ == 3) ksponge_.absorb(OtbnKeccak::right_encode(384));
  ksponge_.finish(kdomain_);
  // Bytes the digest has: SHA3 its size, KMAC 384 bits (fixed by the KMAC block); SHAKE and
  // cSHAKE one rate, or without end with EN_XOF.
  kmac_left_ = kmode_ == 0 ? kBytes[kstrength_] : kmode_ == 3 ? 48 : kx_ ? ~0u : rate_;
}

void Eg2OtbnCore::kmac_next_beat() {
  if (!kmac_left_) {
    kmac_rsp_valid_ = false;
    return;
  }
  uint64_t v = 0;
  unsigned n = std::min(kmac_left_, 8u);
  for (unsigned i = 0; i < n; ++i) v |= uint64_t(ksponge_.squeeze()) << (8 * i);
  if (kmac_left_ != ~0u) kmac_left_ -= n;  // ~0u: XOF output without end
  uint64_t m = urnd_.q[0] ^ urnd_.q[2];
  kmac_s0_.q[0] = v ^ m;
  kmac_s1_.q[0] = m;
  kmac_rsp_valid_ = true;
  kmac_read_s0_ = kmac_read_s1_ = false;
}

OtbnWide Eg2OtbnCore::kmac_read(bool s1) {
  OtbnWide v = s1 ? kmac_s1_ : kmac_s0_;
  if (kstate_ == kKReceiving && kmac_rsp_valid_ && !kmac_rejected_) {
    (s1 ? kmac_read_s1_ : kmac_read_s0_) = true;
    if (kmac_read_s0_ && kmac_read_s1_) kmac_next_beat();
  }
  return v;
}

bool Eg2OtbnCore::mai_op_valid(uint32_t op) {
  return op == kMaiA2b || op == kMaiB2a || op == kMaiSecAdd || op == kMaiSecAddMod;
}

bool Eg2OtbnCore::mai_busy() const { return cycles_ < mai_busy_until_; }

bool Eg2OtbnCore::mai_input_ready() const { return cycles_ >= mai_ready_at_; }

bool Eg2OtbnCore::mai_ctrl_ok(uint32_t v) const {
  if (v & ~0x3fu) return false;
  if (mai_busy() && ((v & 1) || ((v >> 1) & 0x1f) != mai_op_)) return false;
  if ((v & 1) && !mai_op_valid((v >> 1) & 0x1f)) return false;
  return true;
}

void Eg2OtbnCore::mai_land() {
  if (mai_pending_ && !mai_busy()) {
    mai_[0] = mai_res_[0];
    mai_[1] = mai_res_[1];
    mai_pending_ = false;
  }
}

void Eg2OtbnCore::mai_ctrl(uint32_t v) {
  mai_op_ = (v >> 1) & 0x1f;
  if (!(v & 1)) return;
  unsigned vector_cycles = mai_op_ == kMaiSecAdd ? 14 : mai_op_ == kMaiB2a ? 23 : 22;
  mai_busy_until_ = cycles_ + 3 + vector_cycles;
  mai_ready_at_ = cycles_ + 9;
  mai_pending_ = true;
  // Eight 32-bit lanes; MOD[31:0] the modulus.
  uint32_t mod = uint32_t(mod_.q[0]);
  OtbnWide &r0 = mai_res_[0], &r1 = mai_res_[1];
  const OtbnWide &a0 = mai_[2], &a1 = mai_[3], &b0 = mai_[4], &b1 = mai_[5];
  OtbnWide mask = urnd_;
  for (unsigned i = 0; i < 8; ++i) {
    uint32_t x0 = a0.word(i), x1 = a1.word(i), y0 = b0.word(i), y1 = b1.word(i);
    uint32_t m = mask.word(i);
    if (mai_op_ == kMaiB2a) {
      // Boolean shares of a in, arithmetic shares out: (s0 + s1) mod MOD = a mod MOD.
      uint32_t a = x0 ^ x1;
      uint32_t s0 = mod ? m % mod : 0;
      uint32_t s1 = mod ? uint32_t((uint64_t(a % mod) + mod - s0) % mod) : a;
      r0.set_word(i, s0);
      r1.set_word(i, s1);
      continue;
    }
    uint32_t res;
    if (mai_op_ == kMaiSecAdd) {
      res = (x0 ^ x1) + (y0 ^ y1);
    } else if (mai_op_ == kMaiSecAddMod) {
      // The inputs carry 2^32 - MOD: a sum that does not overflow 32 bits gets MOD back.
      uint64_t t = uint64_t(x0 ^ x1) + (y0 ^ y1);
      res = (t >> 32) ? uint32_t(t) : uint32_t(t + mod);
    } else {  // A2B: arithmetic shares of a in, Boolean shares of a mod MOD out
      uint64_t t = uint64_t(x0) + x1;
      res = mod ? uint32_t(t % mod) : uint32_t(t);
    }
    r0.set_word(i, res ^ m);
    r1.set_word(i, m);
  }
}

}  // namespace eg2_model
