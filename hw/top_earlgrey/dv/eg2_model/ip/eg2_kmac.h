// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// kmac   SHA3, SHAKE, cSHAKE and KMAC as a Keccak-f[1600] sponge (the permutation of digestpp,
//        hw/ip/kmac/dv/dpi/vendor, public domain, the DPI reference of the block's DV). START
//        absorbs bytepad(PREFIX, rate) for cSHAKE and bytepad(encode_string(key), rate) in KMAC
//        mode (theory of operation; software writes encode_string(N) || encode_string(S) to PREFIX
//        and appends right_encode(L) itself); PROCESS pads and permutes; the rate part of the state
//        is readable in the STATE window while squeezing (share 1 reads zero: masking is not
//        modelled); RUN permutes again; DONE clears. STOP / STATE_WRITE / CONTINUE switch
//        contexts as the programmer's guide describes (the full state is exposed and restored).
//        CFG and ENTROPY_REFRESH_THRESHOLD use per-field two-write shadowing;
//        reads/reset cancel staging (util/reggen/README.md). A mismatch raises
//        recov_operation_err and STATUS.ALERT_RECOV_CTRL_UPDATE_ERR. A successful
//        CFG update restarts configuration and clears this indication (kmac.hjson,
//        STATUS); this is separate from CMD.err_processed and ERR_CODE command
//        errors. Storage fault injection and shadow timing are not modelled.
//        Entropy (EDN, ENTROPY_SEED) only feeds
//        masking and is not used. The application interface for keymgr and rom_ctrl is
//        Eg2Kmac::app_kmac / app_cshake.

#ifndef EG2_MODEL_IP_EG2_KMAC_H_
#define EG2_MODEL_IP_EG2_KMAC_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/kmac.hpp"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"

namespace eg2_model {

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
    enable_shadowed_registers();
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
    register_sideload_expect();
  }

  // What the chip bench does for chip_sw_keymgr_dpe_sideload_kmac_vseq: it reads the key
  // keymgr sideloaded, computes a 32-byte KMAC128 digest of the test's four-byte message with no
  // customization string, and backdoor-writes the digest. Here: --stim
  // kmac.sideload_expect=<message symbol>:<expected symbol>, the same digestpp reference used by
  // the KMAC model and the key loaded by set_sideload_key().
  void register_sideload_expect() {
    sys_.stim_handlers["kmac.sideload_expect"] = [this](const std::string &v) {
      size_t c = v.find(':');
      if (c == std::string::npos || !side_valid_ || !sys_.read_test_symbol) return false;
      uint8_t msg[4], key[32], out[32];
      if (!sys_.read_test_symbol(v.substr(0, c), msg, sizeof msg)) return false;
      for (unsigned i = 0; i < 8; ++i) {
        for (unsigned b = 0; b < 4; ++b) key[4 * i + b] = uint8_t(side_key_[i] >> (8 * b));
      }
      digestpp::kmac128 kmac(sizeof(out) * 8);
      kmac.set_key(key, sizeof key);
      kmac.absorb(msg, sizeof msg);
      kmac.digest(out, sizeof out);
      return sys_.write_test_symbol(v.substr(c + 1), out, sizeof out);
    };
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
    for (unsigned i = 0; i < 8; ++i) side_key_[i] = share0[i] ^ share1[i];
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    phase_ = kIdle;
    error_pending_ = false;
    shadow_error_ = false;
    clear_state_window();
    update();
  }

 protected:
  void shadow_update(uint16_t r, bool error) override {
    if (error) {
      shadow_error_ = true;
      raise_alert("recov_operation_err");
    } else if (r == cfg_) {
      shadow_error_ = false;
    }
    update();
  }
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == cmd_) {
      // CMD.err_processed acts while an error-handling state machine is waiting for software
      // (registers, CMD.err_processed). A software operation raises kmac_done when recovery has
      // flushed the engine and it is ready again (programmer's guide, Error Handling).
      if (((wdata & wmask) & (1u << 10)) && error_pending_) {
        error_pending_ = false;
        phase_ = kIdle;
        clear_state_window();
        set_intr("kmac_done");
      }
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
    if (phase_ != kIdle || shadow_error_) return;
    sponge_.reset(rate());
    bool side = (value(cfg_) >> 12) & 1;
    // KMAC theory of operation "Key invalid error": using an invalid sideloaded key raises
    // KeyNotValid. Assumption: follow the FSM figure's err_processed recovery path; the prose in
    // the same section instead says KeyNotValid enters a dead-end state and waits for IP reset.
    if (kmac_en() && side && !side_valid_) {
      set_value(err_code_, 0x1u << 24);
      set_intr("kmac_err");
      error_pending_ = true;
      phase_ = kAbsorb;
      return;
    }
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
    // Assumption from chip_sw_keymgr_sideload_kmac_vseq: a KMAC operation with a sideloaded key
    // keeps the capacity hidden; the rate still contains the requested digest. KMAC theory of
    // operation "Software Interface" instead says software can access the full 1600-bit state.
    if (kmac_en() && sideloaded()) n = std::min(n, rate());
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
    if (shadow_error_) st |= 1u << 29;
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
  bool error_pending_ = false, shadow_error_ = false;
  uint32_t side_key_[8] = {};
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_KMAC_H_
