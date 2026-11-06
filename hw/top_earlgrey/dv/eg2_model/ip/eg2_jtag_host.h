// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The JTAG side of the debug path, and the debugger on a host that uses it, in the EG2 SystemC
// model: what OpenOCD with opentitanlib does on FPGA and silicon for the host test programs
// sw/host/tests/chip/rv_dm/src/dtm.rs and control_status.rs. It reaches rv_dm through a modelled
// RISC-V JTAG Debug Transport Module, not through rv_dm's DMI directly (eg2_rv_dm.h's debugger.*
// keys do that), so the TAP selection and the DTM registers are checked too.
//
// TAP selection (pinmux theory_of_operation.md "Strap Sampling and TAP Isolation"): TAP strap 1 is
// IOC5 and TAP strap 0 is IOC8 (pinout_asic.md). Strap 1 = 1, strap 0 = 0 selects the RISC-V TAP
// in TEST_UNLOCKED*, RMA and DEV, which are exactly the states in which lc_ctrl's HW_DEBUG_EN is on
// (the "TAP isolation logic" samples lc_hw_debug_en). 01 selects the life-cycle TAP, 11 the DFT TAP
// (TEST_UNLOCKED*, RMA), 00 none. jtag.strap applies the straps before the chip boots, as the
// harness does (transport.pin_strapping("PINMUX_TAP_RISCV").apply() and then a reset), so the value
// sampled "once at boot" (DEV) and the one sampled "continuously" (TEST_UNLOCKED*, RMA) are both the
// applied one. Only the RISC-V TAP is modelled here; with any other selection the host's checks
// fail with "the RISC-V TAP is not selected" (what TDO shows then is not documented).
//
// The DTM (RISC-V External Debug Support 0.13.2, section 6.1; PULP debug-system.md "JTAG Debug
// Transport Module"): IR length 5 (debug-system.md IrLength default 5; OpenOCD's
// util/openocd/target/lowrisc-earlgrey.cfg "-irlen 5"). Instructions (spec Table 6.1): 0x01 IDCODE,
// 0x10 dtmcs, 0x11 dmi, 0x1f BYPASS (any other IR also selects BYPASS, spec 6.1.2). A TAP reset
// selects IDCODE (spec 6.1.2, IEEE 1149.1).
//   IDCODE   0x10021cdf: util/openocd/target/lowrisc-earlgrey.cfg _CPUTAPID, which the harness
//            gives OpenOCD as -expected-id. No document states the value (the cfg points to the RTL
//            package jtag_id_pkg.sv): ticket "RV_DM JTAG IDCODE, dtmcs.abits and dtmcs.idle not
//            documented". The LSB is 1 as debug-system.md requires.
//   dtmcs    version 1 (0.13; debug-system.md: the debug module is 0.13 compliant), abits 7 (the
//            DM registers of spec 3.12 lie below 0x80; the width is not documented, same ticket),
//            dmistat 0 or the sticky failure, idle 1 (not documented, same ticket); dmireset (bit 16)
//            clears the sticky dmistat.
//   dmi      {address[abits+33:34], data[33:2], op[1:0]} (spec 6.1.5, debug-system.md
//            "DMIACCESS"): op 1 read, 2 write; the next scan returns the read data and op =
//            0 (success) or 2 (failed: rv_dm refused the access because life cycle gates the DMI,
//            eg2_rv_dm.h; no document gives the response, an assumption). A DMI access completes
//            before the next scan here, so op 3 (busy) is never returned.
//
// Each key is one host action at the stimulus's time (--stim jtag.<key>=<value>@<point>, eg2sim.cc):
//   jtag.strap=<strap1><strap0>               e.g. 10 (PINMUX_TAP_RISCV)
//   jtag.checks=<n>                           the host makes n checks in this run: a run that
//                                             ends with fewer (a host step that never ran) fails
//   jtag.idcode_expect=<value>                TAP reset, IDCODE scanned out (OpenOCD's init with
//                                             -expected-id)
//   jtag.dtmcs_expect=<value>[/<mask>]        dtmcs scanned out (OpenOCD's riscv examine needs
//                                             version 1)
//   jtag.dmi_write=<addr>:<value>             a DMI write through the dmi register
//   jtag.dmi_expect=<addr>:<value>[/<mask>]   a DMI read through the dmi register, compared
//   jtag.dmi_wait=<addr>:<value>/<mask>       DMI reads every 10 us until they match, at most 1 ms
//                                             (opentitanlib's wait_halt / wait_resume poll dmstatus)
//   jtag.dmi_rw_random=<addr>:<seed>          a value from <seed> written and read back (dtm.rs's
//                                             data0 and data1 checks with rand::random)
// Every check prints `eg2sim: EG2CHK ok|FAIL jtag <what> <detail>`; a failed one ends the run as
// failed.

#ifndef EG2_MODEL_IP_EG2_JTAG_HOST_H_
#define EG2_MODEL_IP_EG2_JTAG_HOST_H_

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "eg2_rv_dm.h"

namespace eg2_model {

class Eg2JtagHost {
 public:
  static constexpr uint32_t kIdcode = 0x10021cdfu;  // lowrisc-earlgrey.cfg _CPUTAPID (ticket)
  static constexpr unsigned kIrLen = 5, kAbits = 7, kIdle = 1;
  enum : uint32_t { kIrIdcode = 0x01, kIrDtmcs = 0x10, kIrDmi = 0x11, kIrBypass = 0x1f };

  explicit Eg2JtagHost(Eg2RvDm &dm) : dm_(dm) {
    auto &h = dm_.sys().stim_handlers;
    h["jtag.strap"] = [this](const std::string &v) {
      if (v.size() != 2 || (v[0] != '0' && v[0] != '1') || (v[1] != '0' && v[1] != '1')) {
        return false;
      }
      strap_ = unsigned((v[0] - '0') << 1 | (v[1] - '0'));
      log("TAP straps IOC5 (strap 1) = " + v.substr(0, 1) + ", IOC8 (strap 0) = " + v.substr(1));
      return true;
    };
    h["jtag.checks"] = [this](const std::string &v) {
      want_checks_ = int(std::stoul(v, nullptr, 0));
      if (!veto_added_) {
        veto_added_ = true;
        dm_.sys().verdict_checks.push_back([this]() -> std::string {
          if (want_checks_ < 0 || checks_ == want_checks_) return "";
          return "jtag host: " + std::to_string(checks_) + " of " +
                 std::to_string(want_checks_) + " checks made before the run ended";
        });
      }
      return true;
    };
    h["jtag.idcode_expect"] = [this](const std::string &v) {
      uint32_t want = num(v);
      tap_reset();
      uint32_t got = uint32_t(dr_scan(0, 32));
      check("IDCODE (OpenOCD -expected-id)", got, want, 0xffffffffu);
      return true;
    };
    h["jtag.dtmcs_expect"] = [this](const std::string &v) {
      uint32_t want, mask;
      split_mask(v, &want, &mask);
      ir_scan(kIrDtmcs);
      uint32_t got = uint32_t(dr_scan(0, 32));
      check("dtmcs", got, want, mask);
      return true;
    };
    h["jtag.dmi_write"] = [this](const std::string &v) {
      uint32_t a, d, m;
      if (!addr_value(v, &a, &d, &m)) return false;
      uint32_t op = dmi_access(2, a, d, nullptr);
      check("DMI write 0x" + hex2(a) + " op status", op, 0, 3);
      return true;
    };
    h["jtag.dmi_expect"] = [this](const std::string &v) {
      uint32_t a, want, mask, got = 0;
      if (!addr_value(v, &a, &want, &mask)) return false;
      uint32_t op = dmi_access(1, a, 0, &got);
      if (check("DMI read 0x" + hex2(a) + " op status", op, 0, 3)) {
        check("DMI register 0x" + hex2(a), got, want, mask);
      }
      return true;
    };
    h["jtag.dmi_wait"] = [this](const std::string &v) {
      uint32_t a, want, mask;
      if (!addr_value(v, &a, &want, &mask)) return false;
      wait(a, want, mask, 0);
      return true;
    };
    h["jtag.dmi_rw_random"] = [this](const std::string &v) {
      uint32_t a, seed, m;
      if (!addr_value(v, &a, &seed, &m)) return false;
      std::mt19937 rng(seed);
      uint32_t d = uint32_t(rng()), got = 0;
      uint32_t op = dmi_access(2, a, d, nullptr);
      if (!check("DMI write 0x" + hex2(a) + " op status", op, 0, 3)) return true;
      op = dmi_access(1, a, 0, &got);
      if (check("DMI read 0x" + hex2(a) + " op status", op, 0, 3)) {
        check("DMI register 0x" + hex2(a) + " reads back the random value", got, d, 0xffffffffu);
      }
      return true;
    };
  }

 private:
  // ---- TAP and DTM ----
  bool riscv_tap() const { return strap_ == 0x2u && dm_.tap_reachable(); }
  void tap_reset() { ir_ = kIrIdcode; }
  void ir_scan(uint32_t ir) { ir_ = ir & ((1u << kIrLen) - 1); }
  // A DR scan of len bits: shifts in `in`, returns what the selected register captured.
  uint64_t dr_scan(uint64_t in, unsigned len) {
    if (!riscv_tap()) {
      tap_missing_ = true;
      return 0;
    }
    switch (ir_) {
      case kIrIdcode:
        return kIdcode;
      case kIrDtmcs: {
        uint32_t v = (kIdle << 12) | (dmistat_ << 10) | (kAbits << 4) | 1u;
        if (in & (1u << 16)) dmistat_ = 0;  // dmireset
        return v;
      }
      case kIrDmi: {
        // Capture: the result of the previous access; update: start the new one.
        uint64_t out = (uint64_t(last_addr_) << 34) | (uint64_t(last_data_) << 2) |
                       (dmistat_ ? 2u : 0u);
        uint32_t op = uint32_t(in & 3), addr = uint32_t(in >> 34) & ((1u << kAbits) - 1);
        uint32_t data = uint32_t(in >> 2);
        if (!dmistat_ && (op == 1 || op == 2)) {
          bool ok = op == 1 ? dm_.dmi_read(addr, &data) : dm_.dmi_write(addr, data);
          last_addr_ = addr;
          last_data_ = op == 1 ? data : 0;
          if (!ok) dmistat_ = 2;  // sticky until dmireset (spec 6.1.4)
        }
        (void)len;
        return out;
      }
      default:
        return in & 1;  // BYPASS: one bit
    }
  }
  // One DMI access as OpenOCD does it: the request scan, then a nop scan for the result.
  uint32_t dmi_access(uint32_t op, uint32_t addr, uint32_t data, uint32_t *rdata) {
    ir_scan(kIrDmi);
    dr_scan((uint64_t(addr) << 34) | (uint64_t(data) << 2) | op, kAbits + 34);
    uint64_t r = dr_scan(0, kAbits + 34);
    if (rdata) *rdata = uint32_t(r >> 2);
    return uint32_t(r & 3);
  }
  void wait(uint32_t a, uint32_t want, uint32_t mask, unsigned polls) {
    uint32_t got = 0;
    uint32_t op = dmi_access(1, a, 0, &got);
    std::string what = "DMI register 0x" + hex2(a) + " (polled)";
    if (op == 0 && (got & mask) == (want & mask)) {
      check(what, got, want, mask);
      return;
    }
    if (op != 0 || polls >= 100 || dm_.sys().finished()) {
      check(what, got, want, mask);
      return;
    }
    dm_.sys().at(dm_.sys().now_ps() + 10000000ull,
                 [this, a, want, mask, polls] { wait(a, want, mask, polls + 1); });
  }

  // ---- Checks ----
  bool check(const std::string &what, uint32_t got, uint32_t want, uint32_t mask) {
    ++checks_;
    char d[120];
    std::snprintf(d, sizeof d, "= 0x%08x, expected 0x%08x under mask 0x%08x", got, want, mask);
    bool ok = !tap_missing_ && (got & mask) == (want & mask);
    std::string detail = tap_missing_ ? std::string("(the RISC-V TAP is not selected)") : d;
    std::printf("eg2sim: EG2CHK %s jtag %s %s\n", ok ? "ok" : "FAIL", what.c_str(),
                detail.c_str());
    if (!ok) dm_.sys().finish(false, "jtag host: " + what + " " + detail);
    return ok;
  }
  void log(const std::string &s) { std::printf("eg2sim: jtag host: %s\n", s.c_str()); }
  static uint32_t num(const std::string &s) { return uint32_t(std::stoul(s, nullptr, 0)); }
  static std::string hex2(uint32_t a) {
    char b[8];
    std::snprintf(b, sizeof b, "%02x", a);
    return b;
  }
  static void split_mask(const std::string &v, uint32_t *value, uint32_t *mask) {
    size_t s = v.find('/');
    *value = num(v.substr(0, s));
    *mask = s == std::string::npos ? 0xffffffffu : num(v.substr(s + 1));
  }
  static bool addr_value(const std::string &v, uint32_t *a, uint32_t *d, uint32_t *m) {
    size_t c = v.find(':');
    if (c == std::string::npos) return false;
    *a = num(v.substr(0, c));
    split_mask(v.substr(c + 1), d, m);
    return true;
  }

  Eg2RvDm &dm_;
  unsigned strap_ = 0;  // {strap 1, strap 0}; 00: no TAP selected
  uint32_t ir_ = kIrIdcode;
  uint32_t dmistat_ = 0, last_addr_ = 0, last_data_ = 0;
  bool tap_missing_ = false, veto_added_ = false;
  int checks_ = 0, want_checks_ = -1;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_JTAG_HOST_H_
