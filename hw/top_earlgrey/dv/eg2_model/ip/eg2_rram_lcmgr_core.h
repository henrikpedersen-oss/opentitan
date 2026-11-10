// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The life-cycle manager hardware plug of rram_ctrl (rram_ctrl_lcmgr) as an observer model: plain C++
// (no SystemC), fed with what can be seen at the unit's ports, deciding for each request and each
// output whether the documents allow it. For the unit bench (dpi/eg2_rram_lcmgr_dpi.cc); eg2sim's
// RRAM controller does not use it yet.
//
// From rram_ctrl's documents only, never from its RTL:
//   doc/theory_of_operation.md "LCMGR Hardware Plug" (Initialization stages, the StIdle ... StWait
//     sequence, the seed reads: SeedWidth 256 bits as 8 bus words, read twice, a seed valid only if
//     both reads completed without a read error, an integrity error or a discrepancy; seed_valid
//     cleared when the controller is disabled; not provisioned -> no reads, seeds invalid),
//   "RMA Handling" (the wipe entries: info pages 5, 6, 7 and data pages 0-4089, every word written
//     with LFSR data then read back, digests compared per page; rma_sw_dis On from the start of the
//     RMA entry; StRmaRsp: rma_ack On, rma_sw_dis On, until reset; an error -> StInvalid:
//     rma_dis_access On, rma_ack not asserted),
//   "Memory Protection for LCMGR Hardware Plug" (PhaseSeed: creator and owner info pages, read only;
//     PhaseRma: creator, owner, isolated info pages and every non-OTP data page, read-write),
//   "Transmission Integrity Faults" item 3 (a seed word with broken integrity latches data_invalid:
//     the seed is not used; intg_err),
//   "RRAM Escalation & Disable", programmers_guide.md "Controller initialization" and "Errors
//     Encountered During Hardware Initiated Operations" (seed_err for an error of the seed read),
//   registers.md STATUS.init_done / keys_valid, FAULT_STATUS.seed_err,
//   the FSM drawings rram_lcmgr_fsm.svg / rram_rma_fsm.svg (StDisabled on disable; rma_req taken in
//     StIdle, StReqAddrKey, StReqDataKey and StWait),
//   lc_ctrl_pkg's lc_tx_t (multibit: only On enables; any other value acts as Off, the fail-safe
//     reading of RRAM_CTRL.LC_CTRL.INTERSIG.MUBI) and prim_mubi's mubi4 (rram_disable: any value other
//     than MuBi4False disables, as DIS.SW_DIS describes for software; fail-safe).
// What the documents leave open is not predicted exactly (bounds, or "allowed either way"):
//   - seed_err for a seed word with an integrity error (item 3 names intg_err only);
//   - keys_valid after a disable (only seed_valid is said to be cleared);
//   - fatal_err in the RMA error state (the escalation section: the invalid terminal state "raises
//     fatal_err"; the lcmgr section does not say);
//   - the RMA data itself (an LFSR seeded with rma_seed: the polynomial and output order are not
//     documented), so only "every word written once and read back unchanged" is checked;
//   - what the plug uses OTP's seed_valid for (not documented).
//   - the cycle timing of the synchronizers: the scoreboard gives every input a settle window.

#ifndef EG2_MODEL_IP_EG2_RRAM_LCMGR_CORE_H_
#define EG2_MODEL_IP_EG2_RRAM_LCMGR_CORE_H_

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

class Eg2RramLcmgrCore {
 public:
  // Encodings (lc_ctrl_pkg, prim_mubi_pkg).
  static constexpr uint32_t kLcOn = 0x5, kLcOff = 0xa;
  static constexpr uint32_t kMuBi4False = 0x9;
  // Address map (rram_ctrl README "Address Map", theory_of_operation.md tables).
  static constexpr uint32_t kBusWordsPerPage = 128;  // 32 RRAM words of 128 bits, 4 bus words each
  static constexpr uint32_t kPageBytes = kBusWordsPerPage * 4;
  static constexpr uint32_t kDataPages = 4096, kOtpPages = 6, kWipeDataPages = kDataPages - kOtpPages;
  static constexpr uint32_t kCreatorPage = 5, kOwnerPage = 6, kIsolatedPage = 7;
  static constexpr uint32_t kSeedWords = 8;  // SeedWidth 256 bits as bus words
  enum Phase : uint32_t { kPhaseSeed = 0, kPhaseRma = 1, kPhaseNone = 2, kPhaseInvalid = 3 };
  enum Op : uint32_t { kOpRead = 0, kOpWrite = 1 };

  enum State {
    kIdle, kKeyAddr, kKeyData, kSeeds, kWait, kRma, kRmaRsp, kRmaFail, kDisabled
  };

  static const char *state_name(State s) {
    static const char *n[] = {"Idle", "ReqAddrKey", "ReqDataKey", "ReadSeeds", "Wait",
                              "RmaWipe", "RmaRsp", "RmaInvalid", "Disabled"};
    return n[s];
  }

  // ---- inputs ----
  void reset() { *this = Eg2RramLcmgrCore(); }
  void init() {  // INIT.VAL written 1
    init_seen_ = true;
    if (st_ == kIdle) st_ = kKeyAddr;
  }
  void lc_seed_hw_rd_en(uint32_t v) { provisioned_now_ = (v == kLcOn); }
  // rma_req as lc_ctrl drives it; only On counts.
  void rma_req(uint32_t v) {
    rma_req_on_ = (v == kLcOn);
    if (rma_req_on_) rma_requested_ = true;
  }
  void rram_disable(uint32_t v) {
    if (v == kMuBi4False) return;
    disabled_ = true;
    if (st_ != kRmaRsp) st_ = kDisabled;  // a completed RMA keeps acknowledging lc_ctrl
  }

  // A key request observed on the OTP interface (0 addr, 1 data); returns "" or why it is not allowed.
  std::string key_req(int kind) {
    if (st_ == kDisabled) return "key request after rram_disable";
    if (kind == 0 && st_ != kKeyAddr) return std::string("address key request in ") + state_name(st_);
    if (kind == 1 && st_ != kKeyData) return std::string("data key request in ") + state_name(st_);
    return "";
  }
  void key_ack(int kind, const std::array<uint32_t, 4> &key, const std::array<uint32_t, 4> &rand) {
    if (kind == 0) {
      addr_key_ = key;
      rand_addr_key_ = rand;
      addr_acked_ = true;
      if (st_ == kKeyAddr) st_ = kKeyData;
    } else {
      data_key_ = key;
      rand_data_key_ = rand;
      data_acked_ = true;
      if (st_ == kKeyData) {
        // lc_seed_hw_rd_en decides here (StReqDataKey: "proceeds to StReadSeeds if the life cycle
        // controller has set lc_seed_hw_rd_en, or to StWait otherwise").
        provisioned_ = provisioned_now_;
        st_ = provisioned_ ? kSeeds : kWait;
        if (st_ == kWait) wait_reached_ = true;
      }
    }
  }
  // Ambiguity at the key/seed decision: lc_seed_hw_rd_en changed within the synchronizer window;
  // the scoreboard tells the model which way the plug went (its first seed read or init_done).
  void provisioned_was(bool p) {
    provisioned_ = p;
    if (st_ == kSeeds && !p) { st_ = kWait; wait_reached_ = true; }
    if (st_ == kWait && p && !seed_done_[0] && pass_[0] == 0) st_ = kSeeds;
  }

  // RMA entry seen at the outputs (rma_sw_dis On or a wipe request): returns "" or why not allowed.
  std::string rma_started() {
    if (st_ == kRma) return "";
    if (!rma_requested_) return "RMA entry without an RMA request (rma_req On)";
    if (st_ == kDisabled) return "RMA entry after rram_disable";
    if (st_ == kSeeds) return "RMA entry during the seed reads (taken only from StIdle, the key states and StWait)";
    if (st_ == kRmaRsp || st_ == kRmaFail) return "RMA entered a second time without reset";
    st_ = kRma;
    rma_ever_ = true;
    return "";
  }

  // A request of the controller path: returns "" or why the documents do not allow it.
  std::string request(uint32_t op, uint32_t part, uint32_t byte_addr, uint32_t num_words,
                      uint32_t phase) {
    if (op > 1) return "request with an operation other than read or write";
    uint32_t page = byte_addr / kPageBytes;
    uint32_t first = (byte_addr % kPageBytes) / 4;
    if (first + num_words > kBusWordsPerPage) return "request crosses a page boundary";
    if (st_ == kSeeds) {
      if (phase != kPhaseSeed) return "seed read with a phase other than PhaseSeed";
      if (op != kOpRead) return "write during the seed reads (PhaseSeed is read-only)";
      if (part != 1 || (page != kCreatorPage && page != kOwnerPage))
        return "seed read outside the creator and owner info pages";
      if (first + num_words > kSeedWords) return "seed read beyond the 256-bit seed";
      int s = page == kCreatorPage ? 0 : 1;
      if (s == 1 && !seed_done_[0]) return "owner seed read before the creator seed was evaluated";
      if (seed_done_[s]) return "seed page read a third time";
      cur_seed_ = s;
      return "";
    }
    if (st_ == kRma) {
      if (phase != kPhaseRma) return "RMA wipe access with a phase other than PhaseRma";
      bool ok = part == 1 ? (page == kCreatorPage || page == kOwnerPage || page == kIsolatedPage)
                          : page < kWipeDataPages;
      if (!ok) return "RMA access outside the wipe entries (OTP pages and other info pages are never wiped)";
      return "";
    }
    return std::string("controller request in ") + state_name(st_);
  }

  // One read word of a seed pass (index 0..7, the pass the request above opened).
  void seed_word(uint32_t idx, uint32_t data, bool intg_ok) {
    if (cur_seed_ < 0 || idx >= kSeedWords) return;
    int s = cur_seed_;
    int pass = pass_[s];
    if (pass == 0) first_[s][idx] = data; else if (first_[s][idx] != data) mismatch_[s] = true;
    if (!intg_ok) { intg_[s] = true; intg_seen_ = true; }
  }
  // The end of a seed read operation (done, with the controller path's error bits).
  void seed_done(uint32_t err) {
    if (cur_seed_ < 0) return;
    int s = cur_seed_;
    if (err) rd_err_[s] = true;
    if (++pass_[s] == 2) {
      seed_done_[s] = true;
      valid_[s] = !rd_err_[s] && !intg_[s] && !mismatch_[s];
      if (s == 1) { st_ = kWait; wait_reached_ = true; }
    }
    cur_seed_ = -1;
  }

  // RMA: a wipe write / read-back of one bus word, and an error of a wipe operation.
  void rma_write(uint32_t part, uint32_t word_addr) { (part ? wr_info_ : wr_data_).insert(word_addr); }
  void rma_read(uint32_t part, uint32_t word_addr, bool same_as_written) {
    (part ? rd_info_ : rd_data_).insert(word_addr);
    if (!same_as_written) rma_bad_ = true;
  }
  void rma_error() { rma_bad_ = true; }
  bool rma_failed() const { return rma_bad_; }
  // rma_ack On seen: returns "" or why it is not allowed.
  std::string rma_ack_seen() {
    if (st_ != kRma && st_ != kRmaRsp) return std::string("rma_ack On in ") + state_name(st_);
    if (rma_bad_) return "rma_ack On after a wipe or verify error";
    std::string m = wipe_incomplete();
    if (!m.empty()) return "rma_ack On before the wipe is complete: " + m;
    st_ = kRmaRsp;
    return "";
  }
  std::string wipe_incomplete() const {
    const uint32_t info_words = 3 * kBusWordsPerPage, data_words = kWipeDataPages * kBusWordsPerPage;
    if (wr_info_.size() < info_words || wr_data_.size() < data_words)
      return std::to_string(wr_info_.size()) + "/" + std::to_string(info_words) + " info and " +
             std::to_string(wr_data_.size()) + "/" + std::to_string(data_words) + " data words written";
    if (rd_info_.size() < info_words || rd_data_.size() < data_words)
      return std::to_string(rd_info_.size()) + "/" + std::to_string(info_words) + " info and " +
             std::to_string(rd_data_.size()) + "/" + std::to_string(data_words) + " data words read back";
    return "";
  }
  void rma_fail_seen() { if (st_ == kRma) st_ = kRmaFail; }

  // ---- predictions ----
  State state() const { return st_; }
  bool init_seen() const { return init_seen_; }
  bool keys_acked() const { return addr_acked_ && data_acked_; }
  const std::array<uint32_t, 4> &addr_key() const { return addr_key_; }
  const std::array<uint32_t, 4> &data_key() const { return data_key_; }
  const std::array<uint32_t, 4> &rand_addr_key() const { return rand_addr_key_; }
  const std::array<uint32_t, 4> &rand_data_key() const { return rand_data_key_; }
  bool provisioned() const { return provisioned_; }
  bool seed_evaluated(int s) const { return seed_done_[s]; }
  // seeds_valid[s] may be 1 (must be, once evaluated and not disabled).
  bool seed_valid(int s) const { return valid_[s] && !disabled_; }
  uint32_t seed_word_of(int s, int i) const { return first_[s][i]; }
  // seed_err: must be 1 (lo) / may be 1 (hi).
  bool seed_err_lo() const {
    for (int s = 0; s < 2; s++) if (seed_done_[s] && (mismatch_[s] || rd_err_[s])) return true;
    return false;
  }
  // seed_err may be set as soon as a seed read reports an error or differs from the first read (the
  // documents do not say when it is set, only that these conditions set it).
  bool seed_err_hi() const {
    for (int s = 0; s < 2; s++) if (mismatch_[s] || rd_err_[s] || cur_rd_err_[s]) return true;
    return intg_seen_;
  }
  // A read error reported with the operation's last word (done), before seed_done() is called.
  void seed_rd_err_pending(int s) { if (s >= 0 && s < 2) cur_rd_err_[s] = true; }
  int cur_seed() const { return cur_seed_; }
  bool intg_err() const { return intg_seen_; }
  bool init_done_allowed() const { return st_ == kWait || ((st_ == kRma || st_ == kRmaRsp || st_ == kRmaFail) && wait_reached_); }
  bool rma_ever() const { return rma_ever_; }
  bool disabled() const { return disabled_; }
  bool rma_requested() const { return rma_requested_; }

 private:
  State st_ = kIdle;
  bool init_seen_ = false, provisioned_now_ = false, provisioned_ = false;
  bool rma_req_on_ = false, rma_requested_ = false, rma_ever_ = false, disabled_ = false;
  bool addr_acked_ = false, data_acked_ = false, wait_reached_ = false;
  std::array<uint32_t, 4> addr_key_{}, data_key_{}, rand_addr_key_{}, rand_data_key_{};
  int cur_seed_ = -1;
  int pass_[2] = {0, 0};
  bool seed_done_[2] = {false, false}, valid_[2] = {false, false}, cur_rd_err_[2] = {false, false};
  bool mismatch_[2] = {false, false}, rd_err_[2] = {false, false}, intg_[2] = {false, false};
  bool intg_seen_ = false;
  uint32_t first_[2][kSeedWords] = {};
  std::set<uint32_t> wr_info_, wr_data_, rd_info_, rd_data_;
  bool rma_bad_ = false;
};

#endif  // EG2_MODEL_IP_EG2_RRAM_LCMGR_CORE_H_
