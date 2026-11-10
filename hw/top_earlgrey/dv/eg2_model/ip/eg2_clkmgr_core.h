// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The clock manager's reference model for the clkmgr block bench (clkmgr_cmodel_sb, through
// dpi/eg2_clkmgr_dpi.cc): the same rules the SystemC model's clkmgr (eg2_clkmgr.h) follows, without
// the SystemC system around it, extended to the block's ports.
//
// From clkmgr's documents (hw/top_earlgrey/ip_autogen/clkmgr: data/clkmgr.hjson,
// doc/theory_of_operation.md, doc/programmers_guide.md, doc/interfaces.md) and reggen's register
// rules (util/reggen/README.md: rw0c, rw1c, regwen, "Shadow Registers"), never from the RTL.
//
// Registers (clkmgr.hjson; ALERT_TEST from registers.md: two write-only alert bits and REGWEN, bit
// 31, rw0c): the access rules of each field, the write enables (EXTCLK_CTRL_REGWEN,
// JITTER_REGWEN, MEASURE_CTRL_REGWEN: "When 0, writes have no effect"), the shadowed measurement
// thresholds (reggen: the first write stages, a second write with the same data commits, a second
// write with other data "signals an update error ... and the shadow register takes again on the
// value of the committed register"; "software can put the shadow register back into the first phase
// by performing a read operation"), RECOV_ERR_CODE.SHADOW_UPDATE_ERR on an update error.
//
// Behaviour (theory_of_operation.md, register descriptions):
//  - CLK_HINTS / CLK_HINTS_STATUS: "When software provides a disable hint, the clock manager checks
//    to see if the associated hardware block is idle. If the hardware block is idle, then the clock
//    is disabled. If the hardware block is not idle, the clock is kept on. For the enable case, the
//    software hint is immediately honored"; CLK_HINTS_STATUS "provides read feedback for the current
//    clock state". "During low power mode, all clocks are gated off regardless of the software hint."
//  - CLK_ENABLES: "0 CLK_<x> is disabled. 1 CLK_<x> is enabled."
//  - Root clock gating: "When the power manager makes a clock enable request, the clock manager
//    ensures all root clock gates are enabled before acknowledging. Likewise ... disable"; "the power
//    manager's request to turn off clocks supersedes all other local controls".
//  - Life-cycle external clock: lc_clk_byp_req_i "forwards the request to ast through
//    io_clk_byp_req_o, which performs the actual clock switch and is acknowledged through
//    io_clk_byp_ack_i ... the life cycle controller is acknowledged through lc_clk_byp_ack_o".
//  - Software external clock: EXTCLK_CTRL.SEL ("When the current value is not kMultiBitBool4True,
//    writing a value of kMultiBitBool4True selects external clock ... Writing any other value has no
//    impact. When the current value is kMultiBitBool4True, writing a value of kMultiBitBool4False
//    selects internal clock ... Writing any other value during this stage has no impact"); "If
//    hardware debug functions are allowed, the clkmgr sends a request signal all_clk_byp_req_o to
//    ast and is acknowledged through all_clk_byp_ack_i"; EXTCLK_STATUS.ACK "kMultiBitBool4True
//    indicates the switch is complete. kMultiBitBool4False indicates the switch is either not
//    possible or still ongoing".
//  - Dividers: "ast can also request a change in the clock division ratio via a dedicated hardware
//    interface (div_step_down_req_i)"; "When the divider is stepped down, a divide-by-4 clock
//    becomes divide-by-2 clock, and a divide-by-2 becomes a divide-by-1 clock".
//  - JITTER_ENABLE: "A value of kMultiBitBool4False disables the jittery clock, while all other
//    values enable jittery clock" (jitter_en_o).
//  - Measurements: "Clock manager then counts the number of relevant root clock cycles in each
//    always-on clock period ... Clock too fast is registered when the clock cycle count is greater
//    than the software programmed max threshold. Clock too slow is registered when the clock cycle
//    count is less than the software programmed min threshold"; "When calib_rdy_i is MuBi4False,
//    hardware sets MEASURE_CTRL_REGWEN to 1 and disables all measurements by clearing the
//    measurement enables ... No measurement errors are registered while calibration is lost".
//
// The documents give no cycle timing for the clock-domain crossings, the idle counters ("counts for
// a period of 10 local clocks") or the measurement start, and leave some cases open. A prediction
// is therefore a pair of bounds: `lo` the bits that must be 1, `hi` the bits that may be 1. Open:
//  - while the design settles (`quiet` false: an input, a source clock, a handshake or a register
//    write changed within the bench's settle window): the fields hardware updates and every output;
//  - anything depending on an input with a value other than the documented ones (a mubi4 or
//    lc_tx value that is neither true nor false): no document says how it is read;
//  - scanmode_i other than MuBi4False (scan mode is not documented);
//  - hi_speed_sel_o (only the register field is documented, not the output);
//  - CLK_HINTS_STATUS while the main root clock is not requested on (whether the status reflects
//    the root gate is not written);
//  - a measurement error from a period within 3 always-on periods of an enable change, with the
//    count within 2 of a threshold (io_div4: 3, it is derived from io), with the source clock
//    stopped or changing, or with the root clock of its domain not requested on; and the time-out
//    errors whenever a measurement is enabled (the time-out window is "approximately 2-4 always on
//    clock cycles");
//  - lc_clk_byp_ack_o while the io clock is switched but div_step_down_req_i is not MuBi4True;
//  - the divider ratio while a clock switch is active without div_step_down_req_i (whether the
//    clock manager steps down on its own at the end of a switch, or ast asks for it, is not clear:
//    "at the completion of the switch, internal dividers are also stepped down").

#ifndef EG2_MODEL_IP_EG2_CLKMGR_CORE_H_
#define EG2_MODEL_IP_EG2_CLKMGR_CORE_H_

#include <cstdint>
#include <cstring>
#include <initializer_list>

class Eg2ClkmgrCore {
 public:
  static constexpr uint32_t kMuBiTrue = 0x6, kMuBiFalse = 0x9;  // prim_mubi_pkg
  static constexpr uint32_t kLcOn = 0x5, kLcOff = 0xa;          // lc_ctrl_pkg lc_tx_t

  enum : uint32_t {
    kAlertTest = 0x00, kExtclkRegwen = 0x04, kExtclkCtrl = 0x08, kExtclkStatus = 0x0c,
    kJitterRegwen = 0x10, kJitterEnable = 0x14, kClkEnables = 0x18, kClkHints = 0x1c,
    kClkHintsStatus = 0x20, kMeasRegwen = 0x24, kIoMeasEn = 0x28, kIoMeasCtrl = 0x2c,
    kIoDiv4MeasEn = 0x30, kIoDiv4MeasCtrl = 0x34, kMainMeasEn = 0x38, kMainMeasCtrl = 0x3c,
    kUsbMeasEn = 0x40, kUsbMeasCtrl = 0x44, kRecovErr = 0x48, kFatalErr = 0x4c, kNumRegs = 0x50 / 4,
  };

  // The output vector the bench observes (clkmgr_cmodel_sb builds the same).
  enum : unsigned {
    kOAllBypTrue = 0, kOAllBypFalse, kOIoBypTrue, kOIoBypFalse, kOLcAckOn, kOLcAckOff,
    kOJitterTrue, kOJitterFalse, kOHiSpeedTrue, kOHiSpeedFalse,
    kOPwrMain, kOPwrIo, kOPwrUsb,
    kOPeriIoDiv4, kOPeriIoDiv2, kOPeriIo, kOPeriUsb, kOPeriSpiHost1,
    kOTransAes, kOTransHmac, kOTransKmac, kOTransOtbn,
    kODivNormal, kODivStepped, kONumOutputs
  };

  // The block's inputs, as levels.
  struct Inputs {
    uint8_t idle[4] = {kMuBiTrue, kMuBiTrue, kMuBiTrue, kMuBiTrue};  // aes, hmac, kmac, otbn
    uint8_t scanmode = kMuBiFalse;
    uint8_t lc_debug_en = kLcOff;
    uint8_t lc_byp_req = kLcOff;
    uint8_t io_byp_ack = kMuBiFalse;
    uint8_t all_byp_ack = kMuBiFalse;
    uint8_t step_down = kMuBiFalse;
    uint8_t calib = kMuBiTrue;
    bool en_main = true, en_io = true, en_usb = true;     // pwr_i.<x>_ip_clk_en
    bool run_main = true, run_io = true, run_usb = true;  // the source clocks toggle
  };

  // mutate (the bench's +clkmgr_cmodel_mutate): 1 jitter enabled only by kMultiBitBool4True,
  // 2 CLK_HINTS_STATUS ignores the idle inputs. A mutated model must make the scoreboard fail.
  explicit Eg2ClkmgrCore(int mutate = 0) : mutate_(mutate) { por(); }

  void por() {
    std::memset(r_, 0, sizeof(r_));
    std::memset(unk_, 0, sizeof(unk_));
    r_[I(kAlertTest)] = 0x80000000u;  // ALERT_TEST.REGWEN (registers.md: rw0c, reset 1)
    r_[I(kExtclkRegwen)] = 1;
    r_[I(kExtclkCtrl)] = kMuBiFalse | (kMuBiFalse << 4);
    r_[I(kJitterRegwen)] = 1;
    r_[I(kJitterEnable)] = kMuBiFalse;
    r_[I(kClkEnables)] = 0x1f;
    r_[I(kClkHints)] = 0xf;
    r_[I(kMeasRegwen)] = 1;
    for (unsigned m = 0; m < 4; ++m) {
      r_[I(meas_desc(m).en)] = kMuBiFalse;
      r_[I(meas_desc(m).ctrl)] = meas_desc(m).reset;
      shadow_[m] = meas_desc(m).reset;
      phase_[m] = false;
      age_[m] = age_off_[m] = 100;
      pend_[m] = pend_to_[m] = 0;
      unsure_[m] = false;
      phase_unknown_[m] = false;
    }
    sel_ = false;
    calib_false_periods_ = 0;
    in_ = Inputs();
    stats_ = Stats();
  }

  // ---- Software ----

  void write(uint32_t addr, uint32_t data, bool quiet) {
    uint32_t i = I(addr);
    stats_.writes++;
    switch (addr & 0x7c) {
      case kAlertTest:
        // RECOV_FAULT and FATAL_FAULT are write-only (read 0); REGWEN, bit 31, is rw0c.
        r_[i] &= (data & 0x80000000u);
        break;
      case kExtclkRegwen:
      case kJitterRegwen:
      case kMeasRegwen:
        r_[i] &= (data & 1);  // rw0c
        if (addr == kMeasRegwen && in_.calib != kMuBiTrue) unk_[i] |= 1;  // hardware sets it again
        break;
      case kExtclkCtrl:
        if (!(r_[I(kExtclkRegwen)] & 1)) break;
        r_[i] = data & 0xff;
        // SEL selects only from the other documented state; any other value has no impact.
        if (!sel_ && (data & 0xf) == kMuBiTrue) sel_ = true;
        else if (sel_ && (data & 0xf) == kMuBiFalse) sel_ = false;
        break;
      case kJitterEnable:
        if (r_[I(kJitterRegwen)] & 1) r_[i] = data & 0xf;
        break;
      case kClkEnables: r_[i] = data & 0x1f; break;
      case kClkHints: r_[i] = data & 0xf; break;
      case kIoMeasEn:
      case kIoDiv4MeasEn:
      case kMainMeasEn:
      case kUsbMeasEn: {
        if (!(r_[I(kMeasRegwen)] & 1) && !(unk_[I(kMeasRegwen)] & 1)) break;
        unsigned m = meas_of(addr);
        uint32_t old = r_[i];
        bool regwen_known = !(unk_[I(kMeasRegwen)] & 1);
        if (regwen_known) {
          r_[i] = data & 0xf;
        } else {
          unk_[i] |= (r_[i] ^ data) & 0xf;  // the write may or may not have been taken
          r_[i] = data & 0xf;
        }
        if (in_.calib != kMuBiTrue) unk_[i] |= 0xf;  // hardware clears the enables
        if (r_[i] != old) {
          if (old == kMuBiTrue) age_off_[m] = 0;
          age_[m] = 0;
        }
        (void)quiet;
        break;
      }
      case kIoMeasCtrl:
      case kIoDiv4MeasCtrl:
      case kMainMeasCtrl:
      case kUsbMeasCtrl: {
        if (!(r_[I(kMeasRegwen)] & 1) && !(unk_[I(kMeasRegwen)] & 1)) break;
        unsigned m = meas_of(addr);
        if ((unk_[I(kMeasRegwen)] & 1) || phase_unknown_[m]) {
          // Whether the write is taken, or which phase it meets, is open: the committed value, the
          // phase and SHADOW_UPDATE_ERR become open (a read brings the phase back to the first).
          unk_[i] |= meas_desc(m).mask;
          phase_unknown_[m] = true;
          if (!(r_[I(kRecovErr)] & 1)) unk_[I(kRecovErr)] |= 1;
          break;
        }
        shadow_write(m, data & meas_desc(m).mask);
        break;
      }
      case kRecovErr: {
        uint32_t clr = data & 0x1ff;
        r_[i] &= ~clr;
        uint32_t keep = 0;  // bits whose source may set them again at once
        for (unsigned m = 0; m < 4; ++m) {
          if (pend_[m] || unsure_[m]) keep |= 2u << m;
          if (pend_to_[m] || unsure_[m]) keep |= 32u << m;
        }
        unk_[i] = (unk_[i] & ~clr) | (unk_[i] & keep) | (clr & keep);
        break;
      }
      default:
        break;  // ALERT_TEST, the read-only registers
    }
  }

  // A read: bounds of the data (lo must be 1, hi may be 1). Shadowed registers leave the second
  // phase on a read.
  void read(uint32_t addr, bool quiet, uint32_t &lo, uint32_t &hi) {
    uint32_t i = I(addr);
    stats_.reads++;
    lo = hi = 0;
    switch (addr & 0x7c) {
      case kExtclkStatus: {
        if (!quiet) {
          lo = 0;
          hi = 0xf;
          break;
        }
        int req = all_byp_req();
        uint32_t ack = in_.all_byp_ack;
        if (req == 1 && ack == kMuBiTrue) lo = hi = kMuBiTrue;
        else if (req == 1 && ack == kMuBiFalse) lo = hi = kMuBiFalse;
        else if (req == 0 && ack == kMuBiFalse) lo = hi = kMuBiFalse;
        else hi = 0xf;
        break;
      }
      case kClkHintsStatus: {
        hi = 0xf;
        if (!quiet || in_.scanmode != kMuBiFalse || !in_.en_main || !in_.run_main) break;
        lo = 0;
        hi = 0;
        for (unsigned b = 0; b < 4; ++b) {
          int on = trans_on(b);
          if (on == 1) lo |= 1u << b, hi |= 1u << b;
          else if (on < 0) hi |= 1u << b;
        }
        break;
      }
      case kIoMeasCtrl:
      case kIoDiv4MeasCtrl:
      case kMainMeasCtrl:
      case kUsbMeasCtrl: {
        unsigned m = meas_of(addr);
        lo = r_[i] & ~unk_[i];
        hi = (r_[i] | unk_[i]) & meas_desc(m).mask;
        // "software can put the shadow register back into the first phase by performing a read"
        phase_[m] = false;
        phase_unknown_[m] = false;
        shadow_[m] = r_[i];
        break;
      }
      case kRecovErr:
        lo = r_[i] & ~unk_[i];
        hi = quiet ? (r_[i] | unk_[i]) & 0x1ff : 0x1ff;
        break;
      case kMeasRegwen:
        lo = r_[i] & ~unk_[i];
        hi = (quiet ? r_[i] | unk_[i] : 1) & 1;
        if (!quiet && in_.calib != kMuBiTrue) lo = 0;
        break;
      case kIoMeasEn:
      case kIoDiv4MeasEn:
      case kMainMeasEn:
      case kUsbMeasEn:
        lo = r_[i] & ~unk_[i];
        hi = (r_[i] | unk_[i]) & 0xf;
        if (!quiet && in_.calib != kMuBiTrue) lo = 0, hi = 0xf;
        break;
      case kFatalErr:
        break;
      default:
        lo = r_[i] & ~unk_[i];
        hi = r_[i] | unk_[i];
        break;
    }
  }

  // ---- Ports ----

  void set_inputs(const Inputs &in) {
    if (in.calib != in_.calib) calib_false_periods_ = 0;
    in_ = in;
    stats_.input_changes++;
    if (in_.calib != kMuBiTrue) {
      // "hardware sets MEASURE_CTRL_REGWEN to 1 and disables all measurements by clearing the
      // measurement enables": when is open until the design settles.
      unk_[I(kMeasRegwen)] |= 1;
      for (unsigned m = 0; m < 4; ++m) {
        unk_[I(meas_desc(m).en)] |= 0xf;
        age_[m] = 0;
      }
    }
  }

  // The design has settled with the current inputs.
  void settled() {
    stats_.settled++;
    if (in_.calib == kMuBiFalse) {
      r_[I(kMeasRegwen)] = 1;
      unk_[I(kMeasRegwen)] = 0;
      for (unsigned m = 0; m < 4; ++m) {
        if (r_[I(meas_desc(m).en)] == kMuBiTrue) age_off_[m] = 0;
        r_[I(meas_desc(m).en)] = kMuBiFalse;
        unk_[I(meas_desc(m).en)] = 0;
      }
    } else if (in_.calib == kMuBiTrue) {
      // Nothing clears or sets them any more: what is open stays open until software writes.
    }
  }

  // One always-on clock period has passed. cnt: the source clocks' rising edges in it (io, main,
  // usb); steady: that source ran the whole period at the frequency of the period before.
  void aon_period(const uint32_t cnt[3], const bool steady[3]) {
    stats_.aon_periods++;
    // A whole always-on period with calib_rdy_i at MuBi4False (far longer than the synchronizers):
    // the hardware has set MEASURE_CTRL_REGWEN and cleared the enables, and holds them so.
    if (in_.calib == kMuBiFalse && calib_false_periods_++ >= 1) {
      r_[I(kMeasRegwen)] = 1;
      unk_[I(kMeasRegwen)] = 0;
      for (unsigned m = 0; m < 4; ++m) {
        if (r_[I(meas_desc(m).en)] == kMuBiTrue) age_off_[m] = 0;
        r_[I(meas_desc(m).en)] = kMuBiFalse;
        unk_[I(meas_desc(m).en)] = 0;
      }
    }
    uint32_t &rec = r_[I(kRecovErr)];
    uint32_t &rec_unk = unk_[I(kRecovErr)];
    int div = div_stepped();  // 1 stepped, 0 normal, -1 open
    for (unsigned m = 0; m < 4; ++m) {
      uint32_t mbit = 2u << m, tbit = 32u << m;
      // Errors scheduled from an earlier period become visible.
      if (pend_[m] && --pend_[m] == 0) {
        rec |= mbit;
        if (!unsure_[m]) rec_unk &= ~mbit;
      }
      if (pend_to_[m] && --pend_to_[m] == 0) rec_unk |= tbit;
      unsure_[m] = false;
      uint32_t en = r_[I(meas_desc(m).en)];
      bool en_known = !unk_[I(meas_desc(m).en)];
      unsigned src = meas_desc(m).src;
      bool root = src == 0 ? in_.en_io : src == 1 ? in_.en_main : in_.en_usb;
      if (age_[m] < 1000) age_[m]++;
      if (age_off_[m] < 1000) age_off_[m]++;
      if (in_.calib == kMuBiFalse) continue;  // "No measurement errors are registered"
      bool on = en_known && en == kMuBiTrue && in_.calib == kMuBiTrue;
      bool off = en_known && en == kMuBiFalse && in_.calib == kMuBiTrue;
      if (off && age_off_[m] >= 3) continue;
      if (!on || age_[m] < 3 || !steady[src] || !root || cnt[src] == 0 ||
          (m == 1 && div < 0) || phase_unknown_[m] || (unk_[I(meas_desc(m).ctrl)] & meas_desc(m).mask)) {
        // Open: the bits may be set from this period.
        unsure_[m] = true;
        rec_unk |= (mbit | tbit) & ~rec;  // a bit already set stays set
        continue;
      }
      // The time-out window is not exact: open whenever measuring.
      rec_unk |= tbit & ~rec;
      uint32_t c = cnt[src];
      unsigned margin = 2;
      if (m == 1) {
        c = div == 1 ? c / 2 : c / 4;
        margin = 3;
      }
      uint32_t ctrl = r_[I(meas_desc(m).ctrl)];
      uint32_t hi_t = ctrl & meas_desc(m).hi_mask, lo_t = (ctrl >> meas_desc(m).lo_shift) & meas_desc(m).hi_mask;
      if (c > hi_t + margin || c + margin < lo_t) {
        // Too fast or too slow: registered after the crossings.
        if (!pend_[m]) pend_[m] = 2;
        rec_unk |= mbit & ~rec;
        stats_.meas_errors++;
      } else if (c + margin <= hi_t && c >= lo_t + margin) {
        stats_.meas_ok++;
      } else {
        unsure_[m] = true;
        rec_unk |= mbit & ~rec;
      }
    }
  }

  // Bounds of the output vector (kO*); everything open while not quiet.
  void outputs(bool quiet, uint32_t &lo, uint32_t &hi) const {
    lo = 0;
    hi = (1u << kONumOutputs) - 1;
    if (!quiet) return;
    uint32_t L = 0, H = 0;
    auto pair = [&](unsigned t, unsigned f, int v) {  // v: 1 true, 0 false, -1 open
      if (v == 1) L |= 1u << t, H |= 1u << t;
      else if (v == 0) L |= 1u << f, H |= 1u << f;
      else H |= (1u << t) | (1u << f);
    };
    auto one = [&](unsigned b, int v) {
      if (v == 1) L |= 1u << b, H |= 1u << b;
      else if (v < 0) H |= 1u << b;
    };
    pair(kOAllBypTrue, kOAllBypFalse, all_byp_req());
    pair(kOIoBypTrue, kOIoBypFalse,
         in_.lc_byp_req == kLcOn ? 1 : in_.lc_byp_req == kLcOff ? 0 : -1);
    // "When the clock switch is complete, the clock dividers are stepped down by a factor of 2 and the
    // life cycle controller is acknowledged": acknowledged once ast has switched and the dividers are
    // stepped down (div_step_down_req_i); without the step-down request the order is not written.
    int lc_ack = -1;
    if (in_.lc_byp_req == kLcOn && in_.io_byp_ack == kMuBiTrue && in_.step_down == kMuBiTrue)
      lc_ack = 1;
    else if (in_.lc_byp_req == kLcOn && in_.io_byp_ack == kMuBiFalse) lc_ack = 0;
    else if (in_.lc_byp_req == kLcOff && in_.io_byp_ack == kMuBiFalse) lc_ack = 0;
    pair(kOLcAckOn, kOLcAckOff, lc_ack);
    uint32_t jit = r_[I(kJitterEnable)];
    int jit_on = mutate_ == 1 ? (jit == kMuBiTrue) : (jit != kMuBiFalse);
    pair(kOJitterTrue, kOJitterFalse, jit_on);
    H |= (1u << kOHiSpeedTrue) | (1u << kOHiSpeedFalse);
    one(kOPwrMain, in_.en_main);
    one(kOPwrIo, in_.en_io);
    one(kOPwrUsb, in_.en_usb);
    bool scan = in_.scanmode != kMuBiFalse;
    static const unsigned kPeriSrc[5] = {0, 0, 0, 2, 0};  // io_div4, io_div2, io, usb, spi_host1
    for (unsigned b = 0; b < 5; ++b) {
      bool src_run = kPeriSrc[b] == 0 ? in_.run_io : in_.run_usb;
      bool root = kPeriSrc[b] == 0 ? in_.en_io : in_.en_usb;
      int v = !src_run ? 0 : scan ? -1 : (root && ((r_[I(kClkEnables)] >> b) & 1));
      one(kOPeriIoDiv4 + b, v);
    }
    for (unsigned b = 0; b < 4; ++b) {
      int v;
      if (!in_.run_main) v = 0;
      else if (scan) v = -1;
      else if (!in_.en_main) v = 0;
      else v = trans_on(b);
      one(kOTransAes + b, v);
    }
    int div = div_stepped();
    if (in_.run_io) pair(kODivStepped, kODivNormal, div);
    lo = L;
    hi = H;
  }

  // ---- For the report and coverage ----
  struct Stats {
    uint64_t writes = 0, reads = 0, input_changes = 0, settled = 0, aon_periods = 0;
    uint64_t meas_errors = 0, meas_ok = 0, update_errors = 0, commits = 0;
  };
  const Stats &stats() const { return stats_; }
  bool sel() const { return sel_; }
  // EXTCLK_CTRL.SEL holds a value other than kMultiBitBool4True and kMultiBitBool4False: the
  // selection is then the documented "no impact" state (a known RTL finding: clkmgr forwards the
  // field itself, so all_clk_byp_req_o carries that value).
  bool sel_other() const {
    uint32_t s = r_[I(kExtclkCtrl)] & 0xf;
    return s != kMuBiTrue && s != kMuBiFalse;
  }
  uint32_t reg(uint32_t addr) const { return r_[I(addr)]; }

 private:
  struct MeasDesc {
    uint32_t en, ctrl, reset, mask, hi_mask, lo_shift;
    unsigned src;  // 0 io, 1 main, 2 usb
  };
  // clkmgr.hjson: HI and LO thresholds per measured clock and their reset values. (A function-local
  // table: a static constexpr data member needs an out-of-class definition before C++17, and
  // Xcelium compiles DPI code in an older mode.)
  static const MeasDesc &meas_desc(unsigned m) {
    static const MeasDesc t[4] = {
      {kIoMeasEn, kIoMeasCtrl, (470u << 10) | 490u, 0xfffff, 0x3ff, 10, 0},
      {kIoDiv4MeasEn, kIoDiv4MeasCtrl, (110u << 8) | 130u, 0xffff, 0xff, 8, 0},
      {kMainMeasEn, kMainMeasCtrl, (490u << 10) | 510u, 0xfffff, 0x3ff, 10, 1},
      {kUsbMeasEn, kUsbMeasCtrl, (230u << 9) | 250u, 0x3ffff, 0x1ff, 9, 2},
    };
    return t[m];
  }

  static uint32_t I(uint32_t addr) { return (addr & 0x7c) / 4; }
  static unsigned meas_of(uint32_t addr) { return ((addr & 0x7c) - kIoMeasEn) / 8; }

  // The shadowed write of measurement m, field by field (reggen instantiates one shadow register per
  // field): the first write stages, the second compares.
  void shadow_write(unsigned m, uint32_t data) {
    uint32_t i = I(meas_desc(m).ctrl);
    if (!phase_[m]) {
      shadow_[m] = data;
      phase_[m] = true;
      return;
    }
    phase_[m] = false;
    const uint32_t f0 = meas_desc(m).hi_mask, f1 = meas_desc(m).hi_mask << meas_desc(m).lo_shift;
    bool err = false;
    for (uint32_t f : {f0, f1}) {
      if ((data & f) == (shadow_[m] & f)) {
        r_[i] = (r_[i] & ~f) | (data & f);
        unk_[i] &= ~f;
      } else {
        err = true;
      }
    }
    shadow_[m] = r_[i];
    if (err) {
      r_[I(kRecovErr)] |= 1;  // SHADOW_UPDATE_ERR
      stats_.update_errors++;
    } else {
      stats_.commits++;
      age_[m] = 0;  // new thresholds: the next periods are measured against them
    }
  }

  // all_clk_byp_req_o: 1 requested, 0 not, -1 open.
  int all_byp_req() const {
    if (!sel_) return 0;
    if (in_.lc_debug_en == kLcOn) return 1;
    if (in_.lc_debug_en == kLcOff) return 0;
    return -1;
  }
  // A transactional clock: 1 on, 0 off, -1 open (the main root on and its source running).
  int trans_on(unsigned b) const {
    bool hint = (r_[I(kClkHints)] >> b) & 1;
    if (hint) return 1;
    if (mutate_ == 2) return 0;
    if (in_.idle[b] == kMuBiTrue) return 0;
    if (in_.idle[b] == kMuBiFalse) return 1;
    return -1;
  }
  // The dividers: 1 stepped down, 0 normal, -1 open.
  int div_stepped() const {
    if (in_.step_down == kMuBiTrue) return 1;
    bool switching = (in_.lc_byp_req != kLcOff) || (sel_ && in_.lc_debug_en != kLcOff);
    if (in_.step_down == kMuBiFalse && !switching) return 0;
    return -1;
  }

  int mutate_;
  uint32_t r_[kNumRegs], unk_[kNumRegs];
  uint32_t shadow_[4];
  bool phase_[4], phase_unknown_[4] = {false, false, false, false};
  unsigned age_[4], age_off_[4];  // always-on periods since the enable last changed / went off
  unsigned pend_[4], pend_to_[4];
  bool unsure_[4];
  bool sel_;
  unsigned calib_false_periods_ = 0;  // always-on periods begun with calib_rdy_i false
  Inputs in_;
  Stats stats_;
};

#endif  // EG2_MODEL_IP_EG2_CLKMGR_CORE_H_
