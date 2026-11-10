// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The reset manager's behaviour as the EG2 model has it, in plain C++ (no SystemC): one core for
// two bindings, as the power manager's (eg2_pwrmgr_core.h):
//   eg2sim      Eg2Rstmgr (eg2_aon.h), on the SystemC model's register store and system resets;
//   block bench eg2_rstmgr_dpi.cc, its own register file, driven by the rstmgr block bench's ports
//               and TL-UL accesses (rstmgr_cmodel_sb).
// The core reaches its registers only through Eg2RstmgrPort.
//
// From rstmgr's documents (hw/top_earlgrey/ip_autogen/rstmgr: doc/theory_of_operation.md,
// data/rstmgr.hjson), never from its RTL:
// - RESET_INFO ("Reset Information"): POR "is always recorded when the device POR circuitry is
//   triggered", alone (a power-on reset resets the register: POR resval 1, the others 0). The other
//   causes "are recorded when authorized by the reset manager", based on the reset category pwrmgr
//   sends (no reset / low-power entry / software or peripheral request), and "the reset information
//   registers constantly record all reset causes in which it is allowed": bits are only ever added
//   until software clears them (rw1c). "The only case where this is not done is POR, where active
//   recording is silenced until the first processor reset release."
//   Bits: LOW_POWER_EXIT for a low-power entry reset; SW_RESET for a reset "due to RESET_REQ";
//   HW_REQ b3..b7 = sysrst_ctrl, aon_timer, pwrmgr main power glitch, alert_handler escalation,
//   rv_dm NDM request (rstmgr.hjson HW_REQ: "The bit mapping is as follows").
// - RESET_REQ.VAL: "When set to kMultiBitBool4True, a reset to power manager is requested. Upon
//   completion of reset, this bit is automatically cleared by hardware."
// - Crash dumps ("Crash Dump Information", ALERT_INFO_CTRL, CPU_INFO_CTRL): with EN set, the latest
//   alert / CPU information is captured "prior to a triggered reset"; EN "is automatically set to 0
//   upon system reset (even if rstmgr is not reset)"; *_INFO_ATTR.CNT_AVAIL "The number of 32-bit
//   values contained in the ... info dump"; *_INFO the 32-bit value INDEX selects; ALERT_REGWEN /
//   CPU_REGWEN (rw0c) lock the controls ("The enable for such debug capture can be locked such that
//   it never captures").
// Not documented, so left to the bindings as bounds or not predicted: which 32-bit word of the dump
// an INDEX selects (word order); what an INDEX at or beyond CNT_AVAIL reads; whether a low-power
// entry reset captures a dump or clears EN; when within a reset a request bit is recorded.
//
// The core records; the binding decides when (eg2sim: once per reset, from its cause; the block
// bench: continuously while pwrmgr's reset request is active, from the category and rstreqs).

#ifndef EG2_MODEL_IP_EG2_RSTMGR_CORE_H_
#define EG2_MODEL_IP_EG2_RSTMGR_CORE_H_

#include <cstdint>
#include <string>
#include <vector>

// The registers the core reads and writes (rstmgr.hjson).
enum class Eg2RstReg : unsigned { kResetReq, kResetInfo, kAlertCtrl, kCpuCtrl };

// What the core needs from its binding.
class Eg2RstmgrPort {
 public:
  virtual ~Eg2RstmgrPort() = default;
  virtual uint32_t rd(Eg2RstReg r) = 0;
  virtual void wr(Eg2RstReg r, uint32_t v) = 0;
  virtual void note(const std::string &) {}
};

class Eg2RstmgrCore {
 public:
  // RESET_INFO bit positions (rstmgr.hjson).
  static constexpr unsigned kPorBit = 0, kLowPowerExitBit = 1, kSwResetBit = 2, kHwReqLsb = 3;
  static constexpr unsigned kHwReqWidth = 5;  // NumHwResets
  static constexpr uint32_t kInfoMask = 0xff;
  static constexpr uint32_t kMuBi4True = 0x6, kMuBi4False = 0x9;

  explicit Eg2RstmgrCore(Eg2RstmgrPort &p) : p_(p) {}

  // A power-on reset: the binding has reset the registers to their reset values (RESET_INFO = POR
  // alone); the dumps are gone and recording waits for the first processor reset release.
  void power_on_reset() {
    alert_dump_.clear();
    cpu_dump_.clear();
    p_.wr(Eg2RstReg::kResetInfo, 1u << kPorBit);
    recording_ = false;
  }

  // The processor's reset was released: recording of reset causes is allowed from now on.
  void processor_reset_released() { recording_ = true; }
  bool recording() const { return recording_; }

  // Record reset causes (RESET_INFO bits, only added). `hw_req` the HW_REQ bits (b0 = sysrst_ctrl
  // .. b4 = rv_dm NDM), `sw` a reset due to RESET_REQ, `low_power` a low-power entry reset.
  void record(uint32_t hw_req, bool sw, bool low_power) {
    if (!recording_) return;
    uint32_t bits = (hw_req & ((1u << kHwReqWidth) - 1)) << kHwReqLsb;
    if (sw) bits |= 1u << kSwResetBit;
    if (low_power) bits |= 1u << kLowPowerExitBit;
    uint32_t v = p_.rd(Eg2RstReg::kResetInfo);
    if ((v | bits) != v) p_.wr(Eg2RstReg::kResetInfo, (v | bits) & kInfoMask);
  }

  // A triggered (not power-on) reset begins: capture the dumps whose capture is enabled (the dump
  // words as the binding gives them), then clear both EN bits.
  void triggered_reset(const std::vector<uint32_t> &alert_dump,
                       const std::vector<uint32_t> &cpu_dump, bool capture_alert = true,
                       bool capture_cpu = true) {
    if (capture_cpu && (p_.rd(Eg2RstReg::kCpuCtrl) & 1)) {
      cpu_dump_ = cpu_dump;
      p_.note("rstmgr: CPU crash dump captured");
    }
    if (capture_alert && (p_.rd(Eg2RstReg::kAlertCtrl) & 1)) {
      alert_dump_ = alert_dump;
      p_.note("rstmgr: alert crash dump captured (" + std::to_string(alert_dump.size()) +
              " words)");
    }
    clear_enables();
  }
  void clear_enables() {
    p_.wr(Eg2RstReg::kAlertCtrl, p_.rd(Eg2RstReg::kAlertCtrl) & ~1u);
    p_.wr(Eg2RstReg::kCpuCtrl, p_.rd(Eg2RstReg::kCpuCtrl) & ~1u);
  }

  // The reset is complete: RESET_REQ back to false ("Upon completion of reset ... cleared").
  void reset_complete() { p_.wr(Eg2RstReg::kResetReq, kMuBi4False); }

  // RESET_REQ.VAL requests a reset from pwrmgr while it is kMultiBitBool4True.
  bool sw_reset_requested() { return (p_.rd(Eg2RstReg::kResetReq) & 0xf) == kMuBi4True; }

  // The captured dumps (empty until a capture).
  const std::vector<uint32_t> &alert_dump() const { return alert_dump_; }
  const std::vector<uint32_t> &cpu_dump() const { return cpu_dump_; }
  // The word INDEX selects, as eg2sim has it (word i of the dump, 0 beyond it).
  static uint32_t word(const std::vector<uint32_t> &d, uint32_t idx) {
    return idx < d.size() ? d[idx] : 0;
  }

 private:
  Eg2RstmgrPort &p_;
  bool recording_ = true;  // eg2sim starts after its power-on reset has been released
  std::vector<uint32_t> alert_dump_, cpu_dump_;
};

#endif  // EG2_MODEL_IP_EG2_RSTMGR_CORE_H_
