// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_ENTROPY_SRC_H_
#define EG2_MODEL_IP_EG2_ENTROPY_SRC_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_entropy_common.h"

namespace eg2_model {

class Eg2EntropySrc : public Eg2IpModel {
 public:
  static constexpr uint32_t kStateIdle = 0x0f5, kStateBootPhaseDone = 0x08e,
                            kStateContHTRunning = 0x1a2;

  Eg2EntropySrc(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    module_enable_ = reg("MODULE_ENABLE");
    conf_ = reg("CONF");
    entropy_control_ = reg("ENTROPY_CONTROL");
    entropy_data_ = reg("ENTROPY_DATA");
    main_sm_state_ = reg("MAIN_SM_STATE");
    debug_status_ = reg("DEBUG_STATUS");
    health_test_windows_ = reg("HEALTH_TEST_WINDOWS");
    fw_ov_control_ = reg("FW_OV_CONTROL");
    fw_ov_sha3_start_ = reg("FW_OV_SHA3_START");
    fw_ov_rd_fifo_overflow_ = reg("FW_OV_RD_FIFO_OVERFLOW");
    fw_ov_rd_data_ = reg("FW_OV_RD_DATA");
    fw_ov_wr_data_ = reg("FW_OV_WR_DATA");
    observe_fifo_depth_ = reg("OBSERVE_FIFO_DEPTH");
    set_modelled(entropy_data_);
    set_modelled(main_sm_state_);
    set_modelled(debug_status_);
    set_modelled(fw_ov_rd_fifo_overflow_);
    set_modelled(fw_ov_rd_data_);
    set_modelled(observe_fifo_depth_);
    update();
  }

  bool enabled() const { return mubi4_true(value(module_enable_)); }
  bool fips() const { return mubi4_true(value(conf_)); }
  bool to_firmware() const {
    return mubi4_true(value(conf_) >> 20) && mubi4_true(value(entropy_control_));
  }

  // A 384-bit seed for csrng (NIST byte order). False while the source is off or routed to
  // firmware only.
  bool seed_for_csrng(uint8_t *seed, bool *fips_seed) {
    if (!enabled() || to_firmware()) return false;
    // Entropy source theory of operation, Firmware Override: after the conditioner is stopped, its
    // 384-bit result is routed onward as an entropy seed. Consume that published result before
    // asking the synthetic noise source for a new seed.
    // The words are the same esfinal output firmware reads, word 0 least significant, so the seed is
    // built as refill() takes it apart (nist_word).
    if (fw_words_.size() >= 12) {
      uint32_t words[12];
      for (unsigned i = 0; i < 12; ++i) {
        words[i] = fw_words_.front();
        fw_words_.pop_front();
      }
      nist_from_words(words, 12, seed, 48);
    } else {
      next_seed(seed);
    }
    *fips_seed = fips();
    return true;
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    if (c == Eg2ResetCause::kPor) counter_ = 0;
    startup_seed_ = true;
    fw_words_.clear();
    sha3_started_ = false;
    inserted_.clear();
    observe_words_.clear();
    observe_since_ = kObserveOff;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    // The observe FIFO fills from the time the block is enabled in observe mode (refill_observe).
    if (!enabled() || !observing()) {
      observe_since_ = kObserveOff;
    } else if (observe_since_ == kObserveOff) {
      observe_since_ = sys_.now_ps();
    }
    // Firmware override, insert (theory_of_operation.md "Firmware Override"): with FW_OV_CONTROL
    // in override and insert mode, FW_OV_WR_DATA words go into the SHA3 conditioner while
    // FW_OV_SHA3_START is MuBi4True; setting it MuBi4False finishes SHA3-384 and the 384-bit
    // result leaves as a seed would (also after a start and stop with nothing written).
    if (r == fw_ov_wr_data_ && inserting() && sha3_started_) {
      uint32_t w = value(fw_ov_wr_data_);
      for (int i = 0; i < 4; ++i) inserted_.push_back(uint8_t(w >> (8 * i)));
    }
    if (r == fw_ov_sha3_start_) {
      bool start = mubi4_true(value(fw_ov_sha3_start_));
      if (start && !sha3_started_) inserted_.clear();
      if (!start && sha3_started_) {
        digestpp::sha3 h(384);
        h.absorb(inserted_.data(), inserted_.size());
        uint8_t seed[48];
        h.digest(seed, 48);
        // Word i is digest bytes 4i to 4i+3, little-endian (the known answer of
        // entropy_src_kat_impl.c fixes the order).
        for (unsigned i = 0; i < 12; ++i) {
          fw_words_.push_back(uint32_t(seed[4 * i]) | uint32_t(seed[4 * i + 1]) << 8 |
                              uint32_t(seed[4 * i + 2]) << 16 | uint32_t(seed[4 * i + 3]) << 24);
        }
        inserted_.clear();
      }
      sha3_started_ = start;
    }
    if (r == module_enable_ && !enabled()) fw_words_.clear();
    if (r == module_enable_ && !enabled()) startup_seed_ = true;
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == entropy_data_) {
      refill();
      set_value(entropy_data_, fw_words_.empty() ? 0 : fw_words_.front());
    } else if (r == observe_fifo_depth_ || r == fw_ov_rd_data_) {
      refill_observe();
      set_value(observe_fifo_depth_, uint32_t(observe_words_.size()));
      set_value(fw_ov_rd_data_, observe_words_.empty() ? 0 : observe_words_.front());
    }
  }
  void reg_after_read(uint16_t r) override {
    if (r == entropy_data_ && !fw_words_.empty()) {
      fw_words_.pop_front();
      update();
    } else if (r == fw_ov_rd_data_ && !observe_words_.empty()) {
      // entropy_src registers.md, FW_OV_RD_DATA: "A read of this register pops and returns the
      // top of the observe FIFO." The next post-health-test word is made available on demand.
      observe_words_.pop_front();
      set_value(observe_fifo_depth_, uint32_t(observe_words_.size()));
    }
  }

 private:
  void next_seed(uint8_t *seed) {
    if (eg2_rng_t *src = eg2_noise_source()) {
      noise_seed(src, seed);
      return;
    }
    digestpp::shake256 h;
    const char tag[] = "EG2 model entropy_src";
    h.absorb(reinterpret_cast<const unsigned char *>(tag), sizeof tag - 1);
    uint8_t n[8];
    for (int i = 0; i < 8; ++i) n[i] = uint8_t(counter_ >> (8 * i));
    h.absorb(n, sizeof n);
    h.squeeze(seed, 48);
    ++counter_;
  }
  // A seed from the noise source (see eg2_noise_source).
  void noise_seed(eg2_rng_t *src, uint8_t *seed) {
    unsigned window = value(health_test_windows_) & 0xffff;
    unsigned symbols = fips() ? (startup_seed_ ? 2 : 1) * (window ? window : 512) : 96;
    startup_seed_ = false;
    std::vector<uint8_t> raw((symbols + 1) / 2, 0);
    for (unsigned i = 0; i < symbols; ++i) {
      int s = eg2_rng_next(src);
      if (s < 0) s = 0;  // a recorded stream that ended: zeros (eg2sim reports the end)
      raw[i / 2] |= uint8_t(s << (4 * (i & 1)));
    }
    if (fips()) {
      digestpp::sha3 h(384);
      h.absorb(raw.data(), raw.size());
      h.digest(seed, 48);
    } else {
      std::memcpy(seed, raw.data(), 48);
    }
  }
  bool inserting() const {
    uint32_t c = value(fw_ov_control_);
    return mubi4_true(c & 0xf) && mubi4_true((c >> 4) & 0xf);
  }
  bool observing() const { return mubi4_true(value(fw_ov_control_) & 0xf); }
  void refill_observe() {
    if (!enabled() || !observing()) return;
    // entropy_src programmers_guide.md, "Firmware Override - Observe": "firmware can observe
    // post-health test entropy bits (including entropy bits used for startup health testing)" by
    // reading FW_OV_RD_DATA. Eight 4-bit noise symbols make one FIFO word. The documents do not
    // state the nibble order: the first symbol goes in bits 3:0, the capture format's.
    // Rate: AST README "RNG": "The expected rng_b_o valid output rate is about 50KHz", one 4-bit
    // symbol per valid, so a word every 8 / 50 kHz = 160 us from the time the block is enabled in
    // observe mode. Capacity: the hjson's ObserveFifoDepth, 32 words, and programmers_guide.md
    // "Firmware Override - Extract & Insert": "The observe FIFO will collect 1 kBit of contiguous
    // entropy bits. Any entropy bits arriving after the observe FIFO is full are being
    // discarded" (FW_OV_RD_FIFO_OVERFLOW). With a noise source from the host (eg2sim --rng, the rng
    // suite's statistical captures) the host supplies the symbols as fast as firmware reads them:
    // a word whenever firmware finds the FIFO empty, the rate not modelled.
    if (eg2_noise_source()) {
      if (observe_words_.empty()) push_observe_word();
      return;
    }
    const uint64_t now = sys_.now_ps();
    if (observe_since_ == kObserveOff || now < observe_since_) observe_since_ = now;
    uint64_t n = (now - observe_since_) / kObserveWordPs;
    observe_since_ += n * kObserveWordPs;
    for (uint64_t i = 0; i < n; ++i) {
      if (observe_words_.size() >= kObserveFifoDepth) {
        set_value(fw_ov_rd_fifo_overflow_, 1);  // later words are discarded
        break;
      }
      push_observe_word();
    }
  }
  static constexpr uint64_t kObserveOff = ~0ull;
  static constexpr uint64_t kObserveWordPs = 160000000ull;  // 8 symbols at 50 kHz
  static constexpr size_t kObserveFifoDepth = 32;           // ObserveFifoDepth (1 kbit)
  uint64_t observe_since_ = kObserveOff;
  void push_observe_word() {
    uint32_t word = 0;
    if (eg2_rng_t *src = eg2_noise_source()) {
      for (unsigned i = 0; i < 8; ++i) {
        int s = eg2_rng_next(src);
        word |= uint32_t(s < 0 ? 0 : s) << (4 * i);
      }
    } else {
      uint8_t seed[48];
      next_seed(seed);
      std::memcpy(&word, seed, sizeof word);
    }
    observe_words_.push_back(word);
    set_value(observe_fifo_depth_, uint32_t(observe_words_.size()));
    set_intr("es_observe_fifo_ready");
  }
  void refill() {
    if (!enabled() || !to_firmware() || !fw_words_.empty() || inserting()) return;
    uint8_t seed[48];
    next_seed(seed);
    for (unsigned i = 0; i < 12; ++i) fw_words_.push_back(nist_word(seed, 48, i));
  }
  void update() {
    uint32_t st = !enabled() ? kStateIdle : fips() ? kStateContHTRunning : kStateBootPhaseDone;
    set_value(main_sm_state_, st);
    // DEBUG_STATUS: MAIN_SM_IDLE (16), MAIN_SM_BOOT_DONE (17).
    set_value(debug_status_, enabled() ? (1u << 17) : (1u << 16));
    refill();
    if (!fw_words_.empty()) set_intr("es_entropy_valid");
  }
  uint16_t module_enable_, conf_, entropy_control_, entropy_data_, main_sm_state_, debug_status_;
  uint16_t health_test_windows_, fw_ov_control_, fw_ov_sha3_start_, fw_ov_rd_fifo_overflow_,
      fw_ov_rd_data_, fw_ov_wr_data_, observe_fifo_depth_;
  bool sha3_started_ = false;
  std::vector<uint8_t> inserted_;
  bool startup_seed_ = true;
  uint64_t counter_ = 0;
  std::deque<uint32_t> fw_words_;
  std::deque<uint32_t> observe_words_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_ENTROPY_SRC_H_
