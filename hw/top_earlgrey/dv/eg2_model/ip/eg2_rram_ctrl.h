// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_RRAM_CTRL_H_
#define EG2_MODEL_IP_EG2_RRAM_CTRL_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_core_system.h"
#include "eg2_mem_otp.h"
#include "eg2_rram_mp_core.h"

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
// protection per bus word, from the shared model ip/eg2_rram_mp_core.h: the data partition's top
// six pages (OTP integrity and OTP, from page 4090) are never reachable; then the emulated info
// subregions and their window-deny entries, the lowest-numbered enabled MP_REGION that holds the
// page, DEFAULT_REGION if none; in the information partition INFO_PAGE_CFG of the page, and for
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
// scrambling and their errors, the RMA wipe, the
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
    emul_regwen_ = reg("EMUL_INFO_REGWEN_0");
    emul_region_ = reg("EMUL_INFO_REGION_0");
    sub_regwen_ = reg("EMUL_INFO_SUBREGION_REGWEN_0");
    sub_top_ = reg("EMUL_INFO_SUBREGION_0");
    sub_cfg_ = reg("EMUL_INFO_SUBREGION_CFG_0");
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
    std_fault_ = reg("STD_FAULT_STATUS");
    fault_ = reg("FAULT_STATUS");
    set_modelled(std_fault_);
    set_modelled(fault_);
    sec_cm_bench();
    // Fetches from the host window need EXEC (programmers_guide.md: "To enable code execution from
    // RRAM, software must write the magic value 0xa26a38f7 to the EXEC register. Any other value
    // disables instruction fetches") and RRAM not disabled (theory_of_operation.md "RRAM
    // Escalation & Disable": "all host and controller requests are blocked"); a refused fetch is
    // an instruction access fault.
    sys_.add_exec_region("rram_ctrl.host", [this] { return exec_enabled(); });
    sys_.nvm_rma = [this] { return rma_wipe(); };
    power_up();
  }

  bool exec_enabled() { return value(exec_) == 0xa26a38f7u && !disabled(); }

  // theory_of_operation.md "RRAM Escalation & Disable": RRAM is disabled by software (DIS.SW_DIS
  // any value but MuBi4False, rw1s) or by the global escalation (lc_escalate_en), and stays so
  // until a full reset ("the only way to restore normal operation is a full system reset").
  // The local escalation: rram_ctrl.hjson MEM.CTRL.LOCAL_ESC, "A subset of fatal errors cause
  // memory to no longer be accessible. All errors in STD_FAULT_STATUS and FAULT_STATUS are part of
  // this subset" (FAULT_STATUS.PHY_RELBL_ERR only with DIS.LOCAL_ESC_RELBL_ERR: not raised here).
  bool disabled() {
    // rram_ctrl.hjson DIS.LOCAL_ESC_RELBL_ERR (bits 7:4): "Otherwise FAULT_STATUS.PHY_RELBL_ERR is
    // excluded from the local escalation"; theory_of_operation.md "Multi-Bit Error
    // (Uncorrectable)": it "only disables RRAM access once DIS.LOCAL_ESC_RELBL_ERR has been set to
    // any value other than MuBi4False".
    uint32_t esc_faults = value(fault_);
    if (((value(dis_) >> 4) & 0xf) == kMuBi4False) esc_faults &= ~(1u << 9);
    if ((value(dis_) & 0xf) != kMuBi4False || (sys_.lc_escalate_en && sys_.lc_escalate_en()) ||
        (value(std_fault_) != 0 || esc_faults != 0)) {
      disabled_ = true;
    }
    return disabled_;
  }

  bool rma_wipe() {
    // theory_of_operation.md, "RMA Handling": "The RMA entry process wipes the creator, owner
    // and isolated info pages, and the non-OTP portion of the data partition, by overwriting them
    // with pseudo-random values generated by a 64-bit LFSR."
    for (unsigned page : {kCreatorInfoPage, kOwnerInfoPage, 7u}) {
      for (unsigned i = 0; i < kPageBytes; ++i) info_[page * kPageBytes + i] = rma_byte();
    }
    auto it = sys_.memories.find("rram_ctrl.host");
    if (it == sys_.memories.end()) return false;
    std::vector<uint8_t> &m = it->second->data();
    size_t end = std::min<size_t>(m.size(), size_t(kOtpFirstPage) * kPageBytes);
    for (size_t i = 0; i < end; ++i) m[i] = rma_byte();
    return true;
  }

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
  uint8_t rma_byte() {
    // The document specifies a 64-bit LFSR but not its polynomial. This maximal-length polynomial
    // is a model choice; software observes only that the old contents have been overwritten.
    uint64_t bit = rma_lfsr_ & 1;
    rma_lfsr_ >>= 1;
    if (bit) rma_lfsr_ ^= 0xd800000000000000ull;
    return uint8_t(rma_lfsr_);
  }

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
    disabled_ = false;
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

  // Memory protection for one bus word of a controller operation: the shared memory-protection
  // model (ip/eg2_rram_mp_core.h, also the reference of the rram_ctrl_mp unit bench) with this
  // block's registers and the life-cycle enables. A case the documentation leaves open is denied.
  RramMp mp_config() const {
    RramMp m;
    for (unsigned i = 0; i < RramMp::kMpRegions; ++i) {
      m.region[i] = value(uint16_t(mp_region_ + i));
      m.region_cfg[i] = value(uint16_t(mp_cfg_ + i));
    }
    m.default_cfg = value(default_);
    for (unsigned i = 0; i < RramMp::kInfoPages; ++i) m.info_cfg[i] = value(uint16_t(info_cfg_ + i));
    for (unsigned i = 0; i < RramMp::kWindows; ++i) {
      m.emul_regwen[i] = value(uint16_t(emul_regwen_ + i));
      m.emul_region[i] = value(uint16_t(emul_region_ + i));
    }
    for (unsigned i = 0; i < RramMp::kSubregions; ++i) {
      m.sub_regwen[i] = value(uint16_t(sub_regwen_ + i));
      m.sub_top[i] = value(uint16_t(sub_top_ + i));
      m.sub_cfg[i] = value(uint16_t(sub_cfg_ + i));
    }
    Eg2LcCtrl *l = lc();
    m.lc_creator = rram_bool(!l || l->creator_seed_sw_rw_en());
    m.lc_owner = rram_bool(!l || l->owner_seed_sw_rw_en());
    m.lc_iso_rd = rram_bool(!l || l->iso_part_sw_rd_en());
    m.lc_iso_wr = rram_bool(!l || l->iso_part_sw_wr_en());
    return m;
  }

  bool allowed(uint32_t a, bool read) const {
    RramMp::Ctrl c = mp_config().ctrl(RramMp::kSelSw, RramMp::kPhaseInvalid, part_, a / kPageBytes,
                                      !read, false, RramMp::kMuBi4False);
    return c.err == kRramNo && c.fwd == kRramYes;
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
    if (!(value(status_) & (1u << 4)) || op == 3 || disabled()) {
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

  bool disabled_ = false;  // latched until power_up()
  uint16_t std_fault_ = 0, fault_ = 0;

  // ---- Countermeasure faults (rram_ctrl.hjson "countermeasures", STD_FAULT_STATUS, FAULT_STATUS,
  // the alert list) ----
  // A fault the bench injects into one countermeasure's structure (an input event, never a change of
  // the model's behaviour): the countermeasure detects it, sets the status bit the register
  // description gives that structure, and the alert follows ("fatal_std_err: Fired whenever any bit
  // in STD_FAULT_STATUS is set"; "fatal_err: Fired whenever any bit in FAULT_STATUS is set"); every
  // such bit is part of the local escalation (disabled()). The structures per countermeasure are
  // the ones the hjson lists, each with the bit whose description names it.
  struct CmSite {
    const char *what;
    bool std;     // STD_FAULT_STATUS (else FAULT_STATUS)
    unsigned bit;
  };
  static std::vector<CmSite> cm_sites(unsigned cm) {
    switch (cm) {
      case 0:  // CTRL.FSM.SPARSE: lcmgr, otp, arb, rd/wr
        return {{"rram_ctrl_lcmgr FSM (lcmgr_err: \"an FSM sparse encoding error\")", true, 1},
                {"rram_ctrl_otp FSM (otp_err)", true, 3},
                {"rram_ctrl_arb FSM (ctrl_fsm_err: \"Any of the arbiter or rd/wr control FSMs\")",
                 true, 10},
                {"rram_ctrl_rd/wr FSM (ctrl_fsm_err)", true, 10}};
      case 1:  // PHY_WR.FSM.SPARSE
        return {{"rram_phy_wr FSM (phy_fsm_err: \"A RRAM phy fsm\")", true, 7}};
      case 2:  // CTR.REDUN: the lcmgr's seed/address/word/page/wipe-index counters, otp, rd/wr, phy_wr
        return {{"an rram_ctrl_lcmgr counter (lcmgr_err: \"or a count error\")", true, 1},
                {"the rram_ctrl_otp counter (otp_err: \"or a count error\")", true, 3},
                {"an rram_ctrl_rd/wr counter (ctrl_cnt_err)", true, 11},
                {"the rram_phy_wr counter (phy_cnt_err)", true, 8}};
      case 3:  // FIFO.CTR.REDUN: the controller's write FIFO, the phy FIFOs
        return {{"the controller write FIFO's pointers (ctrl_fifo_err)", true, 12},
                {"a phy FIFO's pointers (phy_fifo_err)", true, 6}};
      case 6:  // PHY_ARBITER.CTRL.REDUN
        return {{"a phy arbiter's two instances disagree (phy_arb_err)", true, 9}};
      case 7:  // PHY_HOST_GRANT.CTRL.CONSISTENCY
        return {{"the host granted with info partition access (host_gnt_err)", false, 13}};
      case 8:  // PHY_RSP.CTRL.CONSISTENCY
        return {{"an unexpected transaction done (spurious_done)", false, 12}};
      case 9:  // PHY_RD_RSP.CTRL.INTEGRITY
        return {{"data_valid with no pending request (phy_rd_ctrl_err)", false, 11}};
      default:  // 4, 5: the TL-UL gates' FSMs: no status bit and no alert documented
        return {};
    }
  }
  uint32_t cm_pick_ = 0;  // which structure of the countermeasure (--stim bench.rram_pick=<n>)
  void cm_fault(unsigned cm) {
    std::vector<CmSite> sites = cm_sites(cm);
    if (sites.empty()) {
      Eg2ModelStop::request("undocumented: countermeasure " + std::to_string(cm) +
                            " (a TL-UL gate FSM) has no status bit and no alert in rram_ctrl.hjson "
                            "(dv_tickets.md, rram_ctrl TL-UL gate FSM faults)");
      return;
    }
    const CmSite &s = sites[cm_pick_ % sites.size()];
    sys_.fault_applied("rram_sec_cm", std::string("countermeasure ") + std::to_string(cm) + ": " +
                                          s.what);
    uint16_t r = s.std ? std_fault_ : fault_;
    set_value(r, value(r) | (1u << s.bit));
    raise_alert(s.std ? "fatal_std_err" : "fatal_err");
    // What the bench checks next, through SBA and the alert handler: the bit and the alert.
    sys_.at(sys_.now_ps() + 20000000ull, [this, s, cm] {
      uint16_t r = s.std ? std_fault_ : fault_;
      char d[96];
      std::snprintf(d, sizeof d, "(%s = 0x%08x, bit %u)", s.std ? "STD_FAULT_STATUS" : "FAULT_STATUS",
                    value(r), s.bit);
      if (sys_.bench_check) {
        sys_.bench_check(std::string(s.std ? "STD_FAULT_STATUS" : "FAULT_STATUS") + " bit set",
                         (value(r) >> s.bit) & 1, d);
        int id = Eg2System::alert_id("rram_ctrl", s.std ? "fatal_std_err" : "fatal_err");
        Eg2IpModel *ah = sys_.model("alert_handler");
        int ci = ah && id >= 0 ? ah->reg_index("ALERT_CAUSE_" + std::to_string(id)) : -1;
        bool seen = ci >= 0 && (ah->value(uint16_t(ci)) & 1);
        sys_.bench_check(std::string("rram_ctrl ") + (s.std ? "fatal_std_err" : "fatal_err") +
                             " at the alert handler",
                         seen, "(ALERT_CAUSE_" + std::to_string(id) + ")");
      }
      second_boot();
    });
  }
  // The bench's second boot: boot_phase 1 in the image, then a power-on reset.
  void second_boot() {
    sys_.set_test_param("boot_phase", 1);
    sys_.note("bench: second boot (power-on reset)");
    sys_.apply_stim("rstmgr.por", "1");
  }
  void sec_cm_bench() {
    sys_.stim_handlers["bench.rram_pick"] = [this](const std::string &v) {
      cm_pick_ = uint32_t(std::stoul(v, nullptr, 0));
      return true;
    };
    // chip_sw_eg2_rram_sec_cm_vseq: the software's first `EG2 RRAM seccm ` line.
    sys_.sw_log_watchers.push_back([this](const std::string &m) {
      unsigned cm;
      if (std::sscanf(m.c_str(), "EG2 RRAM seccm fi arm 0x%x", &cm) == 1) cm_fault(cm);
      if (m == "EG2 RRAM seccm verify arm") verify_fault();
      uint32_t a, b;
      if (std::sscanf(m.c_str(), "EG2 RRAM seccm swap 0x%x 0x%x", &a, &b) == 2) swap_fault(a, b);
      if (m.rfind("EG2 RRAM seccm swap silent", 0) == 0) swap_judge(true);
      if (m.rfind("EG2 RRAM seccm swap trapped", 0) == 0) swap_judge(false);
    });
    sys_.read_error = [this](uint32_t addr, bool take) { return relocated_read(addr, take); };
    // A fault in the model, for showing the bench's swap checks fail: the relocated rows are
    // swapped but no longer detected (the address XOR's check taken out).
    sys_.stim_handlers["rram_ctrl.swap_undetected"] = [this](const std::string &v) {
      swap_undetected_ = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
  }
  // The row swap (chip_sw_eg2_rram_sec_cm_vseq scenario 0, sec_cm_mem_addr_infection): the bench
  // swaps two physical RRAM rows, data and ECC, so that each is still a valid codeword.
  // theory_of_operation.md "Address-XOR": each 32-bit bus word is stored XORed with its own
  // address; "If a fault injection attack physically relocates a word to a different address, the
  // XOR inversion on readback will produce a corrupted value. Because the corrupted value is fed
  // into the ECC decoder, the relocation is detected as a data error". "Multi-Bit Error
  // (Uncorrectable)": "Uncorrectable multi-bit errors trigger ecc_fatal_err, which results in a
  // fatal alert", FAULT_STATUS.PHY_RELBL_ERR (bit 9; rram_ctrl.hjson fatal_err "Fired whenever any
  // bit in FAULT_STATUS is set, including uncorrectable ECC errors"), which disables RRAM only once
  // DIS.LOCAL_ESC_RELBL_ERR is set; a host read gets an in-band TL-UL error (programmers_guide.md
  // "Uncorrectable ECC errors or integrity failures produce in-band TL-UL error responses"). The
  // model stores no address XOR or ECC: it records which rows hold a relocated word. Not modelled:
  // a controller read of a relocated row (ERR_CODE.rd_err), which the test does not make.
  void swap_fault(uint32_t a, uint32_t b) {
    std::vector<uint8_t> *m = memory(0);
    uint32_t ra = a & ~15u, rb = b & ~15u;
    if (!m || ra == rb || ra + 16 > m->size() || rb + 16 > m->size()) return;
    for (unsigned i = 0; i < 16; ++i) std::swap((*m)[ra + i], (*m)[rb + i]);
    relocated_ = {ra, rb};
    char d[96];
    std::snprintf(d, sizeof d, "RRAM rows 0x%x and 0x%x swapped (data and ECC)", ra, rb);
    sys_.fault_applied("rram_swap", d);
    // The bench waits for the software's report of its host read, or 2 ms if RRAM stopped it.
    swap_deadline_ = sys_.now_ps() + 2000000000ull;
    sys_.at(swap_deadline_, [this] {
      if (!relocated_.empty()) swap_judge(false, true);
    });
  }
  bool relocated_read(uint32_t addr, bool take) {
    if (relocated_.empty() || swap_undetected_) return false;
    auto it = sys_.memories.find("rram_ctrl.host");
    if (it == sys_.memories.end() || addr < it->second->base()) return false;
    uint32_t row = (addr - uint32_t(it->second->base())) & ~15u;
    if (row != relocated_[0] && row != relocated_[1]) return false;
    if (take) {
      set_value(fault_, value(fault_) | (1u << 9));  // PHY_RELBL_ERR
      raise_alert("fatal_err");
      char d[96];
      std::snprintf(d, sizeof d, "rram_ctrl: host read of relocated row 0x%x: uncorrectable ECC", row);
      sys_.note(d);
    }
    return true;
  }
  // chip_sw_eg2_rram_sec_cm_vseq do_swap: an error if the software says its host read returned
  // the relocated word silently, and one of rram_ctrl's fatal_err or fatal_std_err or
  // rv_core_ibex's fatal_hw_err at the alert handler; then the rows repaired and the second boot.
  void swap_judge(bool silent, bool no_line = false) {
    if (relocated_.empty()) return;
    std::vector<uint32_t> rows = relocated_;
    relocated_.clear();
    if (sys_.bench_check) {
      // --stim bench.inject_fail=1 inverts the check (eg2_bench.h), as for a must-fail image.
      sys_.bench_check("the relocated word was not returned silently",
                       !silent != sys_.bench_inject_fail,
                       no_line ? "(no report from the software)" : "");
      Eg2IpModel *ah = sys_.model("alert_handler");
      auto cause = [&](const char *ip, const char *name) {
        int id = Eg2System::alert_id(ip, name);
        int ci = ah && id >= 0 ? ah->reg_index("ALERT_CAUSE_" + std::to_string(id)) : -1;
        return ci >= 0 && (ah->value(uint16_t(ci)) & 1);
      };
      bool fatal = cause("rram_ctrl", "fatal_err"), std_err = cause("rram_ctrl", "fatal_std_err"),
           core = cause("rv_core_ibex", "fatal_hw_err");
      char d[96];
      std::snprintf(d, sizeof d, "(fatal_err %d fatal_std_err %d core %d)", fatal, std_err, core);
      sys_.bench_check("an alert for the relocated word", fatal || std_err || core, d);
    }
    std::vector<uint8_t> *m = memory(0);
    if (m) {
      for (unsigned i = 0; i < 16; ++i) std::swap((*m)[rows[0] + i], (*m)[rows[1] + i]);
    }
    second_boot();
  }
  // The verify glitch: one bit of the macro's data flipped on a shadow read (theory_of_operation.md
  // "RRAM Read Integrity": "every read operation is issued twice to the macro, and the two results
  // are compared. If the two reads return different data ... rd_intg_err is raised as a fatal
  // alert"; FAULT_STATUS.PHY_RD_INTG_ERR "The shadow response was not identical to the original
  // response"; fatal_err; the local escalation). The software runs from RRAM, so the next read, and
  // its shadow read, comes at once: the fault is applied at the arm. The read buffer is not
  // modelled, so the bench's check that no entry still holds the word is not made here.
  void verify_fault() {
    sys_.fault_applied("rram_verify", "one data bit flipped on the next shadow (verify) read");
    set_value(fault_, value(fault_) | (1u << 10));
    raise_alert("fatal_err");
    sys_.at(sys_.now_ps() + 20000000ull, [this] {
      if (sys_.bench_check) {
        char d[64];
        std::snprintf(d, sizeof d, "(FAULT_STATUS = 0x%08x, bit 10)", value(fault_));
        sys_.bench_check("FAULT_STATUS.PHY_RD_INTG_ERR set", (value(fault_) >> 10) & 1, d);
        int id = Eg2System::alert_id("rram_ctrl", "fatal_err");
        Eg2IpModel *ah = sys_.model("alert_handler");
        int ci = ah && id >= 0 ? ah->reg_index("ALERT_CAUSE_" + std::to_string(id)) : -1;
        sys_.bench_check("rram_ctrl fatal_err at the alert handler",
                         ci >= 0 && (ah->value(uint16_t(ci)) & 1),
                         "(ALERT_CAUSE_" + std::to_string(id) + ")");
      }
      sys_.note("bench: the read-buffer entry check is not made (the model has no read buffer)");
      second_boot();
    });
  }
  uint16_t dis_, exec_, init_, ctrl_regwen_, control_, addr_, mp_cfg_, mp_region_, default_,
      info_cfg_, op_status_, status_, err_code_, err_addr_, phy_, fifo_clr_, fifo_lvl_, curr_lvl_;
  uint16_t emul_regwen_, emul_region_, sub_regwen_, sub_top_, sub_cfg_;
  int rd_win_, wr_win_;
  std::vector<uint8_t> info_;
  std::vector<uint8_t> creator_seed_ = std::vector<uint8_t>(32, 0),
                       owner_seed_ = std::vector<uint8_t>(32, 0);
  bool creator_seed_valid_ = false, owner_seed_valid_ = false;
  std::deque<uint32_t> rd_;
  unsigned part_ = 0, wr_part_ = 0, wr_left_ = 0;
  uint32_t wr_addr_ = 0;
  uint64_t rma_lfsr_ = 0x8f4d35b79a21c6e3ull;
  bool op_err_ = false;
  std::vector<uint32_t> relocated_;  // the two swapped rows (offsets in the data partition)
  uint64_t swap_deadline_ = 0;
  bool swap_undetected_ = false;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_RRAM_CTRL_H_
