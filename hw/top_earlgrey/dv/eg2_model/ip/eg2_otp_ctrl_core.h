// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The OTP controller's behaviour as the EG2 model has it, in plain C++ (no SystemC), so that eg2sim
// (Eg2OtpCtrl in eg2_mem_otp.h, a thin adapter) and a self-test (dpi/eg2_otp_ctrl_core_test.cc)
// share it. The core reaches the outside only through Eg2OtpCtrlPort; the OTP array is a byte
// vector the binding owns (eg2sim: the SW_CFG_WINDOW memory, which --otp loads).
//
// From the documents only, never the RTL: hw/top_earlgrey/ip_autogen/otp_ctrl/doc
// (theory_of_operation.md "ToO", programmers_guide.md "PG", interfaces.md "IF", registers.md "REG",
// otp_ctrl_partitions.md, otp_ctrl_mmap.md, otp_ctrl_digests.md) and hw/ip/lc_ctrl/doc.
//
// Untimed (model decision 2026-11-08): initialisation, DAI commands, digests and checks complete at
// once. STATUS.DAI_IDLE is 1 whenever the DAI is not in its terminal error state; CHECK_PENDING
// stays 0; CHECK_TIMEOUT and the two check periods are stored and have no effect (no LFSR timer):
// the integrity and consistency checks run when CHECK_TRIGGER asks for them.
//
// Partitions (otp_ctrl_partitions.md). Buffered (HW_CFG0/1, SECRET0-2, LIFE_CYCLE): read into
// buffer registers at initialisation, the secret ones descrambled (ToO "Buffered Partition"), and
// not updated again until the next initialisation except LIFE_CYCLE by the life-cycle interface
// (ToO "Power-up and Sense"). Every hardware output (HW_CFG broadcast, keys, life-cycle data) comes
// from the buffers. A partition with a non-zero digest is write-locked, and a secret one also
// read-locked, from the initialisation after the digest was written (PG "Digest Calculation
// Sequence"). SECRET0-2 are zeroizable (data/top_earlgrey_otp_ctrl.ipconfig.hjson); their 64-bit
// zeroization marker follows the digest.
//
// DAI (PG "Direct Access Interface", REG DIRECT_ACCESS_*): RD (1), WR (2), DIGEST (4), ZEROIZE (8).
// Granule: 64 bits in secret partitions and for every digest and zeroization marker, else 32 (the
// address's low bits are ignored). AccessError (5): an address in no partition or in LIFE_CYCLE (ToO
// "Life Cycle Partition"); a partition in its terminal error state (ToO "Buffered Partition": it
// "locks down all access through DAI"); RD of a partition its READ_LOCK CSR locks (data and digest,
// ToO "Locking a Partition"), except in RMA (IF lc_rma_state "enable[s] SW read access to
// (read-locked) partitions in the RMA state"); RD of a locked secret partition's data (its digest
// stays readable); RD or WR of SECRET2's RMA_TOKEN and CREATOR_ROOT_KEY_SHARE* while
// lc_creator_seed_sw_rw_en is off (IF; lc_ctrl_otp_accessibility.md); WR to a digest-locked
// partition, to a hardware digest (PG "Zeroization Sequence": "the digest field is never writable
// through the WRITE command in a hardware partition") or to a zeroization marker (same paragraph:
// only ZEROIZE covers "the entire address space of a partition"); DIGEST on a partition without a
// hardware digest, a locked one or one whose digest is already programmed; ZEROIZE outside a
// zeroizable partition. MacroWriteBlankError (4): WR that would clear a programmed bit (PG
// "Programming Already Programmed Regions"); the model keeps the old bits and sets the new ones.
// Each command sets otp_operation_done; an error sets otp_error (PG "Error Handling"). The DAI's code
// is ERR_CODE_11 and clears with the next command (REG ERR_CODE: "automatically recovers ... when
// issuing a new command"). RDATA holds the read word; a refused RD leaves both words 0 (model
// choice). ZEROIZE sets every bit of the word and returns the number of set bits in RDATA_0 (PG
// "Zeroization Sequence" step 2, ToO "Zeroization Detection"; REG DIRECT_ACCESS_CMD instead says
// "the zeroized data": status/rtl_tickets.md), bypassing descrambling; its first success in a
// partition stops that partition's consistency checks until reset (ToO "Checks on Zeroized
// Partitions").
//
// The digest the DIGEST command writes is a model value (FNV-1a over the partition's scrambled form,
// never 0): the chip's PRESENT Davies-Meyer digest needs the IV and finalisation netlist constants,
// which no document the model may read gives. The integrity check therefore checks that the buffer
// still matches the model digest computed over it at initialisation (ToO "Integrity": it checks
// "whether the buffer register contents remain consistent with the calculated digest"), not the
// stored digest itself.
//
// Checks (ToO "Partition Checks", REG CHECK_TRIGGER). Integrity: HW_CFG* and SECRET* with a digest,
// not zeroized at initialisation. Consistency: LIFE_CYCLE (whole partition against its buffer;
// skipped while lc_check_byp_en is on) and HW_CFG*/SECRET* (OTP digest against the buffered digest),
// not zeroized. A failure: the partition's ERR_CODE is CheckFailError (6), its PARTITION_STATUS bit
// and STATUS.PARTITION_ERROR are set, otp_error, fatal_check_error, and the partition's outputs go to
// their defaults.
//
// Escalation (IF lc_escalate_en: "moves all FSMs within OTP into the error state"; REG ERR_CODE
// FSM_STATE_ERROR): every partition, the DAI and the LCI read FsmStateError (7), all outputs go to
// their defaults, the DAI refuses commands (DAI_IDLE 0), otp_error and fatal_check_error (PG "Error
// Handling": unrecoverable errors other than Macro* raise fatal_check_error).
//
// Life-cycle interface (IF "State Transitions"): lc_ctrl programs the 24 count and 20 state words;
// a word that would clear a programmed bit fails the whole request with MacroWriteBlankError in
// ERR_CODE_12, unrecoverable in the LCI (PG table): STATUS.LCI_ERROR, otp_error, fatal_check_error,
// and lc_ctrl is told. The model writes nothing of a failed request (model choice). A successful
// request updates the OTP and the LIFE_CYCLE buffer.
//
// Not modelled: ECC (no MacroEcc* errors), the macro's own errors, the vendor test interface, the
// key derivation functions' PRESENT reduction (the binding gets the seeds and seed_valid), EDN.

#ifndef EG2_MODEL_IP_EG2_OTP_CTRL_CORE_H_
#define EG2_MODEL_IP_EG2_OTP_CTRL_CORE_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---- The OTP partitions (otp_ctrl_partitions.md, otp_ctrl_mmap.md) ----
struct Eg2OtpPart {
  const char *name;
  uint32_t off, size;     // byte offset and size of the whole partition, digest and marker included
  bool secret;            // scrambled, 64-bit granule, read-locked by its digest
  bool hw_digest;         // the digest is computed by the DAI DIGEST command, never written
  bool has_digest;        // LIFE_CYCLE has none
  const char *read_lock;  // the READ_LOCK CSR, or nullptr
  bool zer;               // zeroizable: a zeroization marker (8 bytes) after the digest
  bool buffered;
};

// Offsets and sizes: otp_ctrl_mmap.md (the generated otp_ctrl_reg_pkg.sv has the same numbers).
inline constexpr Eg2OtpPart kEg2OtpParts[] = {
    {"VENDOR_TEST", 0x000, 64, false, false, true, "VENDOR_TEST_READ_LOCK", false, false},
    {"CREATOR_SW_CFG", 0x040, 400, false, false, true, "CREATOR_SW_CFG_READ_LOCK", false, false},
    {"OWNER_SW_CFG", 0x1D0, 672, false, false, true, "OWNER_SW_CFG_READ_LOCK", false, false},
    {"ROT_CREATOR_AUTH_CODESIGN", 0x470, 472, false, false, true,
     "ROT_CREATOR_AUTH_CODESIGN_READ_LOCK", false, false},
    {"ROT_CREATOR_AUTH_STATE", 0x648, 40, false, false, true, "ROT_CREATOR_AUTH_STATE_READ_LOCK",
     false, false},
    {"HW_CFG0", 0x670, 72, false, true, true, nullptr, false, true},
    {"HW_CFG1", 0x6B8, 16, false, true, true, nullptr, false, true},
    {"SECRET0", 0x6C8, 48, true, true, true, nullptr, true, true},
    {"SECRET1", 0x6F8, 96, true, true, true, nullptr, true, true},
    {"SECRET2", 0x758, 96, true, true, true, nullptr, true, true},
    {"LIFE_CYCLE", 0x7B8, 88, false, false, false, nullptr, false, true},
};
inline constexpr int kEg2OtpNumParts = 11;
inline constexpr int kEg2OtpHwCfg0 = 5, kEg2OtpHwCfg1 = 6;
inline constexpr int kEg2OtpSecret0 = 7, kEg2OtpSecret1 = 8, kEg2OtpSecret2 = 9;
inline constexpr int kEg2OtpLifeCycle = 10;
inline constexpr uint32_t kEg2OtpSize = 0x810;
// Items (otp_ctrl_mmap.md).
inline constexpr uint32_t kEg2OtpDeviceId = 0x670, kEg2OtpManufState = 0x690;
inline constexpr uint32_t kEg2OtpEnSramIfetch = 0x6B8, kEg2OtpDisRvDmLateDebug = 0x6B9;
inline constexpr uint32_t kEg2OtpTestUnlockToken = 0x6C8, kEg2OtpTestExitToken = 0x6D8;
inline constexpr uint32_t kEg2OtpNvmAddrKeySeed = 0x6F8, kEg2OtpNvmDataKeySeed = 0x718;
inline constexpr uint32_t kEg2OtpSramDataKeySeed = 0x738;
inline constexpr uint32_t kEg2OtpRmaToken = 0x758;
inline constexpr uint32_t kEg2OtpCreatorRootKeyShare0 = 0x768, kEg2OtpCreatorRootKeyShare1 = 0x788;
inline constexpr uint32_t kEg2OtpLcTransitionCnt = 0x7B8, kEg2OtpLcState = 0x7E8;
// The digest of a partition: its last 8 bytes, or the 8 before its zeroization marker.
inline constexpr uint32_t eg2_otp_digest_off(const Eg2OtpPart &pt) {
  return pt.off + pt.size - (pt.zer ? 16 : 8);
}
inline constexpr uint32_t eg2_otp_zer_off(const Eg2OtpPart &pt) { return pt.off + pt.size - 8; }

// The life-cycle signals otp_ctrl receives (IF "Inter-Module Signals").
enum class Eg2OtpLc : unsigned { kCreatorSeedSwRwEn, kSeedHwRdEn, kRmaState, kCheckBypEn };

class Eg2OtpCtrlPort {
 public:
  virtual ~Eg2OtpCtrlPort() = default;
  virtual void intr(unsigned bit) = 0;               // 0 otp_operation_done, 1 otp_error
  virtual void fatal_alert(const char *name) = 0;    // held until reset by the binding
  virtual bool lc(Eg2OtpLc sig) = 0;                 // On?
  virtual void note(const std::string &) {}
};

class Eg2OtpCtrlCore {
 public:
  // Register indices (REG); the binding maps them to its register file by name (reg_name).
  enum Reg : unsigned {
    kStatus = 0, kPartitionStatus, kErrCode0,
    kDaiRegwen = kErrCode0 + 13, kDaiCmd, kDaiAddress, kDaiWdata0, kDaiWdata1, kDaiRdata0,
    kDaiRdata1, kCheckTriggerRegwen, kCheckTrigger, kCheckRegwen, kCheckTimeout,
    kIntegrityCheckPeriod, kConsistencyCheckPeriod, kReadLock0,
    kDigest0 = kReadLock0 + 5,  // 2 per partition with a digest, partitions 0..9
    kNumRegs = kDigest0 + 20,
  };
  enum Err : uint32_t {
    kNoError = 0, kMacroError = 1, kWriteBlankError = 4, kAccessError = 5, kCheckFailError = 6,
    kFsmStateError = 7,
  };
  static constexpr unsigned kDaiErr = 11, kLciErr = 12;
  // ZeroizationValidBound (ToO "Zeroization Detection") is "macro-specific" and no document gives
  // it: the model takes 56 of the marker's 64 bits.
  static constexpr unsigned kZeroizeValidBound = 56;
  static constexpr uint32_t kMuBi8True = 0x96;

  static std::string reg_name(unsigned r) {
    static const char *kFixed[] = {"STATUS", "PARTITION_STATUS_0"};
    static const char *kTail[] = {"DIRECT_ACCESS_REGWEN", "DIRECT_ACCESS_CMD",
                                  "DIRECT_ACCESS_ADDRESS", "DIRECT_ACCESS_WDATA_0",
                                  "DIRECT_ACCESS_WDATA_1", "DIRECT_ACCESS_RDATA_0",
                                  "DIRECT_ACCESS_RDATA_1", "CHECK_TRIGGER_REGWEN",
                                  "CHECK_TRIGGER", "CHECK_REGWEN", "CHECK_TIMEOUT",
                                  "INTEGRITY_CHECK_PERIOD", "CONSISTENCY_CHECK_PERIOD"};
    if (r < kErrCode0) return kFixed[r];
    if (r < kDaiRegwen) return "ERR_CODE_" + std::to_string(r - kErrCode0);
    if (r < kReadLock0) return kTail[r - kDaiRegwen];
    if (r < kDigest0) return kEg2OtpParts[r - kReadLock0].read_lock;
    if (r < kNumRegs) {
      return std::string(kEg2OtpParts[(r - kDigest0) / 2].name) + "_DIGEST_" +
             std::to_string((r - kDigest0) % 2);
    }
    return "";
  }

  Eg2OtpCtrlCore(Eg2OtpCtrlPort *port, std::vector<uint8_t> *mem) : port_(port), mem_(mem) {}

  // ---- Initialisation (ToO "Power-up and Sense"; PG "Initialization") ----
  void power_up() {
    if (mem_->size() < kEg2OtpSize) mem_->resize(kEg2OtpSize, 0);
    for (uint32_t &v : r_) v = 0;
    r_[kDaiRegwen] = r_[kCheckTriggerRegwen] = r_[kCheckRegwen] = 1;
    for (int p = 0; p < 5; ++p) r_[kReadLock0 + p] = 1;
    terminal_ = false;
    for (int p = 0; p < kEg2OtpNumParts; ++p) {
      const Eg2OtpPart &pt = kEg2OtpParts[p];
      part_err_[p] = kNoError;
      zer_started_[p] = false;
      zeroized_[p] = pt.zer && popcount(dword(eg2_otp_zer_off(pt))) >= kZeroizeValidBound;
      uint64_t dg = pt.has_digest ? dword(eg2_otp_digest_off(pt)) : 0;
      locked_[p] = dg != 0;
      if (pt.has_digest) {
        r_[kDigest0 + 2 * p] = uint32_t(dg);
        r_[kDigest0 + 2 * p + 1] = uint32_t(dg >> 32);
      }
      buf_[p].assign(pt.size, 0);
      if (!pt.buffered) continue;
      for (uint32_t i = 0; i < pt.size; ++i) buf_[p][i] = (*mem_)[pt.off + i];
      if (pt.secret) {
        for (uint32_t a = pt.off; a < eg2_otp_digest_off(pt); a += 8) {
          set64(&buf_[p][a - pt.off], descramble(dword(a), p));
        }
      }
      snapshot_[p] = buffer_digest(p);
    }
    update_status();
  }

  // ---- CSRs ----
  uint32_t csr_read(unsigned r) const { return r < kNumRegs ? r_[r] : 0; }
  void csr_write(unsigned r, uint32_t wdata, uint32_t wmask = 0xffffffffu) {
    uint32_t w = wdata & wmask;
    switch (r) {
      case kDaiRegwen: case kCheckTriggerRegwen: case kCheckRegwen:
        r_[r] &= (w | ~wmask) & 1;  // rw0c
        return;
      case kDaiCmd:
        if (r_[kDaiRegwen] & 1) dai_command(w & 0xf);
        return;
      case kDaiAddress:
        if (r_[kDaiRegwen] & 1) r_[r] = (r_[r] & ~wmask) | (w & 0xfff);
        return;
      case kDaiWdata0: case kDaiWdata1:
        if (r_[kDaiRegwen] & 1) r_[r] = (r_[r] & ~wmask) | w;
        return;
      case kCheckTrigger:
        if (r_[kCheckTriggerRegwen] & 1) check_trigger(w & 3);
        return;
      case kCheckTimeout: case kIntegrityCheckPeriod: case kConsistencyCheckPeriod:
        if (r_[kCheckRegwen] & 1) r_[r] = (r_[r] & ~wmask) | w;
        return;
      default:
        if (r >= kReadLock0 && r < kDigest0) {  // rw0c, enabled by DIRECT_ACCESS_REGWEN
          if (r_[kDaiRegwen] & 1) r_[r] &= (w | ~wmask) & 1;
        }
        return;  // read-only registers
    }
  }

  // A read of SW_CFG_WINDOW at OTP byte `off` (REG SW_CFG_WINDOW: "directly maps to the
  // corresponding offset"). false: the read answers a bus error (ToO "Unbuffered Partition": reads
  // "will error out if they are out of bounds, or if read access is locked"; the same for a locked
  // secret partition's data and for SECRET2's seeds while lc_creator_seed_sw_rw_en is off).
  bool window_read(uint32_t off, unsigned len, uint8_t *data) {
    std::memset(data, 0, len);
    if (uint64_t(off) + len > kEg2OtpSize) return false;
    for (unsigned i = 0; i < len; ++i) {
      int p = part_of(off + i);
      if (p < 0) return false;
      if (read_locked_csr(p)) return false;
      bool meta = in_meta(p, off + i);
      if (kEg2OtpParts[p].secret && locked_[p] && !meta) return false;
      if (p == kEg2OtpSecret2 && !meta && !port_->lc(Eg2OtpLc::kCreatorSeedSwRwEn)) return false;
      data[i] = (*mem_)[off + i];
    }
    return true;
  }

  // ---- Life-cycle interface ----
  struct LcData {
    bool valid = false;                    // LIFE_CYCLE, SECRET0 and SECRET2 initialised
    uint16_t state[20] = {}, count[24] = {};
    bool test_tokens_valid = false, rma_token_valid = false, secrets_valid = false;
    uint8_t test_unlock_token[16] = {}, test_exit_token[16] = {}, rma_token[16] = {};
  };
  LcData lc_data() const {
    LcData d;
    d.valid = part_ok(kEg2OtpLifeCycle) && part_ok(kEg2OtpSecret0) && part_ok(kEg2OtpSecret2);
    if (part_ok(kEg2OtpLifeCycle)) {
      for (int j = 0; j < 24; ++j) d.count[j] = buf16(kEg2OtpLcTransitionCnt + 2 * j);
      for (int i = 0; i < 20; ++i) d.state[i] = buf16(kEg2OtpLcState + 2 * i);
    }
    // IF "State, Counter and Token Output": the token valids follow the partitions' locks;
    // secrets_valid is On "iff the SECRET2 partition ... has been locked with a digest".
    d.test_tokens_valid = part_ok(kEg2OtpSecret0) && locked_[kEg2OtpSecret0];
    d.rma_token_valid = part_ok(kEg2OtpSecret2) && locked_[kEg2OtpSecret2];
    d.secrets_valid = d.rma_token_valid;
    if (d.test_tokens_valid) {
      buf_bytes(kEg2OtpTestUnlockToken, 16, d.test_unlock_token);
      buf_bytes(kEg2OtpTestExitToken, 16, d.test_exit_token);
    }
    if (d.rma_token_valid) buf_bytes(kEg2OtpRmaToken, 16, d.rma_token);
    return d;
  }
  // A programming request: words[0..23] the count, words[24..43] the state (partition order).
  // false: the request failed (lc_otp_program_o.err).
  bool lci_program(const uint16_t words[44]) {
    if (terminal_ || r_[kErrCode0 + kLciErr] != kNoError) return false;
    bool fail = false;
    if (lci_inject_ > 0 && --lci_inject_ == 0) fail = true;
    for (int i = 0; i < 44 && !fail; ++i) {
      uint16_t old = half(kEg2OtpLcTransitionCnt + 2 * i);
      fail = (old & ~words[i]) != 0;
    }
    if (fail) {
      r_[kErrCode0 + kLciErr] = kWriteBlankError;
      update_status();
      port_->intr(1);
      port_->fatal_alert("fatal_check_error");
      port_->note("otp_ctrl: the life-cycle programming request would clear a programmed bit "
                  "(MacroWriteBlankError in the LCI)");
      return false;
    }
    for (int i = 0; i < 44; ++i) {
      uint32_t a = kEg2OtpLcTransitionCnt + 2 * i;
      (*mem_)[a] = uint8_t(words[i]);
      (*mem_)[a + 1] = uint8_t(words[i] >> 8);
      buf_[kEg2OtpLifeCycle][a - kEg2OtpParts[kEg2OtpLifeCycle].off] = uint8_t(words[i]);
      buf_[kEg2OtpLifeCycle][a + 1 - kEg2OtpParts[kEg2OtpLifeCycle].off] = uint8_t(words[i] >> 8);
    }
    return true;
  }
  // The chip bench's injected programming error: the n-th next LCI request fails as if it cleared
  // a programmed bit (chip_sw_lc_ctrl_program_error_vseq).
  void inject_lci_error(unsigned n) { lci_inject_ = n; }

  // lc_escalate_en On (IF). Idempotent.
  void lc_escalate() {
    if (terminal_) return;
    terminal_ = true;
    for (unsigned i = 0; i < 13; ++i) r_[kErrCode0 + i] = kFsmStateError;
    for (int p = 0; p < kEg2OtpNumParts; ++p) part_err_[p] = kFsmStateError;
    update_status();
    port_->intr(1);
    port_->fatal_alert("fatal_check_error");
  }

  // ---- Hardware outputs (from the buffers; defaults while the partition is in error) ----
  bool partition_locked(int p) const { return locked_[p]; }
  bool zeroized(int p) const { return zeroized_[p]; }
  uint32_t partition_error(int p) const { return part_err_[p]; }
  // HW_CFG broadcast (IF "Hardware Config Bits"); 0 bytes while HW_CFG* is not valid.
  uint8_t hw_cfg_byte(uint32_t off) const {
    int p = part_of(off);
    return (p == kEg2OtpHwCfg0 || p == kEg2OtpHwCfg1) && part_ok(p)
               ? buf_[p][off - kEg2OtpParts[p].off] : 0;
  }
  bool en_sram_ifetch() const { return hw_cfg_byte(kEg2OtpEnSramIfetch) == kMuBi8True; }
  // SRAM/OTBN key requests (IF "Interfaces to SRAM and OTBN Scramblers"): seed_valid once SECRET1 is
  // provisioned (locked); unprovisioned seeds read as zero ("derived from all-zero constants").
  bool sram_key_seed(uint8_t seed[16]) const {
    bool v = part_ok(kEg2OtpSecret1) && locked_[kEg2OtpSecret1];
    std::memset(seed, 0, 16);
    if (v) buf_bytes(kEg2OtpSramDataKeySeed, 16, seed);
    return v;
  }
  // NVM scrambling seeds (IF "Interface to NVM Scrambler").
  bool nvm_key_seeds(uint8_t addr[32], uint8_t data[32]) const {
    bool v = part_ok(kEg2OtpSecret1) && locked_[kEg2OtpSecret1];
    std::memset(addr, 0, 32);
    std::memset(data, 0, 32);
    if (v) {
      buf_bytes(kEg2OtpNvmAddrKeySeed, 32, addr);
      buf_bytes(kEg2OtpNvmDataKeySeed, 32, data);
    }
    return v;
  }
  // keymgr_creator_root_key (IF "Interface to Key Manager": provided "if they have been provisioned
  // and locked"; lc_seed_hw_rd_en "enables HW read access to the CREATOR_ROOT_KEY_SHARE0 and
  // CREATOR_ROOT_KEY_SHARE1"). Shares descrambled; zero when not valid (the chip's "random netlist
  // constants" are not documented).
  bool creator_root_key(uint8_t share0[32], uint8_t share1[32]) {
    bool v = part_ok(kEg2OtpSecret2) && locked_[kEg2OtpSecret2] &&
             port_->lc(Eg2OtpLc::kSeedHwRdEn);
    std::memset(share0, 0, 32);
    std::memset(share1, 0, 32);
    if (v) {
      buf_bytes(kEg2OtpCreatorRootKeyShare0, 32, share0);
      buf_bytes(kEg2OtpCreatorRootKeyShare1, 32, share1);
    }
    return v;
  }

  // A fault in a buffer register (the self-test's, or a bench's, fault injection): flips one bit of
  // partition p's buffer. The next integrity check of p must find it.
  void inject_buffer_fault(int p, uint32_t byte_off, uint8_t mask) {
    if (p >= 0 && p < kEg2OtpNumParts && byte_off < buf_[p].size()) buf_[p][byte_off] ^= mask;
  }

  // ---- PRESENT scrambling (ToO "Scrambling": 128-bit key, 31 rounds, a netlist key per secret
  // partition; the testing keys of top_earlgrey.secrets.testing.gen.hjson) ----
  static uint64_t scramble(uint64_t data, int p) {
    PresentKey keys[32];
    present_round_keys(p, keys);
    for (unsigned i = 0; i < 31; ++i) data = present_perm(present_sbox(data ^ keys[i].hi));
    return data ^ keys[31].hi;
  }
  static uint64_t descramble(uint64_t data, int p) {
    PresentKey keys[32];
    present_round_keys(p, keys);
    data ^= keys[31].hi;
    for (int i = 30; i >= 0; --i) {
      data = present_inv_perm(data);
      data = present_inv_sbox(data);
      data ^= keys[i].hi;
    }
    return data;
  }

  static int part_of(uint32_t a) {
    for (int p = 0; p < kEg2OtpNumParts; ++p) {
      if (a >= kEg2OtpParts[p].off && a < kEg2OtpParts[p].off + kEg2OtpParts[p].size) return p;
    }
    return -1;
  }

 private:
  // ---- Array access ----
  uint8_t byte(uint32_t a) const { return a < mem_->size() ? (*mem_)[a] : 0; }
  uint16_t half(uint32_t a) const { return uint16_t(byte(a) | (byte(a + 1) << 8)); }
  uint64_t dword(uint32_t a) const {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(byte(a + i)) << (8 * i);
    return v;
  }
  static void set64(uint8_t *b, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) b[i] = uint8_t(v >> (8 * i));
  }
  static unsigned popcount(uint64_t v) {
    unsigned n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
  }
  uint16_t buf16(uint32_t a) const {
    const std::vector<uint8_t> &b = buf_[kEg2OtpLifeCycle];
    uint32_t o = a - kEg2OtpParts[kEg2OtpLifeCycle].off;
    return uint16_t(b[o] | (b[o + 1] << 8));
  }
  void buf_bytes(uint32_t a, unsigned n, uint8_t *out) const {
    int p = part_of(a);
    for (unsigned i = 0; i < n; ++i) out[i] = buf_[p][a - kEg2OtpParts[p].off + i];
  }
  bool part_ok(int p) const { return part_err_[p] == kNoError && !terminal_; }

  bool is_digest(int p, uint32_t a) const {
    const Eg2OtpPart &pt = kEg2OtpParts[p];
    return pt.has_digest && a >= eg2_otp_digest_off(pt) && a < eg2_otp_digest_off(pt) + 8;
  }
  bool is_zer(int p, uint32_t a) const {
    return kEg2OtpParts[p].zer && a >= eg2_otp_zer_off(kEg2OtpParts[p]);
  }
  bool in_meta(int p, uint32_t a) const { return is_digest(p, a) || is_zer(p, a); }
  bool wide(int p, uint32_t a) const { return kEg2OtpParts[p].secret || in_meta(p, a); }
  bool read_locked_csr(int p) const {
    if (!kEg2OtpParts[p].read_lock || (r_[kReadLock0 + p] & 1)) return false;
    return !port_->lc(Eg2OtpLc::kRmaState);
  }

  void update_status() {
    uint32_t ps = 0;
    for (int p = 0; p < kEg2OtpNumParts; ++p) {
      r_[kErrCode0 + p] = part_err_[p];
      if (part_err_[p]) ps |= 1u << p;
    }
    r_[kPartitionStatus] = ps;
    uint32_t st = 0;
    if (ps) st |= 1u << 0;                                    // PARTITION_ERROR
    if (r_[kErrCode0 + kDaiErr]) st |= 1u << 1;               // DAI_ERROR
    if (r_[kErrCode0 + kLciErr]) st |= 1u << 2;               // LCI_ERROR
    if (!terminal_) st |= 1u << 8;                            // DAI_IDLE
    r_[kStatus] = st;
  }

  // ---- DAI ----
  void dai_command(uint32_t c) {
    if (!c || terminal_) return;
    uint32_t err = c == 1 ? dai_read() : c == 2 ? dai_write() : c == 4 ? dai_digest()
                 : c == 8 ? dai_zeroize() : uint32_t(kAccessError);  // several bits: model choice
    r_[kErrCode0 + kDaiErr] = err;
    update_status();
    port_->intr(0);
    if (err) port_->intr(1);
  }
  int dai_part(uint32_t *a) {
    *a = r_[kDaiAddress] & 0xfff;
    int p = part_of(*a);
    if (p < 0 || p == kEg2OtpLifeCycle || part_err_[p] != kNoError) return -1;
    return p;
  }
  bool seed_refused(int p, uint32_t a) {
    return p == kEg2OtpSecret2 && !in_meta(p, a) && !port_->lc(Eg2OtpLc::kCreatorSeedSwRwEn);
  }
  uint32_t dai_read() {
    r_[kDaiRdata0] = r_[kDaiRdata1] = 0;
    uint32_t a;
    int p = dai_part(&a);
    if (p < 0 || read_locked_csr(p) || seed_refused(p, a)) return kAccessError;
    bool meta = in_meta(p, a);
    if (kEg2OtpParts[p].secret && locked_[p] && !meta) return kAccessError;
    if (wide(p, a)) {
      a &= ~7u;
      uint64_t d = dword(a);
      if (kEg2OtpParts[p].secret && !meta) d = descramble(d, p);
      r_[kDaiRdata0] = uint32_t(d);
      r_[kDaiRdata1] = uint32_t(d >> 32);
    } else {
      a &= ~3u;
      r_[kDaiRdata0] = uint32_t(dword(a));
    }
    return kNoError;
  }
  uint32_t dai_write() {
    uint32_t a;
    int p = dai_part(&a);
    if (p < 0 || locked_[p] || seed_refused(p, a) || is_zer(p, a)) return kAccessError;
    if (kEg2OtpParts[p].hw_digest && is_digest(p, a)) return kAccessError;
    unsigned n = wide(p, a) ? 8 : 4;
    a &= ~(n - 1);
    uint64_t w = uint64_t(r_[kDaiWdata0]) | (n == 8 ? uint64_t(r_[kDaiWdata1]) << 32 : 0);
    if (kEg2OtpParts[p].secret && !in_meta(p, a)) w = scramble(w, p);
    uint32_t err = kNoError;
    for (unsigned i = 0; i < n; ++i) {
      uint8_t nb = uint8_t(w >> (8 * i)), &ob = (*mem_)[a + i];
      if (ob & ~nb) err = kWriteBlankError;
      ob = uint8_t(ob | nb);
    }
    return err;
  }
  uint32_t dai_digest() {
    uint32_t a;
    int p = dai_part(&a);
    if (p < 0 || !kEg2OtpParts[p].hw_digest || locked_[p]) return kAccessError;
    uint32_t da = eg2_otp_digest_off(kEg2OtpParts[p]);
    if (dword(da)) return kAccessError;  // already programmed; it locks at the next reset
    set64(&(*mem_)[da], model_digest(&(*mem_)[kEg2OtpParts[p].off], da - kEg2OtpParts[p].off));
    return kNoError;
  }
  uint32_t dai_zeroize() {
    uint32_t a;
    int p = dai_part(&a);
    if (p < 0 || !kEg2OtpParts[p].zer) return kAccessError;
    a &= ~7u;  // only hardware (secret) partitions are zeroizable here: 64-bit words
    for (unsigned i = 0; i < 8; ++i) (*mem_)[a + i] = 0xff;
    r_[kDaiRdata0] = popcount(dword(a));
    r_[kDaiRdata1] = 0;
    zer_started_[p] = true;
    return kNoError;
  }

  // ---- Checks ----
  static uint64_t model_digest(const uint8_t *b, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h ? h : 1;
  }
  // The model digest over partition p's buffer in its scrambled form (ToO "Secret Partition
  // Integrity Checks": "re-scrambling the data before passing it through the digest").
  uint64_t buffer_digest(int p) const {
    const Eg2OtpPart &pt = kEg2OtpParts[p];
    if (!pt.has_digest) return 0;
    uint32_t n = eg2_otp_digest_off(pt) - pt.off;
    std::vector<uint8_t> b(buf_[p].begin(), buf_[p].begin() + n);
    if (pt.secret) {
      for (uint32_t o = 0; o < n; o += 8) {
        uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[o + i]) << (8 * i);
        set64(&b[o], scramble(v, p));
      }
    }
    return model_digest(b.data(), n);
  }
  void check_trigger(uint32_t which) {
    if (terminal_) return;
    for (int p = kEg2OtpHwCfg0; p < kEg2OtpNumParts; ++p) {
      if (part_err_[p] != kNoError || zeroized_[p]) continue;
      const Eg2OtpPart &pt = kEg2OtpParts[p];
      bool bad_i = false, bad_c = false;
      if ((which & 1) && pt.has_digest && locked_[p]) bad_i = buffer_digest(p) != snapshot_[p];
      if ((which & 2) && !zer_started_[p]) {
        if (p == kEg2OtpLifeCycle) {
          if (!port_->lc(Eg2OtpLc::kCheckBypEn)) {
            for (uint32_t i = 0; i < pt.size; ++i) bad_c = bad_c || buf_[p][i] != (*mem_)[pt.off + i];
          }
        } else {
          uint32_t o = eg2_otp_digest_off(pt) - pt.off;
          for (unsigned i = 0; i < 8; ++i) {
            bad_c = bad_c || buf_[p][o + i] != (*mem_)[pt.off + o + i];
          }
        }
      }
      if (bad_i || bad_c) {
        part_err_[p] = kCheckFailError;
        update_status();
        port_->note(std::string("otp_ctrl: ") + (bad_i ? "integrity" : "consistency") +
                    " check failed in partition " + pt.name);
        port_->intr(1);
        port_->fatal_alert("fatal_check_error");
      }
    }
  }

  // ---- PRESENT ----
  struct PresentKey {
    uint64_t hi, lo;
  };
  static PresentKey present_next_key(PresentKey k, unsigned round) {
    static constexpr uint8_t kSbox[16] = {0xc, 0x5, 0x6, 0xb, 0x9, 0x0, 0xa, 0xd,
                                          0x3, 0xe, 0xf, 0x8, 0x4, 0x7, 0x1, 0x2};
    uint64_t hi = ((k.hi & 7) << 61) | (k.lo >> 3);
    uint64_t lo = ((k.lo & 7) << 61) | (k.hi >> 3);
    hi = (hi & 0x00ffffffffffffffull) | (uint64_t(kSbox[hi >> 60]) << 60) |
         (uint64_t(kSbox[(hi >> 56) & 0xf]) << 56);
    hi ^= uint64_t(round) >> 2;
    lo ^= uint64_t(round) << 62;
    return {hi, lo};
  }
  static uint64_t present_sbox(uint64_t d) {
    static constexpr uint8_t kSbox[16] = {0xc, 0x5, 0x6, 0xb, 0x9, 0x0, 0xa, 0xd,
                                          0x3, 0xe, 0xf, 0x8, 0x4, 0x7, 0x1, 0x2};
    uint64_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= uint64_t(kSbox[(d >> (4 * i)) & 0xf]) << (4 * i);
    return out;
  }
  static uint64_t present_inv_sbox(uint64_t d) {
    static constexpr uint8_t kInv[16] = {0x5, 0xe, 0xf, 0x8, 0xc, 0x1, 0x2, 0xd,
                                         0xb, 0x4, 0x6, 0x3, 0x0, 0x7, 0x9, 0xa};
    uint64_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= uint64_t(kInv[(d >> (4 * i)) & 0xf]) << (4 * i);
    return out;
  }
  static uint64_t present_perm(uint64_t d) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; ++i) out |= ((d >> i) & 1) << (i == 63 ? 63 : (16 * i) % 63);
    return out;
  }
  static uint64_t present_inv_perm(uint64_t d) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; ++i) out |= ((d >> i) & 1) << (i == 63 ? 63 : (4 * i) % 63);
    return out;
  }
  static void present_round_keys(int p, PresentKey keys[32]) {
    static constexpr PresentKey kKeys[] = {
        {0xf9089cf2787c6c14ull, 0xb7d1a6c0618e831full},
        {0xc8129e7a4149ba28ull, 0x31fe2e9fb3b2a1d2ull},
        {0x5254f861ba80f82bull, 0x303da30635aa3d3aull},
    };
    keys[0] = kKeys[p - kEg2OtpSecret0];
    for (unsigned i = 1; i < 32; ++i) keys[i] = present_next_key(keys[i - 1], i);
  }

  Eg2OtpCtrlPort *port_;
  std::vector<uint8_t> *mem_;
  uint32_t r_[kNumRegs] = {};
  uint32_t part_err_[kEg2OtpNumParts] = {};
  bool locked_[kEg2OtpNumParts] = {}, zeroized_[kEg2OtpNumParts] = {},
       zer_started_[kEg2OtpNumParts] = {};
  std::vector<uint8_t> buf_[kEg2OtpNumParts];
  uint64_t snapshot_[kEg2OtpNumParts] = {};
  bool terminal_ = false;
  unsigned lci_inject_ = 0;
};

#endif  // EG2_MODEL_IP_EG2_OTP_CTRL_CORE_H_
