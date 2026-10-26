// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// aes of the EG2 SystemC model (Phase B, crypto group), from hw/ip/aes/doc (theory of operation,
// programmer's guide) and aes.hjson. The block cipher is hw/ip/aes/model/aes.c, the reference
// model of the block's DV.
//
//   Modes        ECB, CBC, CFB (CFB-128), OFB, CTR, encryption and decryption, AES-128/192/256.
//                GCM (programmer's guide "Galois/Counter Mode"), 96-bit IV, 128-bit tag:
//                CTRL_GCM_SHADOWED.PHASE INIT (H = E(0) and S = E(IV || 1) once IV_0..3 are
//                written, or on two STARTs in manual mode; the counter then holds IV || 2), AAD
//                (GHASH only), TEXT (CTR with inc32, GHASH over the ciphertext), TAG (the length
//                block; output GHASH ^ S), SAVE (outputs the GHASH state, then reads as GCM_TAG;
//                only after a first AAD or TEXT block) and RESTORE (loads the GHASH state).
//                NUM_VALID_BYTES zeroes a last partial block's invalid bytes at GHASH's input.
//                The saved state's encoding is not documented: the model saves GHASH's Y as is.
//                GHASH-only blocks (AAD, TAG, RESTORE) take kGhashCycles, a value the
//                documentation does not give.
//   Operation    automatic (a block starts once DATA_IN_0..3 are all written) and manual
//                (TRIGGER.START). "If the output has not been read, the unit stalls" (STATUS.STALL)
//                until DATA_OUT_0..3 have all been read. The IV registers advance per block.
//   Latency      a block takes 56/66/72 cycles of the main clock (Eg2System's) for AES-128/192/256 (the
//                masked DOM implementation, README "Features"; top_earlgrey.hjson: SecMasking 1,
//                SboxImplDom). ECB/CBC decryption after a key change first spends one block's
//                latency on the decryption start key (theory of operation; the model also counts a
//                CTRL write, which can change the key length or source). The unit is busy (not
//                idle, no output) for that time; the result appears at its end.
//   Registers    all little-endian; the CTR increment is big-endian (programmer's guide). The key
//                is KEY_SHARE0 ^ KEY_SHARE1, or the key keymgr sideloads (set_sideload_key) when
//                CTRL.SIDELOAD is set. Writes to CTRL, KEY and IV while the unit is busy are
//                ignored.
//   Entropy      the masking PRNG is reseeded from edn0 after reset (TRIGGER.PRNG_RESEED),
//                when a key is written and CTRL_AUX.KEY_TOUCH_FORCES_RESEED is set, on
//                TRIGGER.PRNG_RESEED, and every 1, 64 or 8192 blocks (CTRL.PRNG_RESEED_RATE),
//                before the block. The block counter follows aes_control_fsm.sv: zero (expired)
//                at reset, loaded with the rate minus one by every CTRL write, and a block
//                started while it is zero reseeds first and reloads it. The documentation gives
//                no reseed width; from the RTL: TRIGGER.PRNG_RESEED (also set at reset and by a key
//                touch) reseeds the clearing PRNG (64 bits) and the masking PRNG (Bivium, 177 bits
//                in 32-bit parts), 2 + 6 = 8 words from edn0; the block counter reseeds the masking
//                PRNG alone, 6 words (aes_control_fsm.sv, aes_prng_clearing.sv,
//                prim_trivium.sv). While edn0 has none the unit waits and is not idle, as the PRNGs
//                wait for EDN in hardware. Masking itself is not modelled: the results are the
//                unmasked cipher's.
//   Clearing     TRIGGER.KEY_IV_DATA_IN_CLEAR / DATA_OUT_CLEAR clear those registers (to zero; the
//                hardware writes pseudo-random data). Both are set at reset, so the unit becomes
//                idle after reset only once it could reseed; it retries on its own every
//                microsecond, as the hardware reseeds right after reset (from edn0's boot-time
//                mode, which the test ROM enables).

#ifndef EG2_MODEL_IP_EG2_CRYPTO_AES_H_
#define EG2_MODEL_IP_EG2_CRYPTO_AES_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "../eg2_ip_model.h"
#include "eg2_crypto_entropy.h"
#include "hw/ip/aes/model/aes.h"

namespace eg2_model {

class Eg2Aes : public Eg2IpModel {
 public:
  Eg2Aes(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    for (unsigned i = 0; i < 8; ++i) {
      key0_[i] = reg(("KEY_SHARE0_" + std::to_string(i)).c_str());
      key1_[i] = reg(("KEY_SHARE1_" + std::to_string(i)).c_str());
    }
    for (unsigned i = 0; i < 4; ++i) {
      iv_[i] = reg(("IV_" + std::to_string(i)).c_str());
      din_[i] = reg(("DATA_IN_" + std::to_string(i)).c_str());
      dout_[i] = reg(("DATA_OUT_" + std::to_string(i)).c_str());
      set_modelled(iv_[i]);
      set_modelled(dout_[i]);
    }
    ctrl_ = reg("CTRL_SHADOWED");
    aux_ = reg("CTRL_AUX_SHADOWED");
    trigger_ = reg("TRIGGER");
    status_ = reg("STATUS");
    gcm_ctrl_ = reg("CTRL_GCM_SHADOWED");
    set_modelled(gcm_ctrl_);
    set_modelled(trigger_);
    set_modelled(status_);
    start_after_reset();
  }

  // keymgr's sideload interface (theory of operation "Sideload").
  void set_sideload_key(const uint32_t *share0, const uint32_t *share1, bool valid) {
    side_valid_ = valid;
    for (unsigned i = 0; i < 8; ++i) side_key_[i] = valid ? share0[i] ^ share1[i] : 0;
    dec_key_stale_ = true;
    progress();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    start_after_reset();
  }
  // The hardware reseeds on its own after reset, while software does other things: retry every
  // microsecond until edn0 delivers.
  void on_time(uint64_t now) override {
    progress();
    if (computing_) sys_.schedule(this, done_at_);
    else if (reseed_needed_) sys_.schedule(this, now + kRetryPs);
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t, uint32_t) override {
    bool cfg = r == ctrl_ || r == aux_;
    bool is_key = false, is_iv = false, is_din = false;
    for (unsigned i = 0; i < 8; ++i) is_key |= r == key0_[i] || r == key1_[i];
    for (unsigned i = 0; i < 4; ++i) is_iv |= r == iv_[i];
    for (unsigned i = 0; i < 4; ++i) is_din |= r == din_[i];
    if ((cfg || is_key || is_iv || r == gcm_ctrl_) && !idle()) {
      set_value(r, old);  // ignored while busy (theory of operation "Initialization")
      return;
    }
    if (r == ctrl_) {
      // A new configuration starts a new message: the data input tracking restarts, and in GCM
      // the internal counters and tracking ("Writing this register clears internal counters").
      din_written_ = 0;
      block_ctr_ = block_ctr_reload();
      dec_key_stale_ = true;
      gcm_reset();
    } else if (r == gcm_ctrl_) {
      gcm_ctrl_write(old);
    } else if (is_iv) {
      for (unsigned i = 0; i < 4; ++i) {
        if (r == iv_[i]) iv_written_ |= 1u << i;
      }
    } else if (is_key) {
      dec_key_stale_ = true;
      if (value(aux_) & 1) request_reseed(kTriggerReseedWords);  // KEY_TOUCH_FORCES_RESEED
    } else if (is_din) {
      for (unsigned i = 0; i < 4; ++i) {
        if (r == din_[i]) din_written_ |= 1u << i;
      }
    } else if (r == trigger_) {
      uint32_t t = value(trigger_);
      if (t & (1u << 3)) request_reseed(kTriggerReseedWords);
      if (t & 1) start_ = true;
      set_value(trigger_, t & ~1u);
    }
    progress();
  }
  void reg_before_read(uint16_t r) override {
    if (r == status_) progress();
  }
  void reg_after_read(uint16_t r) override {
    for (unsigned i = 0; i < 4; ++i) {
      if (r == dout_[i]) dout_read_ |= 1u << i;
    }
    if (output_valid_ && dout_read_ == 0xf) {
      output_valid_ = false;
      progress();
    }
  }

 private:
  enum Mode { kEcb = 1, kCbc = 2, kCfb = 4, kOfb = 8, kCtr = 16, kGcm = 32 };

  void start_after_reset() {
    // TRIGGER resets to KEY_IV_DATA_IN_CLEAR | DATA_OUT_CLEAR | PRNG_RESEED (0xe).
    reseed_needed_ = false;
    request_reseed(kTriggerReseedWords);
    block_ctr_ = 0;
    din_written_ = 0;
    dout_read_ = 0xf;
    output_valid_ = false;
    start_ = false;
    stalled_ = false;
    side_valid_ = false;
    computing_ = false;
    dec_key_stale_ = true;
    gcm_reset();
    set_value(gcm_ctrl_, kGcmCtrlReset);
    progress();
    if (reseed_needed_) sys_.schedule(this, sys_.now_ps() + kRetryPs);
  }
  static constexpr uint64_t kRetryPs = 1000000;  // 1 us
  static constexpr unsigned kTriggerReseedWords = 8, kBlockReseedWords = 6;

  // A reseed asked for while one is outstanding waits for the larger of the two.
  void request_reseed(unsigned words) {
    if (!reseed_needed_) reseed_words_ = 0, reseed_need_ = 0;
    reseed_needed_ = true;
    reseed_need_ = std::max(reseed_need_, words);
  }

  bool manual() const { return (value(ctrl_) >> 15) & 1; }
  unsigned mode() const { return (value(ctrl_) >> 2) & 0x3f; }
  bool decrypt() const { return (value(ctrl_) & 3) == 2; }
  unsigned key_len() const {
    switch ((value(ctrl_) >> 8) & 7) {
      case 1: return 16;
      case 2: return 24;
      default: return 32;
    }
  }
  bool sideload() const { return (value(ctrl_) >> 11) & 1; }
  uint32_t block_ctr_reload() const {
    switch ((value(ctrl_) >> 12) & 7) {
      case 1: return 0;
      case 2: return 63;
      case 4: return 8191;
      default: return 0;
    }
  }
  bool idle() const override {
    return !reseed_needed_ && !(value(trigger_) & 0x6) && !stalled_ && !computing_ &&
           !start_pending();
  }
  // Cycles of a block (masked DOM S-Box, AES-128/192/256).
  unsigned block_cycles() const { return key_len() == 16 ? 56 : key_len() == 24 ? 66 : 72; }
  bool start_pending() const {
    if (mode() == kGcm && gcm_phase() == kGcmInit) {
      // INIT runs once the IV is complete (automatic mode), or on each START (manual mode).
      return gcm_init_step_ < 2 && (manual() ? start_ : iv_written_ == 0xf);
    }
    if (mode() == kGcm && gcm_phase() == kGcmSave) return false;
    return manual() ? start_ : din_written_ == 0xf;
  }

  // Take the reseed's words from edn0; true once all have arrived.
  bool reseed() {
    if (!reseed_needed_) return true;
    auto *edn = dynamic_cast<Eg2Edn *>(sys_.model("edn0"));
    uint32_t w;
    while (reseed_words_ < reseed_need_ && edn && edn->get(&w)) ++reseed_words_;
    if (reseed_words_ < reseed_need_) return false;
    reseed_words_ = 0;
    reseed_needed_ = false;
    return true;
  }

  void progress() {
    if (computing_) {
      if (sys_.now_ps() < done_at_) {
        update();
        return;
      }
      for (unsigned i = 0; i < 4; ++i) set_value(iv_[i], res_iv_[i]);
      computing_ = false;
      if (has_output_) {
        for (unsigned i = 0; i < 4; ++i) set_value(dout_[i], res_out_[i]);
        output_valid_ = true;
        dout_read_ = 0;
      }
    }
    if (!reseed()) {
      update();
      return;
    }
    uint32_t t = value(trigger_);
    if (t & (1u << 1)) {  // KEY_IV_DATA_IN_CLEAR
      for (unsigned i = 0; i < 8; ++i) set_value(key0_[i], 0), set_value(key1_[i], 0);
      for (unsigned i = 0; i < 4; ++i) set_value(iv_[i], 0), set_value(din_[i], 0);
      din_written_ = 0;
    }
    if (t & (1u << 2)) {  // DATA_OUT_CLEAR
      for (unsigned i = 0; i < 4; ++i) set_value(dout_[i], 0);
    }
    set_value(trigger_, t & ~0xeu);
    if (start_pending()) {
      if (output_valid_ && !manual()) {
        stalled_ = true;  // the previous output has not been read
      } else if (block_ctr_ == 0) {
        request_reseed(kBlockReseedWords);
        if (reseed()) {
          block_ctr_ = block_ctr_reload() + 1;  // process() counts this block
          process();
        }
      } else {
        process();
      }
    }
    update();
  }

  void process() {
    has_output_ = true;
    if (mode() == kGcm) {
      gcm_process();
      return;
    }
    uint8_t key[32] = {};
    for (unsigned i = 0; i < 8; ++i) {
      uint32_t k = sideload() ? side_key_[i] : value(key0_[i]) ^ value(key1_[i]);
      for (unsigned b = 0; b < 4; ++b) key[4 * i + b] = uint8_t(k >> (8 * b));
    }
    if (sideload() && !side_valid_) return;  // waits for keymgr's key
    uint8_t in[16], iv[16], out[16], tmp[16];
    for (unsigned i = 0; i < 4; ++i) {
      for (unsigned b = 0; b < 4; ++b) {
        in[4 * i + b] = uint8_t(value(din_[i]) >> (8 * b));
        iv[4 * i + b] = uint8_t(value(iv_[i]) >> (8 * b));
      }
    }
    int kl = int(key_len());
    switch (mode()) {
      case kEcb:
        if (decrypt()) aes_decrypt_block(in, key, kl, out);
        else aes_encrypt_block(in, key, kl, out);
        break;
      case kCbc:
        if (decrypt()) {
          aes_decrypt_block(in, key, kl, out);
          for (unsigned i = 0; i < 16; ++i) out[i] ^= iv[i];
          std::memcpy(iv, in, 16);
        } else {
          for (unsigned i = 0; i < 16; ++i) tmp[i] = in[i] ^ iv[i];
          aes_encrypt_block(tmp, key, kl, out);
          std::memcpy(iv, out, 16);
        }
        break;
      case kCfb:
        aes_encrypt_block(iv, key, kl, tmp);
        for (unsigned i = 0; i < 16; ++i) out[i] = in[i] ^ tmp[i];
        std::memcpy(iv, decrypt() ? in : out, 16);
        break;
      case kOfb:
        aes_encrypt_block(iv, key, kl, tmp);
        for (unsigned i = 0; i < 16; ++i) out[i] = in[i] ^ tmp[i];
        std::memcpy(iv, tmp, 16);
        break;
      case kCtr:
        aes_encrypt_block(iv, key, kl, tmp);
        for (unsigned i = 0; i < 16; ++i) out[i] = in[i] ^ tmp[i];
        for (int i = 15; i >= 0 && ++iv[i] == 0; --i) {
        }
        break;
      default:
        return;  // AES_NONE: no operation
    }
    for (unsigned i = 0; i < 4; ++i) {
      uint32_t o = 0, v = 0;
      for (unsigned b = 0; b < 4; ++b) {
        o |= uint32_t(out[4 * i + b]) << (8 * b);
        v |= uint32_t(iv[4 * i + b]) << (8 * b);
      }
      res_out_[i] = o;
      res_iv_[i] = v;
    }
    din_written_ = 0;
    start_ = false;
    stalled_ = false;
    output_valid_ = false;
    unsigned cycles = block_cycles();
    if (decrypt() && (mode() == kEcb || mode() == kCbc) && dec_key_stale_) cycles *= 2;
    dec_key_stale_ = false;
    computing_ = true;
    done_at_ = sys_.clock_time_of_cycle(
        Eg2System::Clk::kMain, sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps()) + cycles);
    sys_.schedule(this, done_at_);
    if (block_ctr_) --block_ctr_;
  }

  // ---- GCM (programmer's guide "Galois/Counter Mode"; aes.hjson CTRL_GCM_SHADOWED) ----------
  enum GcmPhase {
    kGcmInit = 1, kGcmRestore = 2, kGcmAad = 4, kGcmText = 8, kGcmSave = 16, kGcmTag = 32
  };
  static constexpr uint32_t kGcmCtrlReset = (16u << 6) | kGcmInit;  // NUM_VALID_BYTES 16, INIT
  static constexpr unsigned kGhashCycles = 32;  // not documented: an assumed GHASH-only latency

  unsigned gcm_phase() const {
    unsigned p = value(gcm_ctrl_) & 0x3f;
    // One-hot; anything else maps to GCM_INIT (aes.hjson PHASE).
    return (p && !(p & (p - 1))) ? p : unsigned(kGcmInit);
  }
  unsigned gcm_valid_bytes() const {
    unsigned n = (value(gcm_ctrl_) >> 6) & 0x1f;
    return (n == 0 || n > 16) ? 16 : n;  // 0 maps to 16 (aes.hjson NUM_VALID_BYTES)
  }

  void gcm_reset() {
    gcm_init_step_ = 0;
    gcm_blocks_ = 0;
    iv_written_ = 0;
    std::memset(gcm_h_, 0, 16);
    std::memset(gcm_s_, 0, 16);
    std::memset(gcm_y_, 0, 16);
  }

  void gcm_ctrl_write(uint32_t old) {
    unsigned prev = old & 0x3f, next = gcm_phase();
    if (next == kGcmSave) {
      // Only after a first AAD or TEXT block; otherwise the write does not go through.
      if ((prev != kGcmAad && prev != kGcmText) || gcm_blocks_ == 0) {
        set_value(gcm_ctrl_, old);
        return;
      }
      // The GHASH state goes to DATA_OUT, and the phase then reads as GCM_TAG.
      set_value(gcm_ctrl_, (value(gcm_ctrl_) & ~0x3fu) | kGcmTag);
      for (unsigned i = 0; i < 4; ++i) set_value(dout_[i], le_word(gcm_y_ + 4 * i));
      output_valid_ = true;
      dout_read_ = 0;
      return;
    }
    if (next == kGcmInit && prev != kGcmInit) {
      gcm_init_step_ = 0;
      iv_written_ = 0;
    }
    din_written_ = 0;
  }

  static uint32_t le_word(const uint8_t *b) {
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
  }
  static void inc32(uint8_t *cb) {
    for (int i = 15; i >= 12 && ++cb[i] == 0; --i) {
    }
  }
  // GHASH: Y = (Y ^ X) * H in GF(2^128) (SP 800-38D Algorithm 1 and 2).
  void ghash(const uint8_t *x) {
    uint8_t z[16] = {}, v[16];
    for (unsigned k = 0; k < 16; ++k) gcm_y_[k] ^= x[k];
    std::memcpy(v, gcm_h_, 16);
    for (unsigned i = 0; i < 128; ++i) {
      if (gcm_y_[i / 8] & (0x80 >> (i % 8)))
        for (unsigned k = 0; k < 16; ++k) z[k] ^= v[k];
      bool lsb = v[15] & 1;
      for (unsigned k = 15; k > 0; --k) v[k] = uint8_t((v[k] >> 1) | (v[k - 1] << 7));
      v[0] = uint8_t(v[0] >> 1);
      if (lsb) v[0] ^= 0xe1;
    }
    std::memcpy(gcm_y_, z, 16);
  }

  void gcm_process() {
    uint8_t key[32] = {};
    for (unsigned i = 0; i < 8; ++i) {
      uint32_t k = sideload() ? side_key_[i] : value(key0_[i]) ^ value(key1_[i]);
      for (unsigned b = 0; b < 4; ++b) key[4 * i + b] = uint8_t(k >> (8 * b));
    }
    if (sideload() && !side_valid_) return;  // waits for keymgr's key
    uint8_t in[16], iv[16], out[16] = {};
    for (unsigned i = 0; i < 4; ++i) {
      for (unsigned b = 0; b < 4; ++b) {
        in[4 * i + b] = uint8_t(value(din_[i]) >> (8 * b));
        iv[4 * i + b] = uint8_t(value(iv_[i]) >> (8 * b));
      }
    }
    int kl = int(key_len());
    unsigned cycles = kGhashCycles;
    has_output_ = false;
    switch (gcm_phase()) {
      case kGcmInit: {
        // 1) H = E_K(0), counter inc32; 2) S = E_K(counter), counter inc32. One step per START
        // in manual mode, both in automatic mode.
        unsigned steps = manual() ? 1 : 2 - gcm_init_step_;
        for (unsigned s = 0; s < steps && gcm_init_step_ < 2; ++s, ++gcm_init_step_) {
          uint8_t zero[16] = {};
          aes_encrypt_block(gcm_init_step_ == 0 ? zero : iv, key, kl,
                            gcm_init_step_ == 0 ? gcm_h_ : gcm_s_);
          inc32(iv);
        }
        std::memset(gcm_y_, 0, 16);
        cycles = steps * block_cycles();
        break;
      }
      case kGcmRestore:
        std::memcpy(gcm_y_, in, 16);
        break;
      case kGcmAad: {
        uint8_t x[16] = {};
        std::memcpy(x, in, gcm_valid_bytes());
        ghash(x);
        ++gcm_blocks_;
        break;
      }
      case kGcmText: {
        uint8_t ks[16], x[16] = {};
        aes_encrypt_block(iv, key, kl, ks);
        inc32(iv);
        for (unsigned i = 0; i < 16; ++i) out[i] = in[i] ^ ks[i];
        // GHASH over the ciphertext: the output when encrypting, the input when decrypting.
        std::memcpy(x, decrypt() ? in : out, gcm_valid_bytes());
        ghash(x);
        ++gcm_blocks_;
        has_output_ = true;
        cycles = block_cycles();
        break;
      }
      case kGcmTag:
        ghash(in);  // len(A) || len(C)
        for (unsigned i = 0; i < 16; ++i) out[i] = gcm_y_[i] ^ gcm_s_[i];
        has_output_ = true;
        break;
      default:
        return;
    }
    for (unsigned i = 0; i < 4; ++i) {
      res_out_[i] = le_word(out + 4 * i);
      res_iv_[i] = le_word(iv + 4 * i);
    }
    din_written_ = 0;
    start_ = false;
    stalled_ = false;
    if (has_output_) output_valid_ = false;
    computing_ = true;
    done_at_ = sys_.clock_time_of_cycle(
        Eg2System::Clk::kMain, sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps()) + cycles);
    sys_.schedule(this, done_at_);
    if (block_ctr_) --block_ctr_;
  }

  void update() {
    // STATUS: IDLE (0), STALL (1), OUTPUT_LOST (2), OUTPUT_VALID (3), INPUT_READY (4).
    uint32_t st = 0;
    if (idle()) st |= 1;
    if (stalled_) st |= 2;
    if (output_valid_) st |= 8;
    if (!stalled_ && !(manual() && start_) && !reseed_needed_) st |= 16;
    set_value(status_, st);
  }

  uint16_t key0_[8], key1_[8], iv_[4], din_[4], dout_[4];
  uint16_t ctrl_, aux_, trigger_, status_, gcm_ctrl_;
  bool reseed_needed_ = true, output_valid_ = false, start_ = false, stalled_ = false;
  bool side_valid_ = false, computing_ = false, dec_key_stale_ = true;
  uint64_t done_at_ = 0;
  uint32_t res_out_[4] = {}, res_iv_[4] = {};
  uint32_t side_key_[8] = {};
  unsigned reseed_words_ = 0, reseed_need_ = 0;
  uint32_t block_ctr_ = 0;
  unsigned din_written_ = 0, dout_read_ = 0xf;
  bool has_output_ = true;  // the block in progress writes DATA_OUT (not GCM's INIT, AAD, RESTORE)
  unsigned iv_written_ = 0, gcm_init_step_ = 0, gcm_blocks_ = 0;
  uint8_t gcm_h_[16] = {}, gcm_s_[16] = {}, gcm_y_[16] = {};
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CRYPTO_AES_H_
