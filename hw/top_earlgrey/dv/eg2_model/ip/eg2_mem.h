// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Memory controllers, OTP and life cycle of the EG2 SystemC model (Phase B group: memories).
// Registered through EG2_MODELS_MEM, at the end of this file. otp_ctrl and lc_ctrl are in
// ip/eg2_mem_otp.h.

#ifndef EG2_MODEL_IP_EG2_MEM_H_
#define EG2_MODEL_IP_EG2_MEM_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_core_system.h"
#include "eg2_mem_otp.h"

namespace eg2_model {


// rram_ctrl.core (hw/ip/rram_ctrl: README "Address Map", doc/programmer's guide, theory of
// operation "Memory Protection", "RRAM Escalation & Disable").
//
// The data partition is the rram_ctrl.host memory (2 MiB, the host's view); the information
// partition is 8 pages of 512 bytes the model keeps, unchanged by resets. The PHY reports its
// initialisation done from power-up (PHY_STATUS.init_done). INIT completes the controller's
// initialisation at once: STATUS.init_done and keys_valid.
//
// Controller operations (CONTROL.START with OP Read, Write or Rewrite; PARTITION; NUM + 1 bus
// words; ADDR): before STATUS.init_done, with an undefined OP, or with RRAM disabled (DIS.SW_DIS
// not MuBi4False: "all host and controller requests are blocked"; the doc names no error code, the
// model reports op_err) the operation ends at once with OP_STATUS.err and ERR_CODE.op_err. Memory
// protection per bus word: the data partition's top six pages (OTP integrity and OTP, from page
// 4090) are never reachable; otherwise the lowest-numbered enabled MP_REGION that holds the page
// decides, DEFAULT_REGION if none; in the information partition INFO_PAGE_CFG of the page, and for
// pages 5, 6 and 7 the life-cycle signals as well (creator seed, owner seed, isolated partition
// read / write). An attribute allows only when it is MuBi4True. A denied word sets ERR_CODE.mp_err,
// ERR_ADDR (first error) and OP_STATUS.err; a read returns 0xFFFFFFFF from that word on, a write
// drops it. Read: the words go into the read FIFO; a write: CONTROL.START clears the write FIFO,
// and each word software pushes to wr_fifo is programmed at the next address, CTRL_REGWEN low until
// the last. Rewrite: one RRAM word, checked for read and write, contents unchanged. A finished
// operation sets OP_STATUS.done and op_done, a failed one also the recov_err alert. FIFO status (STATUS, CURR_FIFO_LVL) and the
// wr_empty / wr_lvl / rd_full / rd_lvl status interrupts follow the FIFOs (16 words each).
//
// A read of rd_fifo pops the oldest word (window-read hook); reading the empty FIFO returns 0. ECC,
// scrambling and their errors, the emulated info windows (treated as absent), the RMA wipe, the
// OTP plug (otp_ctrl keeps the OTP array), host-read memory protection and EXEC (the host window
// is a DMI memory; exec_enabled() answers for whoever needs it), local escalation.
class Eg2RramCtrl : public Eg2IpModel {
 public:
  static constexpr unsigned kFifoDepth = 16, kPageBytes = 512, kInfoPages = 8;
  static constexpr uint32_t kOtpFirstPage = 4090;
  static constexpr unsigned kCreatorInfoPage = 5, kOwnerInfoPage = 6;

  Eg2RramCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), info_(kInfoPages * kPageBytes, 0) {
    dis_ = reg("DIS");
    exec_ = reg("EXEC");
    init_ = reg("INIT");
    ctrl_regwen_ = reg("CTRL_REGWEN");
    control_ = reg("CONTROL");
    addr_ = reg("ADDR");
    mp_cfg_ = reg("MP_REGION_CFG_0");
    mp_region_ = reg("MP_REGION_0");
    default_ = reg("DEFAULT_REGION");
    info_cfg_ = reg("INFO_PAGE_CFG_0");
    op_status_ = reg("OP_STATUS");
    status_ = reg("STATUS");
    err_code_ = reg("ERR_CODE");
    err_addr_ = reg("ERR_ADDR");
    phy_ = reg("PHY_STATUS");
    fifo_clr_ = reg("FIFO_CLR");
    fifo_lvl_ = reg("FIFO_LVL");
    curr_lvl_ = reg("CURR_FIFO_LVL");
    rd_win_ = win_index("rd_fifo");
    wr_win_ = win_index("wr_fifo");
    for (uint16_t r : {status_, phy_, curr_lvl_, err_addr_, ctrl_regwen_}) set_modelled(r);
    set_modelled(op_status_);
    // Fetches from the host window need EXEC (programmers_guide.md: "To enable code execution from
    // RRAM, software must write the magic value 0xa26a38f7 to the EXEC register. Any other value
    // disables instruction fetches"); a refused fetch is an instruction access fault.
    sys_.add_exec_region("rram_ctrl.host", [this] { return exec_enabled(); });
    power_up();
  }

  bool exec_enabled() const { return value(exec_) == 0xa26a38f7u; }

  // The seeds INIT forwards to keymgr_dpe (top: rram_ctrl.keymgr_creator_seed / keymgr_owner_seed;
  // theory_of_operation.md "StReadSeeds": the creator-seed and owner-seed info pages, 256 bits each,
  // read only if lc_seed_hw_rd_en is set, "the seeds are not read and remain invalid" otherwise).
  // The seed is the page's first 32 bytes; the double read and its seed_err are not modelled.
  bool keymgr_seed(bool owner, std::vector<uint8_t> *seed) const {
    *seed = owner ? owner_seed_ : creator_seed_;
    return owner ? owner_seed_valid_ : creator_seed_valid_;
  }
  std::vector<uint8_t> &info() { return info_; }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    rd_.clear();
    wr_left_ = 0;
    power_up();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == init_ && (value(init_) & 1)) {
      set_value(status_, value(status_) | (1u << 4) | (1u << 5));
      read_seeds();
    } else if (r == control_ && (w & 1) && (value(ctrl_regwen_) & 1)) {
      start();
    } else if (r == fifo_clr_) {
      if (w & 1) wr_left_ = 0;
      if (w & 2) rd_.clear();
      fifo_update();
    } else if (r == fifo_lvl_) {
      fifo_update();
    }
  }

  // rd_fifo: each bus read pops one word (programmers_guide.md: "The software drains the FIFO by
  // reading from rd_fifo").
  bool win_read(uint16_t win, uint32_t off, uint8_t *data, unsigned len) override {
    if (int(win) != rd_win_) return false;
    uint32_t v = 0;
    if (!rd_.empty()) {
      v = rd_.front();
      rd_.pop_front();
    }
    for (unsigned i = 0; i < len && i < 4; ++i) data[i] = uint8_t(v >> (8 * ((off + i) & 3)));
    fifo_update();
    return true;
  }

  void win_write(uint16_t win, uint32_t, unsigned) override {
    if (int(win) != wr_win_ || !wr_left_) return;
    const std::vector<uint8_t> &m = win_mem(win);
    uint32_t v = uint32_t(m[0]) | (uint32_t(m[1]) << 8) | (uint32_t(m[2]) << 16) |
                 (uint32_t(m[3]) << 24);
    if (allowed(wr_addr_, false)) {
      std::vector<uint8_t> *mem = memory(wr_part_);
      if (mem && wr_addr_ + 4 <= mem->size()) {
        for (int i = 0; i < 4; ++i) (*mem)[wr_addr_ + i] = uint8_t(v >> (8 * i));
      }
    } else {
      mp_error(wr_addr_);
    }
    wr_addr_ += 4;
    if (--wr_left_ == 0) finish();
    fifo_update();
  }

 private:
  void read_seeds() {
    Eg2LcCtrl *l = lc();
    bool rd = l && l->seed_hw_rd_en();
    creator_seed_.assign(info_.begin() + kCreatorInfoPage * kPageBytes,
                         info_.begin() + kCreatorInfoPage * kPageBytes + 32);
    owner_seed_.assign(info_.begin() + kOwnerInfoPage * kPageBytes,
                       info_.begin() + kOwnerInfoPage * kPageBytes + 32);
    creator_seed_valid_ = owner_seed_valid_ = rd;
  }

  void power_up() {
    creator_seed_valid_ = owner_seed_valid_ = false;
    set_value(phy_, value(phy_) | (1u << 1));
    set_value(ctrl_regwen_, 1);
    fifo_update();
  }

  std::vector<uint8_t> *memory(unsigned part) {
    if (part == 1) return &info_;
    auto it = sys_.memories.find("rram_ctrl.host");
    return it == sys_.memories.end() ? nullptr : &it->second->data();
  }

  Eg2LcCtrl *lc() const { return dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs")); }

  // Memory protection for one bus word of a controller operation.
  bool allowed(uint32_t a, bool read) const {
    uint32_t page = a / kPageBytes;
    unsigned sh = read ? 4 : 8;  // RD_EN at 7:4, WR_EN at 11:8 (INFO_PAGE_CFG, MP_REGION_CFG)
    if (part_ == 1) {
      if (page >= kInfoPages) return false;
      uint32_t cfg = value(uint16_t(info_cfg_ + page));
      if ((cfg & 0xf) != kMuBi4True || ((cfg >> sh) & 0xf) != kMuBi4True) return false;
      Eg2LcCtrl *l = lc();
      if (l && page == kCreatorInfoPage) return l->creator_seed_sw_rw_en();
      if (l && page == kOwnerInfoPage) return l->owner_seed_sw_rw_en();
      if (l && page == 7) return read ? l->iso_part_sw_rd_en() : l->iso_part_sw_wr_en();
      return true;
    }
    if (page >= kOtpFirstPage) return false;
    for (uint16_t i = 0; i < 10; ++i) {
      uint32_t cfg = value(uint16_t(mp_cfg_ + i));
      if ((cfg & 0xf) != kMuBi4True) continue;
      uint32_t rg = value(uint16_t(mp_region_ + i));
      uint32_t b = rg & 0xfff, sz = (rg >> 12) & 0xfff;
      if (page >= b && page <= b + sz) return ((cfg >> sh) & 0xf) == kMuBi4True;
    }
    uint32_t dflt = value(default_);  // RD_EN at 3:0, WR_EN at 7:4
    return ((dflt >> (read ? 0 : 4)) & 0xf) == kMuBi4True;
  }

  void mp_error(uint32_t a) {
    if (!(value(err_code_) & 0xf)) set_value(err_addr_, a);
    set_value(err_code_, value(err_code_) | 2u);
    op_err_ = true;
  }

  void start() {
    uint32_t c = value(control_);
    unsigned op = (c >> 4) & 3, num = ((c >> 16) & 0x3ff) + 1;
    part_ = (c >> 8) & 1;
    op_err_ = false;
    bool disabled = (value(dis_) & 0xf) != kMuBi4False;
    if (!(value(status_) & (1u << 4)) || op == 3 || disabled) {
      set_value(err_code_, value(err_code_) | 1u);
      op_err_ = true;
      finish();
      return;
    }
    uint32_t a = value(addr_);
    std::vector<uint8_t> *mem = memory(part_);
    if (op == 0) {  // Read
      a &= ~3u;
      bool failed = false;
      for (unsigned i = 0; i < num; ++i, a += 4) {
        uint32_t v = 0xffffffffu;
        if (!failed && allowed(a, true) && mem && a + 4 <= mem->size()) {
          v = uint32_t((*mem)[a]) | (uint32_t((*mem)[a + 1]) << 8) |
              (uint32_t((*mem)[a + 2]) << 16) | (uint32_t((*mem)[a + 3]) << 24);
        } else if (!failed) {
          mp_error(a);
          failed = true;
        }
        rd_.push_back(v);
      }
      finish();
    } else if (op == 1) {  // Write
      wr_addr_ = a & ~0xfu;
      wr_part_ = part_;
      wr_left_ = num;
      set_value(ctrl_regwen_, 0);
    } else {  // Rewrite
      a &= ~0xfu;
      if (!allowed(a, true) || !allowed(a, false)) mp_error(a);
      finish();
    }
    fifo_update();
  }

  void finish() {
    wr_left_ = 0;
    set_value(ctrl_regwen_, 1);
    set_value(op_status_, value(op_status_) | 1u | (op_err_ ? 2u : 0u));
    set_intr("op_done");
    // A failed controller operation raises the recoverable alert (rram_ctrl_lc_rw_en_test: "a
    // permission-denied access completes with mp_err / wr_err set, which raises recov_err").
    if (op_err_) raise_alert("recov_err");
  }

  void fifo_update() {
    unsigned rd = unsigned(std::min<size_t>(rd_.size(), kFifoDepth));
    unsigned wr = 0;  // the model programs each pushed word at once
    uint32_t st = value(status_) & 0x30;
    if (rd == kFifoDepth) st |= 1u << 0;
    if (rd == 0) st |= 1u << 1;
    st |= 1u << 3;  // wr_empty
    set_value(status_, st);
    set_value(curr_lvl_, (rd << 8) | wr);
    uint32_t lvl = value(fifo_lvl_);
    set_intr_level("wr_empty", true);
    set_intr_level("wr_lvl", wr <= (lvl & 0x1f));
    set_intr_level("rd_full", rd == kFifoDepth);
    set_intr_level("rd_lvl", rd >= ((lvl >> 8) & 0x1f));
  }

  uint16_t dis_, exec_, init_, ctrl_regwen_, control_, addr_, mp_cfg_, mp_region_, default_,
      info_cfg_, op_status_, status_, err_code_, err_addr_, phy_, fifo_clr_, fifo_lvl_, curr_lvl_;
  int rd_win_, wr_win_;
  std::vector<uint8_t> info_;
  std::vector<uint8_t> creator_seed_ = std::vector<uint8_t>(32, 0),
                       owner_seed_ = std::vector<uint8_t>(32, 0);
  bool creator_seed_valid_ = false, owner_seed_valid_ = false;
  std::deque<uint32_t> rd_;
  unsigned part_ = 0, wr_part_ = 0, wr_left_ = 0;
  uint32_t wr_addr_ = 0;
  bool op_err_ = false;
};

// sram_ctrl (hw/ip/sram_ctrl: doc/theory_of_operation.md, programmer's guide, data/sram_ctrl.hjson).
// CTRL.RENEW_SCR_KEY obtains a new key from OTP at once: STATUS.SCR_KEY_VALID, SCR_KEY_SEED_VALID
// while OTP's SECRET1 is locked ("if the key seeds have not yet been provisioned in OTP, the keys
// are derived from all-zero constants, and seed_valid is 0"), and SCR_KEY_ROTATED becomes
// MuBi4True (software clears it to MuBi4False by writing MuBi4True). CTRL.INIT overwrites the
// whole RAM ("LFSR Initialization Feature") and sets STATUS.INIT_DONE: with zeros where the instance
// is built with SecZeroInit = 1 (only sram_ctrl_meta in top_earlgrey.gen.hjson, so CHERIoT mode
// starts with no tag), with pseudo-random data otherwise. A key renewal without INIT turns the old
// contents into garbage ("a key renewal makes old contents unreadable"). Code execution from the RAM
// (exec_enabled(), for whoever needs it; the RAM itself is a DMI memory): only on the instances built
// with InstrExec = 1 (main and sec), then as the doc's table: EXEC.EN == MuBi4True if OTP's
// EN_SRAM_IFETCH is MuBi8True, the life cycle's HW_DEBUG_EN otherwise. Not modelled: scrambling
// itself, the integrity errors garbage data gives, readback checks, escalation.
class Eg2SramCtrl : public Eg2IpModel {
 public:
  Eg2SramCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    status_ = reg("STATUS");
    exec_ = reg("EXEC");
    rotated_ = reg("SCR_KEY_ROTATED");
    set_modelled(status_);
    std::string w = d.window;
    inst_ = w.substr(0, w.find('.'));
    ram_ = inst_ + ".ram";
    zero_init_ = inst_ == "sram_ctrl_meta";
    instr_exec_ = inst_ == "sram_ctrl_main" || inst_ == "sram_ctrl_sec";
    // Fetches only while execution is enabled (theory_of_operation.md "Code Execution from SRAM",
    // the table). The doc does not say how a refused fetch answers; the RTL's TL-UL adapter answers
    // it with an error (sram_ctrl.sv, en_ifetch), an instruction access fault on the core.
    sys_.add_exec_region(ram_, [this] { return exec_enabled(); });
  }

  bool exec_enabled() const {
    if (!instr_exec_) return false;
    auto *o = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    if (o && o->en_sram_ifetch()) return (value(exec_) & 0xf) == kMuBi4True;
    auto *l = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
    return l && l->hw_debug_en();
  }

  // Out of reset the controller uses its predefined key again, so data written under a renewed key
  // reads as garbage. The retention SRAM's controller keeps its key through a low-power exit
  // (sram_ctrl_sleep_sram_ret_contents_scramble_test: preserved after deep sleep, garbage after a
  // watchdog reset); the model's always-on rule keeps its registers except at power-on.
  void on_reset(Eg2ResetCause c) override {
    bool ret = inst_ == "sram_ctrl_ret";
    if (keyed_ && !(ret && c == Eg2ResetCause::kLowPowerExit)) {
      garble();
      keyed_ = false;
      if (ret) {
        set_value(status_, value(status_) & ~((1u << 3) | (1u << 4)));
        set_value(rotated_, kMuBi4False);
      }
    }
    Eg2IpModel::on_reset(c);
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == rotated_) {
      // rw1c of a MuBi4: writing MuBi4True clears it to MuBi4False.
      if ((w & 0xf) == kMuBi4True) set_value(rotated_, kMuBi4False);
      return;
    }
    if (r != ctrl_) return;
    if (w & 1) {
      auto *o = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
      bool seed = o && o->sram_key_seed_valid();
      set_value(status_, (value(status_) & ~(1u << 4)) | (1u << 3) | (seed ? (1u << 4) : 0));
      set_value(rotated_, kMuBi4True);
      // "A key renewal makes old contents unreadable": the old data reads back as garbage (the
      // chip's integrity errors on such reads are not modelled).
      if (!(w & 2)) garble();
      keyed_ = true;
    }
    if (w & 2) {
      auto it = sys_.memories.find(ram_);
      if (it != sys_.memories.end()) {
        for (uint8_t &b : it->second->data()) {
          b = zero_init_ ? 0 : next_byte();
        }
      }
      set_value(status_, value(status_) | (1u << 5));
    }
  }

 private:
  void garble() {
    auto it = sys_.memories.find(ram_);
    if (it == sys_.memories.end()) return;
    for (uint8_t &b : it->second->data()) b ^= next_byte();
  }
  uint8_t next_byte() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    return uint8_t(lfsr_ | 1);  // never 0: every byte changes
  }

  uint16_t ctrl_, status_, exec_, rotated_;
  std::string inst_, ram_;
  bool zero_init_, instr_exec_, keyed_ = false;
  uint32_t lfsr_ = 0x1234567;
};

// rom_ctrl (hw/ip/rom_ctrl: doc/theory_of_operation.md). The checker's expected digest is the top
// eight words of the ROM (EXP_DIGEST_*). The digest the chip computes (cSHAKE256 "ROM_CTRL" through
// KMAC over the scrambled ROM words with their ECC) is not modelled: DIGEST_* equal EXP_DIGEST_*,
// as on a good ROM, so the check passes. FATAL_ALERT_CAUSE stays 0.
class Eg2RomCtrl : public Eg2IpModel {
 public:
  Eg2RomCtrl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    digest_ = reg("DIGEST_0");
    exp_ = reg("EXP_DIGEST_0");
    for (uint16_t i = 0; i < 8; ++i) {
      set_modelled(uint16_t(digest_ + i));
      set_modelled(uint16_t(exp_ + i));
    }
  }
  void start_of_simulation() override { check(); }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    check();
  }

 private:
  void check() {
    auto it = sys_.memories.find("rom_ctrl.rom");
    if (it == sys_.memories.end()) return;
    std::vector<uint8_t> &m = it->second->data();
    size_t top = m.size() - 32;
    for (uint16_t i = 0; i < 8; ++i) {
      uint32_t v = uint32_t(m[top + 4 * i]) | (uint32_t(m[top + 4 * i + 1]) << 8) |
                   (uint32_t(m[top + 4 * i + 2]) << 16) | (uint32_t(m[top + 4 * i + 3]) << 24);
      set_value(uint16_t(exp_ + i), v);
      set_value(uint16_t(digest_ + i), v);
    }
  }
  uint16_t digest_, exp_;
};

// The base of a CHERIoT capability from its two words (CHERIoT ISA v1.0, capability encoding;
// CHERIoT-Sail getCapBoundsBits): metadata R 31, p 30:25, otype 24:22, E 21:18 (15 meaning 24),
// T 17:9, B 8:0; base = ((a >> (E + 9)) + c_b) << (E + 9) | B << E, c_b = -1 if the address's
// middle bits a[E+8:E] are below B. Truncated to 32 bits.
inline uint32_t cheriot_cap_base(uint32_t addr, uint32_t meta) {
  unsigned ce = (meta >> 18) & 0xf, e = ce == 15 ? 24 : ce;
  uint32_t b = meta & 0x1ff, mid = (addr >> e) & 0x1ff;
  uint64_t top = uint64_t(addr) >> (e + 9);
  if (mid < b) top -= 1;
  return uint32_t((top << (e + 9)) | (uint64_t(b) << e));
}
// A sealing capability (permit seal, unseal or user 0: compressed permissions p[4:3] == 0 with any
// of p[2:0]), which the load barrier and the revocation engine never revoke.
inline bool cheriot_cap_sealing(uint32_t meta) {
  uint32_t p = (meta >> 25) & 0x3f;
  return ((p >> 3) & 3) == 0 && (p & 7) != 0;
}

// What the subsystem's sweep needs of the top: its meta SRAM and the regions it covers
// (cheriot_mem_sys.sv's address-map parameters). Given with configure(), the TBRE sweeps; without
// it the model is the stand-in described below.
struct Eg2TbreLayout {
  struct Region {
    uint32_t base, top;          // [base, top): MainSram or Nvm
    uint32_t tag_offset;         // its capability tags in the meta SRAM
    std::vector<uint8_t> *data;  // the memory's bytes
  };
  std::vector<uint8_t> *meta = nullptr;  // the meta SRAM: the revocation bitmap at offset 0
  uint32_t heap_base = 0;                // MainSramBaseAddr: bitmap bit b covers heap_base + 8b
  uint32_t revbm_bytes = 0;              // MemSizeRevbm
  std::vector<Region> regions;
  std::function<bool()> cheriot;         // cheriot_ena_i is MuBi4True
  Eg2System::Clk clk = Eg2System::Clk::kMain;
  unsigned cycles_per_cap = 2;           // two bus words per capability, one per cycle
};

// cheriot_mem_sys.regs (hw/ip/cheriot_mem_sys/data/cheriot_mem_sys.hjson): the revocation engine's
// registers. TBRE_START is ignored, setting TBRE_STATUS.start_err, outside CHERIoT mode (the mode
// rv_core_ibex.cfg locked), with TBRE_NUM_CAPS zero, or with TBRE_BASE_ADDR outside the tagged
// regions (main and sec SRAM, the NVM). Otherwise the sweep runs: TBRE_REGWEN low, TBRE_STATUS.busy,
// TBRE_EPOCH odd, for two bus words per capability (one 100 MHz cycle each) up to the top of the
// region; then the epoch counts the sweep and tbre_done is raised. Functional stand-in only: no
// capability is revoked, because the model has no tags yet (M4), and sweep errors are not modelled.
//
// With a layout (configure; the RTOS test SoC board, rtos_soc/) the sweep is the engine's
// (doc/theory_of_operation.md "Revocation Engine"): the regions, the mode and the clock are the
// layout's; it sweeps min(TBRE_NUM_CAPS, (top - TBRE_BASE_ADDR) / 8) capabilities, in address
// order, cycles_per_cap clock cycles each (the doc gives no rate: "up to two words are in flight",
// so at best one word a cycle), and for each one that is tagged, not a sealing capability, and whose
// base has its revocation bit set clears its tag in the meta SRAM, never the data. A base outside
// the bitmap (below heap_base, or past MemSizeRevbm) is not revoked (the doc is silent; the RTL's
// TRVK filter, ibex_trvk.sv revbm_out_of_range, skips the lookup). Each capability is read and
// cleared at one instant, so the doc's watch of core writes in flight has nothing to do. Errors
// (sweep_err, the fatal alert) are not modelled: the model's memories answer every read.
class Eg2CheriotMemSys : public Eg2IpModel {
 public:
  Eg2CheriotMemSys(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    regwen_ = reg("TBRE_REGWEN");
    base_addr_ = reg("TBRE_BASE_ADDR");
    num_ = reg("TBRE_NUM_CAPS");
    start_ = reg("TBRE_START");
    status_ = reg("TBRE_STATUS");
    epoch_ = reg("TBRE_EPOCH");
    for (uint16_t r : {regwen_, status_, epoch_}) set_modelled(r);
  }

  void configure(const Eg2TbreLayout &l) {
    layout_ = l;
    swept_ = true;
  }
  // Capabilities whose tag a sweep cleared, since the start of the run.
  uint64_t revoked() const { return revoked_; }
  // Break the engine, to show a check can fail: it sweeps but clears no tag (the RTOS test SoC's
  // tbre_no_inval mutation).
  bool no_invalidate = false;

  void on_time(uint64_t now) override {
    if (!(value(status_) & 1)) return;
    if (swept_) {
      sweep_to(now);
      if (left_) {
        schedule_next();
        return;
      }
    }
    set_value(status_, value(status_) & ~1u);
    set_value(regwen_, 1);
    set_value(epoch_, (value(epoch_) & ~1u) + 2);
    set_intr("tbre_done");
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    left_ = 0;
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r != start_ || !(wdata & wmask & 1) || !(value(regwen_) & 1)) return;
    uint32_t a = value(base_addr_) & ~7u, n = value(num_);
    uint32_t top = region_top(a);
    bool cheriot;
    if (swept_) {
      cheriot = layout_.cheriot && layout_.cheriot();
    } else {
      auto *cfg = dynamic_cast<Eg2RvCoreIbexCfg *>(sys_.model("rv_core_ibex.cfg"));
      cheriot = cfg && cfg->cheriot();
    }
    if (!cheriot || n == 0 || !top) {
      set_value(status_, value(status_) | (1u << 8));
      return;
    }
    uint64_t caps = std::min<uint64_t>(n, (top - a) / 8);
    set_value(regwen_, 0);
    set_value(status_, value(status_) | 1u);
    set_value(epoch_, value(epoch_) | 1u);
    if (!swept_) {
      sys_.schedule(this, sys_.now_ps() + caps * 2 * 10000ull);
      return;
    }
    next_ = a;
    left_ = caps;
    done_ = 0;
    cycle0_ = sys_.clock_cycles(layout_.clk, sys_.now_ps());
    schedule_next();
  }

 private:
  // The top (exclusive) of the tagged region holding a, 0 outside every one.
  uint32_t region_top(uint32_t a) const {
    if (swept_) {
      for (const Eg2TbreLayout::Region &r : layout_.regions) {
        if (a >= r.base && a < r.top) return r.top;
      }
      return 0;
    }
    uint32_t lo, hi;
    if (span("sram_ctrl_main.ram", lo, hi) && a >= lo) {
      uint32_t lo2, hi2;
      if (span("sram_ctrl_sec.ram", lo2, hi2) && lo2 == hi) hi = hi2;
      if (a < hi) return hi;
    }
    if (span("rram_ctrl.host", lo, hi) && a >= lo && a < hi) return hi;
    return 0;
  }
  bool span(const char *mem, uint32_t &lo, uint32_t &hi) const {
    auto it = sys_.memories.find(mem);
    if (it == sys_.memories.end()) return false;
    lo = it->second->base();
    hi = lo + it->second->size();
    return true;
  }

  // The next event: when the next few capabilities are done (a handful at a time, so the core
  // runs between them as it does beside the engine).
  static constexpr uint64_t kCapsPerEvent = 16;
  void schedule_next() {
    uint64_t n = done_ + std::min<uint64_t>(left_, kCapsPerEvent);
    sys_.schedule(this, sys_.clock_time_of_cycle(layout_.clk, cycle0_ + n * layout_.cycles_per_cap));
  }
  // Every capability whose turn has come by now.
  void sweep_to(uint64_t now) {
    uint64_t due = (sys_.clock_cycles(layout_.clk, now) - cycle0_) / layout_.cycles_per_cap;
    while (left_ && done_ < due) {
      resolve(next_);
      next_ += 8;
      --left_;
      ++done_;
    }
  }
  void resolve(uint32_t a) {
    for (const Eg2TbreLayout::Region &r : layout_.regions) {
      if (a < r.base || a >= r.top) continue;
      uint32_t g = (a - r.base) / 8;
      uint8_t &tags = (*layout_.meta)[r.tag_offset + g / 8];
      if (!((tags >> (g % 8)) & 1)) return;
      const std::vector<uint8_t> &m = *r.data;
      uint32_t off = a - r.base, w0 = 0, w1 = 0;
      for (int i = 0; i < 4; ++i) {
        w0 |= uint32_t(m[off + i]) << (8 * i);
        w1 |= uint32_t(m[off + 4 + i]) << (8 * i);
      }
      if (cheriot_cap_sealing(w1)) return;
      uint32_t base = cheriot_cap_base(w0, w1);
      if (base < layout_.heap_base) return;
      uint32_t bit = (base - layout_.heap_base) / 8;
      if (bit / 8 >= layout_.revbm_bytes) return;
      if (!(((*layout_.meta)[bit / 8] >> (bit % 8)) & 1)) return;
      if (no_invalidate) return;
      tags = uint8_t(tags & ~(1u << (g % 8)));
      ++revoked_;
      return;
    }
  }

  uint16_t regwen_, base_addr_, num_, start_, status_, epoch_;
  Eg2TbreLayout layout_;
  bool swept_ = false;
  uint32_t next_ = 0;
  uint64_t left_ = 0, done_ = 0, cycle0_ = 0, revoked_ = 0;
};

}  // namespace eg2_model

// This group's entries of kModels (eg2_ip_models.cc): {window name or IP type, factory}.
#define EG2_MODELS_MEM \
  {"otp_ctrl", make<eg2_model::Eg2OtpCtrl>}, \
  {"lc_ctrl", make<eg2_model::Eg2LcCtrl>}, \
  {"rram_ctrl.core", make<eg2_model::Eg2RramCtrl>}, \
  {"sram_ctrl", make<eg2_model::Eg2SramCtrl>}, \
  {"rom_ctrl", make<eg2_model::Eg2RomCtrl>}, \
  {"cheriot_mem_sys", make<eg2_model::Eg2CheriotMemSys>},

#endif  // EG2_MODEL_IP_EG2_MEM_H_
