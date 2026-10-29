// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The entropy complex of the EG2 SystemC model (Phase B, crypto group): entropy_src, csrng, edn0
// and edn1. Modelled from the IPs' hjson and documentation (hw/ip/{entropy_src,csrng,edn}/doc).
//
// entropy_src  The noise source and its conditioning are replaced by a deterministic generator:
//              seed n is the first 384 bits of SHAKE256("EG2 model entropy_src" || n), n a 64-bit
//              little-endian counter that only a power-on reset clears. Each seed is distinct and a
//              run is reproducible. Health tests always pass. Routed to firmware
//              (CONF.ENTROPY_DATA_REG_ENABLE and ENTROPY_CONTROL.ES_ROUTE true) the seeds are read
//              through ENTROPY_DATA; otherwise they go to csrng. Firmware override: insert (FW_OV_WR_DATA
//              through the SHA3 conditioner); observe (FW_OV_RD_DATA) is not modelled.
// csrng        NIST SP 800-90A CTR_DRBG, AES-256, no derivation function, seedlen 384, as the
//              theory of operation specifies, with the block cipher from hw/ip/aes/model/aes.c.
//              Three instances: edn0 (0), edn1 (1), software (2). Byte order as in the programmer's
//              guide "Endianness and Known-Answer Tests": every 32-bit word carries the next less
//              significant part of the NIST byte string, word 0 the least significant.
// edn0, edn1   Boot-time, auto and software request modes. EDN_BUS_CMP_ALERT compares each
//              block with the previous one this EDN received since it was enabled
//              (edn_core.sv compares 64 bits; the model all 128). An endpoint (another model) asks for
//              32-bit words with Eg2Edn::get(); EDN serves them from the genbits its csrng instance
//              generates, and get() fails while EDN has nothing to give, as an endpoint waits in
//              hardware.
//
// Where the documentation is silent and the RTL or the DIF was read: the MAIN_SM_STATE encodings
// (dif_entropy_src.h; hw/ip/edn/rtl/edn_pkg.sv).

#ifndef EG2_MODEL_IP_EG2_CRYPTO_ENTROPY_H_
#define EG2_MODEL_IP_EG2_CRYPTO_ENTROPY_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "hw/ip/aes/model/aes.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"
#include "../../eg2_rng/eg2_rng_source.h"

namespace eg2_model {

// The noise source (`eg2sim --rng <source>`, ../../eg2_rng/eg2_rng_source.h); null: the
// deterministic generator below. With a source, entropy_src takes its seeds from the source's
// 4-bit symbols as the theory of operation describes: in FIPS mode SHA3-384 over
// HEALTH_TEST_WINDOWS.FIPS_WINDOW symbols (two windows for the first seed after the block is
// enabled, the startup seed), otherwise (boot-time mode) the 96 symbols of 384 raw bits. The
// symbols are packed in arrival order, the first in the least significant nibble of the first
// byte; the seed's bytes are taken as the NIST byte string csrng uses. The packing and the byte
// order the hardware uses are not documented; the chip bench's comparison settles them.
inline eg2_rng_t *&eg2_noise_source() {
  static eg2_rng_t *src = nullptr;
  return src;
}

inline bool mubi4_true(uint32_t v) { return (v & 0xf) == 0x6; }

// A NIST byte string of n bytes, B1 (most significant) first, as words least significant first.
inline void nist_from_words(const uint32_t *w, unsigned nwords, uint8_t *b, unsigned nbytes) {
  std::memset(b, 0, nbytes);
  for (unsigned i = 0; i < nwords && 4 * (i + 1) <= nbytes; ++i) {
    unsigned at = nbytes - 4 * (i + 1);
    b[at] = uint8_t(w[i] >> 24);
    b[at + 1] = uint8_t(w[i] >> 16);
    b[at + 2] = uint8_t(w[i] >> 8);
    b[at + 3] = uint8_t(w[i]);
  }
}
inline uint32_t nist_word(const uint8_t *b, unsigned nbytes, unsigned i) {
  unsigned at = nbytes - 4 * (i + 1);
  return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) |
         b[at + 3];
}

// ---------------------------------------------------------------------------------------------
// CTR_DRBG (SP 800-90A 10.2.1, AES-256, no derivation function).
struct Eg2CtrDrbg {
  static constexpr unsigned kSeedLen = 48;
  uint8_t key[32] = {};
  uint8_t v[16] = {};
  uint32_t reseed_counter = 0;
  bool instantiated = false;
  bool fips = false;

  static void inc(uint8_t *v) {
    for (int i = 15; i >= 0 && ++v[i] == 0; --i) {
    }
  }
  void block(uint8_t *out) {
    inc(v);
    aes_encrypt_block(v, key, 32, out);
  }
  void update(const uint8_t *provided) {
    uint8_t temp[kSeedLen];
    for (unsigned i = 0; i < kSeedLen; i += 16) block(temp + i);
    for (unsigned i = 0; i < kSeedLen; ++i) temp[i] ^= provided[i];
    std::memcpy(key, temp, 32);
    std::memcpy(v, temp + 32, 16);
  }
  void instantiate(const uint8_t *seed, bool fips_seed) {
    std::memset(key, 0, sizeof key);
    std::memset(v, 0, sizeof v);
    update(seed);
    reseed_counter = 0;
    instantiated = true;
    fips = fips_seed;
  }
  void reseed(const uint8_t *seed, bool fips_seed) {
    update(seed);
    reseed_counter = 0;
    fips = fips_seed;
  }
  void uninstantiate() { *this = Eg2CtrDrbg(); }
};

// ---------------------------------------------------------------------------------------------
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
    fw_ov_wr_data_ = reg("FW_OV_WR_DATA");
    set_modelled(entropy_data_);
    set_modelled(main_sm_state_);
    set_modelled(debug_status_);
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
    next_seed(seed);
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
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
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
    }
  }
  void reg_after_read(uint16_t r) override {
    if (r == entropy_data_ && !fw_words_.empty()) {
      fw_words_.pop_front();
      update();
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
  uint16_t health_test_windows_, fw_ov_control_, fw_ov_sha3_start_, fw_ov_wr_data_;
  bool sha3_started_ = false;
  std::vector<uint8_t> inserted_;
  bool startup_seed_ = true;
  uint64_t counter_ = 0;
  std::deque<uint32_t> fw_words_;
};

// ---------------------------------------------------------------------------------------------
// The csrng application command (theory of operation "Command Description").
struct Eg2CsrngCmd {
  enum Acmd { kInstantiate = 1, kReseed = 2, kGenerate = 3, kUpdate = 4, kUninstantiate = 5 };
  enum Sts {
    kSuccess = 0,
    kInvalidAcmd = 1,
    kInvalidGenCmd = 2,
    kInvalidCmdSeq = 3,
    kReseedCntExceeded = 4,
  };
  uint32_t header = 0;
  std::vector<uint32_t> data;
  unsigned acmd() const { return header & 0xf; }
  unsigned clen() const { return (header >> 4) & 0xf; }
  bool flag0() const { return mubi4_true(header >> 8); }
  unsigned glen() const { return (header >> 12) & 0xfff; }
  bool complete() const { return data.size() >= clen(); }
};

class Eg2Csrng : public Eg2IpModel {
 public:
  static constexpr unsigned kInstances = 3, kSwInstance = 2;

  Eg2Csrng(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    cmd_req_ = reg("CMD_REQ");
    sw_cmd_sts_ = reg("SW_CMD_STS");
    genbits_vld_ = reg("GENBITS_VLD");
    genbits_ = reg("GENBITS");
    int_state_num_ = reg("INT_STATE_NUM");
    int_state_val_ = reg("INT_STATE_VAL");
    int_state_read_enable_ = reg("INT_STATE_READ_ENABLE");
    for (uint16_t r : {sw_cmd_sts_, genbits_vld_, genbits_, int_state_val_}) set_modelled(r);
    for (unsigned i = 0; i < kInstances; ++i) {
      reseed_counter_[i] = reg(("RESEED_COUNTER_" + std::to_string(i)).c_str());
      set_modelled(reseed_counter_[i]);
    }
    recov_alert_sts_ = reg("RECOV_ALERT_STS");
    set_modelled(recov_alert_sts_);
    update();
  }

  bool enabled() const { return mubi4_true(value(ctrl_)); }

  // ---- The hardware application interfaces (edn0, edn1) ----
  // Run a command on instance inst; false while it waits for entropy (call again later).
  // For a generate, the blocks are produced one by one with next_block().
  bool run(unsigned inst, const Eg2CsrngCmd &cmd, unsigned *sts) {
    if (!enabled()) return false;
    return execute(inst, cmd, sts);
  }
  // The next 128-bit block of the generate in progress on inst (four words, least significant
  // first); false when none is in progress.
  bool next_block(unsigned inst, uint32_t *words) {
    Gen &g = gen_[inst];
    if (g.remaining == 0) return false;
    uint8_t out[16];
    drbg_[inst].block(out);
    for (unsigned i = 0; i < 4; ++i) words[i] = nist_word(out, 16, i);
    // CS_BUS_CMP_ALERT: the same block twice in a row on the software application port
    // (RECOV_ALERT_STS), forgotten when csrng is disabled. The hardware compares 64 bits of each
    // block (csrng_core.sv); the model compares all 128.
    if (inst == kSwInstance && have_last_ && std::equal(out, out + 16, last_out_)) {
      set_value(recov_alert_sts_, value(recov_alert_sts_) | (1u << 12));
      raise_alert("recov_alert");
    }
    if (inst == kSwInstance) std::copy(out, out + 16, last_out_), have_last_ = true;
    if (--g.remaining == 0) finish_generate(inst);
    return true;
  }
  unsigned remaining(unsigned inst) const { return gen_[inst].remaining; }
  bool instantiated(unsigned inst) const { return drbg_[inst].instantiated; }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    for (auto &d : drbg_) d.uninstantiate();
    for (auto &g : gen_) g = Gen();
    sw_cmd_ = Eg2CsrngCmd();
    sw_have_header_ = false;
    sw_pending_ = false;
    sw_words_.clear();
    sw_ack_ = false;
    sw_sts_ = 0;
    state_ptr_ = 0;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r == cmd_req_ && enabled()) {
      uint32_t w = wdata & wmask;
      if (!sw_have_header_) {
        sw_cmd_ = Eg2CsrngCmd();
        sw_cmd_.header = w;
        sw_have_header_ = true;
        sw_ack_ = false;
      } else {
        sw_cmd_.data.push_back(w);
      }
      if (sw_cmd_.complete()) {
        sw_have_header_ = false;
        sw_pending_ = true;
        try_sw();
      }
    } else if (r == int_state_num_) {
      state_ptr_ = 0;
    } else if (r == ctrl_ && !enabled()) {
      // Disabled, csrng drops every instance's working state and any command in progress.
      for (auto &d : drbg_) d.uninstantiate();
      have_last_ = false;
      for (auto &g : gen_) g = Gen();
      sw_have_header_ = sw_pending_ = sw_gen_running_ = false;
      sw_words_.clear();
    }
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == sw_cmd_sts_ || r == genbits_vld_) try_sw();
    if (r == genbits_) {
      fill_sw_words();
      bool ok = mubi4_true(value(ctrl_) >> 4) && !sw_words_.empty();
      set_value(genbits_, ok ? sw_words_.front() : 0);
    }
    if (r == int_state_val_) set_value(int_state_val_, int_state_word());
    update();
  }
  void reg_after_read(uint16_t r) override {
    if (r == genbits_ && mubi4_true(value(ctrl_) >> 4) && !sw_words_.empty()) {
      sw_words_.pop_front();
      if (sw_words_.empty() && gen_[kSwInstance].remaining == 0 && sw_gen_running_) {
        sw_gen_running_ = false;
        acknowledge(Eg2CsrngCmd::kSuccess);
      }
      update();
    } else if (r == int_state_val_) {
      ++state_ptr_;
    }
  }

 private:
  struct Gen {
    unsigned remaining = 0;
    std::vector<uint8_t> adata;  // the generate's additional input (NIST order), empty if none
  };

  // Seed material for instantiate / reseed (theory of operation, the flag0 / clen table).
  bool seed_material(const Eg2CsrngCmd &cmd, uint8_t *seed, bool *fips_seed) {
    uint8_t adata[48];
    nist_from_words(cmd.data.data(), unsigned(cmd.data.size()), adata, 48);
    if (cmd.flag0()) {
      std::memcpy(seed, adata, 48);  // clen 0: a seed of zero
      *fips_seed = false;
      return true;
    }
    // "cs_entropy_req asserts whenever the instance requests entropy from ENTROPY_SRC"
    // (programmer's guide).
    if (!entropy_requested_) {
      entropy_requested_ = true;
      set_intr("cs_entropy_req");
    }
    auto *es = dynamic_cast<Eg2EntropySrc *>(sys_.model("entropy_src"));
    if (!es || !es->seed_for_csrng(seed, fips_seed)) return false;
    entropy_requested_ = false;
    for (unsigned i = 0; i < 48; ++i) seed[i] ^= adata[i];
    return true;
  }

  bool execute(unsigned inst, const Eg2CsrngCmd &cmd, unsigned *sts) {
    Eg2CtrDrbg &d = drbg_[inst];
    *sts = Eg2CsrngCmd::kSuccess;
    switch (cmd.acmd()) {
      case Eg2CsrngCmd::kInstantiate:
      case Eg2CsrngCmd::kReseed: {
        bool inst_cmd = cmd.acmd() == Eg2CsrngCmd::kInstantiate;
        if (inst_cmd == d.instantiated) {
          *sts = Eg2CsrngCmd::kInvalidCmdSeq;
          return true;
        }
        uint8_t seed[48];
        bool fips_seed = false;
        if (!seed_material(cmd, seed, &fips_seed)) return false;
        if (inst_cmd) d.instantiate(seed, fips_seed);
        else d.reseed(seed, fips_seed);
        return true;
      }
      case Eg2CsrngCmd::kGenerate: {
        if (!d.instantiated) {
          *sts = Eg2CsrngCmd::kInvalidCmdSeq;
          return true;
        }
        if (cmd.glen() == 0) {
          *sts = Eg2CsrngCmd::kInvalidGenCmd;
          return true;
        }
        Gen &g = gen_[inst];
        g.adata.clear();
        if (cmd.clen()) {
          g.adata.resize(48);
          nist_from_words(cmd.data.data(), unsigned(cmd.data.size()), g.adata.data(), 48);
          d.update(g.adata.data());
        }
        g.remaining = cmd.glen();
        return true;
      }
      case Eg2CsrngCmd::kUpdate: {
        if (!d.instantiated) {
          *sts = Eg2CsrngCmd::kInvalidCmdSeq;
          return true;
        }
        uint8_t adata[48];
        nist_from_words(cmd.data.data(), unsigned(cmd.data.size()), adata, 48);
        d.update(adata);
        return true;
      }
      case Eg2CsrngCmd::kUninstantiate:
        d.uninstantiate();
        gen_[inst] = Gen();
        return true;
      default:
        *sts = Eg2CsrngCmd::kInvalidAcmd;
        return true;
    }
  }

  void finish_generate(unsigned inst) {
    Gen &g = gen_[inst];
    uint8_t zero[48] = {};
    drbg_[inst].update(g.adata.empty() ? zero : g.adata.data());
    ++drbg_[inst].reseed_counter;
  }

  void try_sw() {
    if (!sw_pending_ || !enabled()) return;
    unsigned sts;
    if (!execute(kSwInstance, sw_cmd_, &sts)) return;  // waits for entropy
    sw_pending_ = false;
    if (sw_cmd_.acmd() == Eg2CsrngCmd::kGenerate && sts == Eg2CsrngCmd::kSuccess) {
      sw_gen_running_ = true;  // acknowledged once software has read every block
      fill_sw_words();
    } else {
      acknowledge(sts);
    }
  }
  void fill_sw_words() {
    if (!sw_words_.empty() || gen_[kSwInstance].remaining == 0) return;
    uint32_t w[4];
    if (next_block(kSwInstance, w)) sw_words_.assign(w, w + 4);
  }
  void acknowledge(unsigned sts) {
    sw_ack_ = true;
    sw_sts_ = sts;
    set_intr("cs_cmd_req_done");
  }

  // INT_STATE_VAL (dif_csrng_get_internal_state): reseed counter, V (4 words), key (8 words),
  // flags (0 instantiated, 1 FIPS); zero unless CTRL.READ_INT_STATE and the instance's
  // INT_STATE_READ_ENABLE allow it.
  uint32_t int_state_word() const {
    unsigned inst = value(int_state_num_) & 0xf;
    if (inst >= kInstances || !mubi4_true(value(ctrl_) >> 8) ||
        !((value(int_state_read_enable_) >> inst) & 1)) {
      return 0;
    }
    const Eg2CtrDrbg &d = drbg_[inst];
    unsigned p = state_ptr_;
    if (p == 0) return d.reseed_counter;
    if (p <= 4) return nist_word(d.v, 16, p - 1);
    if (p <= 12) return nist_word(d.key, 32, p - 5);
    if (p == 13) return (d.instantiated ? 1u : 0u) | (d.fips ? 2u : 0u);
    return 0;
  }

  void update() {
    // SW_CMD_STS: CMD_RDY (1), CMD_ACK (2), CMD_STS (5:3).
    bool rdy = enabled() && !sw_pending_ && !sw_gen_running_;
    set_value(sw_cmd_sts_, (rdy ? 2u : 0u) | (sw_ack_ ? 4u : 0u) | ((sw_sts_ & 7) << 3));
    bool vld = !sw_words_.empty() || gen_[kSwInstance].remaining != 0;
    set_value(genbits_vld_, (vld ? 1u : 0u) | (drbg_[kSwInstance].fips ? 2u : 0u));
    for (unsigned i = 0; i < kInstances; ++i) set_value(reseed_counter_[i], drbg_[i].reseed_counter);
  }

  uint16_t ctrl_, cmd_req_, sw_cmd_sts_, genbits_vld_, genbits_, int_state_num_, int_state_val_,
      int_state_read_enable_, reseed_counter_[kInstances], recov_alert_sts_;
  uint8_t last_out_[16] = {};
  bool have_last_ = false;
  std::array<Eg2CtrDrbg, kInstances> drbg_;
  std::array<Gen, kInstances> gen_;
  Eg2CsrngCmd sw_cmd_;
  bool sw_have_header_ = false, sw_pending_ = false, sw_gen_running_ = false, sw_ack_ = false;
  unsigned sw_sts_ = 0;
  std::deque<uint32_t> sw_words_;
  unsigned state_ptr_ = 0;
  bool entropy_requested_ = false;
};

// ---------------------------------------------------------------------------------------------
class Eg2Edn : public Eg2IpModel {
 public:
  // MAIN_SM_STATE encodings (the documentation lists none; from hw/ip/edn/rtl/edn_pkg.sv).
  static constexpr uint32_t kStateIdle = 0x0c1, kStateSwPortMode = 0x095,
                            kStateBootDone = 0x0f0, kStateAutoAckWait = 0x092;

  Eg2Edn(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    inst_ = std::string(d.window) == "edn1" ? 1 : 0;
    ctrl_ = reg("CTRL");
    boot_ins_cmd_ = reg("BOOT_INS_CMD");
    boot_gen_cmd_ = reg("BOOT_GEN_CMD");
    sw_cmd_req_ = reg("SW_CMD_REQ");
    sw_cmd_sts_ = reg("SW_CMD_STS");
    reseed_cmd_ = reg("RESEED_CMD");
    generate_cmd_ = reg("GENERATE_CMD");
    max_reqs_ = reg("MAX_NUM_REQS_BETWEEN_RESEEDS");
    main_sm_state_ = reg("MAIN_SM_STATE");
    recov_alert_sts_ = reg("RECOV_ALERT_STS");
    set_modelled(recov_alert_sts_);
    set_modelled(sw_cmd_sts_);
    set_modelled(main_sm_state_);
    update();
  }

  bool enabled() const { return mubi4_true(value(ctrl_)); }
  bool boot_mode() const { return mubi4_true(value(ctrl_) >> 4); }
  bool auto_mode() const { return mubi4_true(value(ctrl_) >> 8); }

  // An endpoint's request for one 32-bit word of entropy; false while EDN has none to give.
  bool get(uint32_t *word) {
    if (!enabled()) return false;
    if (words_.empty() && !produce()) return false;
    *word = words_.front();
    words_.pop_front();
    return true;
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    words_.clear();
    sw_cmd_ = Eg2CsrngCmd();
    sw_have_header_ = sw_pending_ = sw_ack_ = false;
    sw_sts_ = 0;
    boot_state_ = 0;
    have_last_ = false;
    reseed_fifo_.clear();
    gen_fifo_.clear();
    reqs_since_reseed_ = 0;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == ctrl_) {
      // CMD_FIFO_RST (15:12) clears the auto mode's command FIFOs.
      if (mubi4_true(value(ctrl_) >> 12)) {
        reseed_fifo_.clear();
        gen_fifo_.clear();
      }
      bool was_boot = mubi4_true(old >> 4) && mubi4_true(old);
      if (was_boot && !(boot_mode() && enabled()) && boot_state_) {
        // Leaving boot-time request mode uninstantiates the boot instance.
        unsigned sts;
        Eg2CsrngCmd un;
        un.header = Eg2CsrngCmd::kUninstantiate;
        if (auto *cs = csrng()) cs->run(inst_, un, &sts);
        boot_state_ = 0;
      }
      if (!enabled()) {
        // Disabling stops EDN "immediately" (theory of operation): auto mode starts afresh, its
        // first reseed after MAX_NUM_REQS_BETWEEN_RESEEDS generates of the new configuration.
        words_.clear();
        boot_state_ = 0;
        have_last_ = false;
        reqs_since_reseed_ = 0;
      }
    } else if (r == sw_cmd_req_ && enabled()) {
      if (!sw_have_header_) {
        sw_cmd_ = Eg2CsrngCmd();
        sw_cmd_.header = w;
        sw_have_header_ = true;
        sw_ack_ = false;
      } else {
        sw_cmd_.data.push_back(w);
      }
      if (sw_cmd_.complete()) {
        sw_have_header_ = false;
        sw_pending_ = true;
        try_sw();
      }
    } else if (r == reseed_cmd_) {
      reseed_fifo_.push_back(w);
    } else if (r == generate_cmd_) {
      gen_fifo_.push_back(w);
    }
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == sw_cmd_sts_) try_sw();
    update();
  }

 private:
  Eg2Csrng *csrng() { return dynamic_cast<Eg2Csrng *>(sys_.model("csrng")); }

  // Fill the word queue from the csrng instance: the generate in progress, or a new one in
  // boot-time or auto request mode.
  bool produce() {
    Eg2Csrng *cs = csrng();
    if (!cs) return false;
    if (cs->remaining(inst_) == 0) {
      if (boot_mode()) {
        if (!boot_command()) return false;
      } else if (auto_mode()) {
        if (!auto_command()) return false;
      } else {
        return false;
      }
    }
    uint32_t blk[4];
    if (!cs->next_block(inst_, blk)) return false;
    // EDN_BUS_CMP_ALERT: the same genbits block twice in a row from csrng (RECOV_ALERT_STS).
    if (have_last_ && std::equal(blk, blk + 4, last_blk_)) {
      set_value(recov_alert_sts_, value(recov_alert_sts_) | (1u << 12));
      raise_alert("recov_alert");
    }
    std::copy(blk, blk + 4, last_blk_);
    have_last_ = true;
    words_.insert(words_.end(), blk, blk + 4);
    return true;
  }
  // Boot-time request mode: instantiate (BOOT_INS_CMD), then one generate (BOOT_GEN_CMD). A
  // further generate needs firmware to leave the mode.
  bool boot_command() {
    Eg2Csrng *cs = csrng();
    unsigned sts;
    if (boot_state_ == 0) {
      Eg2CsrngCmd c;
      c.header = value(boot_ins_cmd_);
      if (!cs->instantiated(inst_) && !cs->run(inst_, c, &sts)) return false;
      boot_state_ = 1;
    }
    if (boot_state_ == 1) {
      Eg2CsrngCmd c;
      c.header = value(boot_gen_cmd_);
      if (!cs->run(inst_, c, &sts)) return false;
      boot_state_ = 2;
      return sts == Eg2CsrngCmd::kSuccess;
    }
    return false;
  }
  // Auto request mode: the next GENERATE_CMD from its FIFO, and a RESEED_CMD after
  // MAX_NUM_REQS_BETWEEN_RESEEDS generates. Both FIFOs replay their commands.
  bool auto_command() {
    Eg2Csrng *cs = csrng();
    unsigned sts;
    if (gen_fifo_.empty()) return false;
    if (value(max_reqs_) && reqs_since_reseed_ >= value(max_reqs_) && !reseed_fifo_.empty()) {
      Eg2CsrngCmd c = parse(reseed_fifo_);
      if (!cs->run(inst_, c, &sts)) return false;
      reqs_since_reseed_ = 0;
    }
    Eg2CsrngCmd c = parse(gen_fifo_);
    if (!cs->run(inst_, c, &sts)) return false;
    ++reqs_since_reseed_;
    return sts == Eg2CsrngCmd::kSuccess;
  }
  static Eg2CsrngCmd parse(const std::vector<uint32_t> &fifo) {
    Eg2CsrngCmd c;
    c.header = fifo[0];
    for (unsigned i = 1; i < fifo.size() && c.data.size() < c.clen(); ++i) {
      c.data.push_back(fifo[i]);
    }
    return c;
  }

  void try_sw() {
    if (!sw_pending_) return;
    Eg2Csrng *cs = csrng();
    unsigned sts;
    if (!cs || !cs->run(inst_, sw_cmd_, &sts)) return;
    sw_pending_ = false;
    sw_ack_ = true;
    sw_sts_ = sts;
    set_intr("edn_cmd_req_done");
  }

  void update() {
    // SW_CMD_STS: CMD_REG_RDY (0), CMD_RDY (1), CMD_ACK (2), CMD_STS (5:3).
    bool rdy = enabled() && !boot_mode() && !sw_pending_;
    uint32_t st = (rdy ? 1u : 0u) | (rdy && !sw_have_header_ ? 2u : 0u) | (sw_ack_ ? 4u : 0u) |
                  ((sw_sts_ & 7) << 3);
    set_value(sw_cmd_sts_, st);
    uint32_t sm = !enabled() ? kStateIdle
                  : boot_mode() ? kStateBootDone
                  : auto_mode() ? kStateAutoAckWait
                                : kStateSwPortMode;
    set_value(main_sm_state_, sm);
  }

  unsigned inst_;
  uint16_t ctrl_, boot_ins_cmd_, boot_gen_cmd_, sw_cmd_req_, sw_cmd_sts_, reseed_cmd_,
      generate_cmd_, max_reqs_, main_sm_state_, recov_alert_sts_;
  uint32_t last_blk_[4] = {};
  bool have_last_ = false;
  std::deque<uint32_t> words_;
  Eg2CsrngCmd sw_cmd_;
  bool sw_have_header_ = false, sw_pending_ = false, sw_ack_ = false;
  unsigned sw_sts_ = 0;
  unsigned boot_state_ = 0;
  std::vector<uint32_t> reseed_fifo_, gen_fifo_;
  uint32_t reqs_since_reseed_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CRYPTO_ENTROPY_H_
