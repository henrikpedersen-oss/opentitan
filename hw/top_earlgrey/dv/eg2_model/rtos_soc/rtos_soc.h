// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The second board of the EG2 SystemC model: Ibex's CHERIoT RTOS test SoC
// (ibex_cheriot_verification: ibex/dv/cheriot-rtos-test-suite, README.md and
// rtl/cheriot_rtos_soc.sv), so that the CHERIoT RTOS suites, whose UART output is their verdict,
// run on the model alone (eg2sim --board rtos_soc; rtos_soc.cc builds the platform).
//
//   0x0010_0000  SRAM, 128 KiB; boot vector +0x80                 Eg2Memory
//   0x3000_0000  revocation bitmap window, 2 KiB                   RtosRevbmWindow (meta SRAM)
//   0x4000_0000  code RAM, 1 MiB (the subsystem's NVM)             Eg2Memory
//   0x8000_a000  rev_ctl, in front of the subsystem's TBRE         RtosRevCtl -> Eg2CheriotMemSys
//   0x8004_0000  timer (CLINT mtime / mtimecmp)                    RtosClint
//   0x8010_0000  UART0                                             Eg2Uart
//   0x8050_0000  perf device and its DMA, 4 KiB (the perf tests)   RtosPerfDev
//   0x8800_0000  rv_plic, 32 sources (1 revoker, 8 UART0, 30 perf) Eg2RvPlic
//   anything else: the default responder (reads 0, writes dropped) RtosDefaultRsp
//   the debug module's system bus access, as the testbench drives it  RtosDbgHost
//
// Reused where the hardware is the same: the UART (OpenTitan uart, the version Sonata vendors: the
// register map Sonata's hjson gives, rtos_soc_regs.h, matches EG2's but for ALERT_TEST), rv_plic
// (OpenTitan's, 32 sources, msip_o open), the TBRE's register model with its sweep
// (cheriot_mem_sys, the same hjson as EG2's at the commit the SoC pins). New: the timer, rev_ctl
// with the SoC's shim to the TBRE, the revocation bitmap window, the default responder. One clock,
// 40 MHz (the testbench's and the board file's timer_hz): the core (one instruction per cycle), the
// UART's baud clock and the timer.
//
// The core is Ibex in CHERIoT mode from reset (cheriot_en_i tied 1): MPACT-CHERIoT from the reset
// state (Eg2CoreConfig::cheriot_at_reset), with this board's tags (one meta SRAM: the bitmap, the
// code RAM's tags, the SRAM's) and its load barrier (heap base 0x0010_0000, the bitmap read through
// Ibex's own port, trvk_revbm_*, as in the SoC). Instruction fetch reaches the two memories only.
//
// RtosRunMonitor plays the testbench's part (dv/tb/cheriot_rtos_tb.sv: uartdpi's log and exit
// string, the UART line watch, the idle watch, the cycle limit) and prints the testbench's lines,
// so that dv/scripts/cheriot_rtos_verdict.sh judges a model run by the same rules.

#ifndef EG2_MODEL_RTOS_SOC_RTOS_SOC_H_
#define EG2_MODEL_RTOS_SOC_RTOS_SOC_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include "../eg2_ip_model.h"
#include "../ip/eg2_mem.h"
#include "rtos_soc_regs.h"

namespace rtos_soc {

// The address map (README.md; Sonata's tl_main_pkg, which the SoC's crossbar decodes with).
constexpr uint32_t kSramBase = 0x00100000, kSramSize = 128 * 1024;
constexpr uint32_t kRevbmBase = 0x30000000, kRevbmSize = 2048;
constexpr uint32_t kCodeBase = 0x40000000, kCodeSize = 1024 * 1024;
constexpr uint32_t kRevCtlBase = 0x8000a000, kRevCtlSize = 0x1000;
constexpr uint32_t kTimerBase = 0x80040000, kTimerSize = 0x10000;
constexpr uint32_t kUart0Base = 0x80100000, kUart0Size = 0x1000;
constexpr uint32_t kPlicBase = 0x88000000, kPlicSize = 0x8000000;
// The meta SRAM (cheriot_mem_subsys.sv): the bitmap, the code RAM's tags, the SRAM's tags, one bit
// per 8 bytes each.
constexpr uint32_t kMetaCodeTags = kRevbmSize;
constexpr uint32_t kMetaSramTags = kMetaCodeTags + kCodeSize / 64;
constexpr uint32_t kMetaSize = kMetaSramTags + kSramSize / 64;
constexpr uint64_t kClockHz = 40000000;
constexpr uint16_t kPlicRevoker = 1;  // Sonata's numbering (doc/ip/plic.md)
constexpr Eg2System::Clk kClk = Eg2System::Clk::kMain;

// The timer (Sonata rtl/system/rv_timer.sv; doc/ip/timer.md): a 64-bit mtime that counts every clock
// cycle and a 64-bit mtimecmp; the core's timer interrupt is raised once mtime >= mtimecmp. The
// documentation's register map (0x0-0xC) is not the RTL's: the RTL, and the board file the firmware
// is built for, have the CLINT layout (mtimecmp 0x4000, mtime 0xBFF8); the model follows the RTL.
// The doc says the interrupt is raised once the counter "has exceeded" the compare value; the RTL
// compares >=, and the model follows the RTL. Where the doc is silent, from the RTL: the interrupt
// stays raised until software writes either half of mtimecmp (and is raised again at once if
// mtime is still >= mtimecmp); a write with fewer than four byte enables is ignored; mtime and
// mtimecmp reset to 0, so the interrupt is raised from the first cycle until software writes
// mtimecmp; an offset with no register answers an error.
class RtosClint : public Eg2IpModel {
 public:
  RtosClint(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    cmp_lo_ = reg("MTIMECMP_LOW");
    cmp_hi_ = reg("MTIMECMP_HIGH");
    lo_ = reg("MTIME_LOW");
    hi_ = reg("MTIME_HIGH");
    set_modelled(lo_);
    set_modelled(hi_);
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    rebase(0);
    irq_ = false;
    evaluate();
  }
  void start_of_simulation() override { on_reset(Eg2ResetCause::kPor); }
  void on_time(uint64_t) override { evaluate(); }
  void on_clock_change() override { evaluate(); }

 protected:
  bool reg_before_write(uint16_t, uint32_t, uint32_t wmask) override { return wmask == ~0u; }
  void reg_before_read(uint16_t r) override {
    if (r == lo_ || r == hi_) evaluate();
  }
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == lo_ || r == hi_) {
      uint64_t mt = mtime(sys_.now_ps());
      mt = r == lo_ ? (mt & ~0xffffffffull) | value(lo_)
                    : (mt & 0xffffffffull) | (uint64_t(value(hi_)) << 32);
      rebase(mt);
    } else if (r == cmp_lo_ || r == cmp_hi_) {
      irq_ = false;
    }
    evaluate();
  }

 private:
  uint64_t mtime(uint64_t t) const {
    return base_mtime_ + (sys_.clock_cycles(kClk, t) - sys_.clock_cycles(kClk, base_ps_));
  }
  void rebase(uint64_t mt) {
    base_ps_ = sys_.now_ps();
    base_mtime_ = mt;
  }
  void evaluate() {
    uint64_t mt = mtime(sys_.now_ps());
    uint64_t cmp = (uint64_t(value(cmp_hi_)) << 32) | value(cmp_lo_);
    set_value(lo_, uint32_t(mt));
    set_value(hi_, uint32_t(mt >> 32));
    if (mt >= cmp) irq_ = true;
    sys_.mtip = irq_;
    // When mtime reaches mtimecmp; never, for a compare value beyond 2^40 cycles away.
    if (!irq_ && cmp - mt < (1ull << 40)) {
      sys_.schedule(this, sys_.clock_time_of_cycle(
                              kClk, sys_.clock_cycles(kClk, base_ps_) + (cmp - base_mtime_)));
    } else {
      sys_.unschedule(this);
    }
  }
  uint16_t cmp_lo_, cmp_hi_, lo_, hi_;
  uint64_t base_ps_ = 0, base_mtime_ = 0;
  bool irq_ = false;
};

// rev_ctl (Sonata rtl/ip/rev_ctl: data/rev_ctl.hjson, doc/ip/revoker.md) and the SoC's shim from its
// core-side interface to the memory subsystem's TBRE (rtl/cheriot_rev_ctl_tbre.sv, whose header is
// its specification).
//
// rev_ctl: START_ADDR and END_ADDR; a write of any value to GO starts the engine, and GO reads
// 0x55000000; EPOCH bit 0 is the engine running, bits 31:1 count the sweeps; INTERRUPT_ENABLE
// "for raising an interrupt when sweeping is done"; INTERRUPT_STATUS "used to read interrupt status
// and clear it". Where the doc is silent, from the RTL (rev_ctl.sv): a sweep is done when running
// falls; done advances the epoch and, with INTERRUPT_ENABLE set, is the interrupt (a one-cycle pulse
// to PLIC source 1, which the PLIC's gateway latches); INTERRUPT_STATUS is set by done only while
// INTERRUPT_ENABLE is clear, cleared by any write, and held clear while INTERRUPT_ENABLE is set.
//
// The shim, on a go while idle: the range is [START_ADDR rounded down to a capability, END_ADDR),
// (END - start + 7) / 8 capabilities, and running rises. An empty range (END <= START) is a sweep
// of nothing: running falls a cycle later. Otherwise it programs the TBRE as firmware would, over
// its register port (here the TLM socket tbre, bound to the subsystem's register block): on its
// first go INTR_ENABLE = tbre_done; TBRE_BASE_ADDR, TBRE_NUM_CAPS, TBRE_START = 1; it reads
// TBRE_STATUS: start_err or sweep_err fails the sweep, busy waits for tbre_done, else it completes
// at once. At tbre_done it reads TBRE_STATUS (busy or sweep_err fails), writes INTR_STATE =
// tbre_done, and running falls. A failed sweep keeps running high until reset (the epoch stays odd,
// the RTOS never sees it complete). A go while a sweep runs is ignored. The shim's own bus cycles
// take no time here.
class RtosRevCtl : public Eg2IpModel {
 public:
  tlm_utils::simple_initiator_socket<RtosRevCtl> tbre;

  RtosRevCtl(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base), tbre("tbre") {
    start_ = reg("START_ADDR");
    end_ = reg("END_ADDR");
    go_ = reg("GO");
    epoch_ = reg("EPOCH");
    istatus_ = reg("INTERRUPT_STATUS");
    ienable_ = reg("INTERRUPT_ENABLE");
    for (uint16_t r : {go_, epoch_, istatus_}) set_modelled(r);
  }

  // The TBRE behind the shim: its register block on the shim's port, its tbre_done into the shim.
  void attach(eg2_model::Eg2CheriotMemSys *m) {
    tbre.bind(m->socket);
    m->irq_out = [this](uint8_t, bool level) {
      tbre_done_ = level;
      if (level && state_ == kWaitDone) sys_.schedule(this, sys_.now_ps());
    };
    using namespace eg2_regs::cheriot_mem_sys_regs;
    off_intr_state_ = kRegs[kIntrState].offset;
    off_intr_enable_ = kRegs[kIntrEnable].offset;
    off_base_ = kRegs[kTbreBaseAddr].offset;
    off_num_ = kRegs[kTbreNumCaps].offset;
    off_start_ = kRegs[kTbreStart].offset;
    off_status_ = kRegs[kTbreStatus].offset;
  }

  bool failed() const { return state_ == kFailed; }
  uint32_t sweeps() const { return epoch_count_; }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    state_ = kIdle;
    running_ = istatus_q_ = intr_en_written_ = false;
    epoch_count_ = 0;
  }
  void on_time(uint64_t) override {
    if (state_ == kEmpty) {
      done();
    } else if (state_ == kWaitDone && tbre_done_) {
      uint32_t st = 0;
      if (!csr(false, off_status_, &st) || (st & (kBusy | kSweepErr))) return fail(st);
      uint32_t one = 1;
      if (!csr(true, off_intr_state_, &one)) return fail(st);
      done();
    }
  }

 protected:
  void reg_before_read(uint16_t r) override {
    if (value(ienable_) & 1) istatus_q_ = false;
    if (r == go_) set_value(go_, 0x55000000u);
    if (r == epoch_) set_value(epoch_, (epoch_count_ << 1) | (running_ ? 1u : 0u));
    if (r == istatus_) set_value(istatus_, istatus_q_ ? 1u : 0u);
  }
  void reg_write(uint16_t r, uint32_t, uint32_t, uint32_t) override {
    if (r == go_) {
      set_value(go_, 0x55000000u);
      go();
    } else if (r == istatus_) {
      istatus_q_ = false;
    } else if (r == ienable_ && (value(ienable_) & 1)) {
      istatus_q_ = false;
    }
  }

 private:
  static constexpr uint32_t kBusy = 1u << 0, kStartErr = 1u << 8, kSweepErr = 1u << 9;
  enum State { kIdle, kEmpty, kWaitDone, kFailed };

  void go() {
    if (state_ != kIdle) return;
    uint32_t start = value(start_), end = value(end_), start_cap = start & ~7u;
    running_ = true;
    if (!(end > start)) {
      state_ = kEmpty;
      sys_.schedule(this, sys_.clock_time_of_cycle(kClk, sys_.clock_cycles(kClk, sys_.now_ps()) + 1));
      return;
    }
    uint32_t num = (end - start_cap + 7) >> 3, one = 1, st = 0;
    if (!intr_en_written_) {
      if (!csr(true, off_intr_enable_, &one)) return fail(0);
      intr_en_written_ = true;
    }
    if (!csr(true, off_base_, &start_cap) || !csr(true, off_num_, &num) ||
        !csr(true, off_start_, &one) || !csr(false, off_status_, &st) ||
        (st & (kStartErr | kSweepErr))) {
      return fail(st);
    }
    if (st & kBusy) {
      state_ = kWaitDone;
      return;
    }
    // A sweep already over: acknowledge its interrupt, as at its end.
    if (!csr(true, off_intr_state_, &one)) return fail(st);
    done();
  }
  // running falls: the epoch counts the sweep and, if enabled, the interrupt pulses.
  void done() {
    state_ = kIdle;
    running_ = false;
    ++epoch_count_;
    if (value(ienable_) & 1) {
      sys_.set_plic_source(kPlicRevoker, true);
      sys_.set_plic_source(kPlicRevoker, false);
    } else {
      istatus_q_ = true;
    }
  }
  void fail(uint32_t status) {
    state_ = kFailed;
    char b[128];
    std::snprintf(b, sizeof b, "rev_ctl: the TBRE sweep failed (TBRE_STATUS 0x%08x); running stays "
                               "high until reset", status);
    sys_.note(b);
  }
  // One access of the shim to the TBRE's registers; false for an error response.
  bool csr(bool write, uint32_t offset, uint32_t *data) {
    tlm::tlm_generic_payload t;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    t.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
    t.set_address(offset);
    t.set_data_ptr(reinterpret_cast<unsigned char *>(data));
    t.set_data_length(4);
    t.set_streaming_width(4);
    t.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    tbre->b_transport(t, delay);
    return t.is_response_ok();
  }

  uint16_t start_, end_, go_, epoch_, istatus_, ienable_;
  uint32_t off_intr_state_ = 0, off_intr_enable_ = 0, off_base_ = 0, off_num_ = 0, off_start_ = 0,
           off_status_ = 0;
  State state_ = kIdle;
  bool running_ = false, istatus_q_ = false, intr_en_written_ = false, tbre_done_ = false;
  uint32_t epoch_count_ = 0;
};

// Software's window on the revocation bitmap (cheriot_mem_sys doc/theory_of_operation.md, "Meta
// SRAM Address Map"): the first kRevbmSize bytes of the meta SRAM. "Accesses must be full 32-bit
// words": any other access answers an error. The core's bus is 32 bits wide, so an access of a
// capability (CLC, CSC: the loader zeroes the bitmap with csc) is two words; the model's core
// presents it as one access of 8 bytes, which the window takes as its two words. (The window also
// answers errors outside CHERIoT mode; this SoC's core is in CHERIoT mode from reset.)
class RtosRevbmWindow : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<RtosRevbmWindow> socket;

  RtosRevbmWindow(sc_core::sc_module_name n, std::vector<uint8_t> *meta)
      : sc_core::sc_module(n), socket("socket"), meta_(meta) {
    socket.register_b_transport(this, &RtosRevbmWindow::b_transport);
  }

 private:
  void b_transport(tlm::tlm_generic_payload &t, sc_core::sc_time &) {
    uint64_t off = t.get_address();
    unsigned len = t.get_data_length();
    if (len == 0 || (len & 3) || (off & 3) || off + len > kRevbmSize || t.get_byte_enable_ptr()) {
      t.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    if (t.is_read()) std::memcpy(t.get_data_ptr(), &(*meta_)[off], len);
    else std::memcpy(&(*meta_)[off], t.get_data_ptr(), len);
    t.set_response_status(tlm::TLM_OK_RESPONSE);
  }
  std::vector<uint8_t> *meta_;
};

// The default responder (rtl/cheriot_rtos_default_rsp.sv, whose header is its specification): every
// data access no device takes. Reads return 0 and writes are dropped, or, with error_mode (the
// SoC's +default_rsp_error=1), every access answers an error. Each is reported as the RTL reports
// it, "[cheriot_rtos_default_rsp] <time>: READ |WRITE <address> (unmapped, <outcome>)" (time in
// ps), one per 32-bit bus word (an 8-byte capability access is two, as the core issues it), and the
// counts at the end.
class RtosDefaultRsp : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<RtosDefaultRsp> socket;
  bool error_mode = false;
  uint64_t reads = 0, writes = 0;

  explicit RtosDefaultRsp(sc_core::sc_module_name n) : sc_core::sc_module(n), socket("socket") {
    socket.register_b_transport(this, &RtosDefaultRsp::b_transport);
  }
  void report() const {
    std::printf("[cheriot_rtos_default_rsp] %llu read(s), %llu write(s) to unmapped addresses\n",
                (unsigned long long)reads, (unsigned long long)writes);
  }

 private:
  void b_transport(tlm::tlm_generic_payload &t, sc_core::sc_time &) {
    bool rd = t.is_read();
    uint32_t a = uint32_t(t.get_address());
    for (uint32_t w = a & ~3u; w < a + t.get_data_length(); w += 4) {
      ++(rd ? reads : writes);
      std::printf("[cheriot_rtos_default_rsp] %llu: %s %08x (unmapped, %s)\n",
                  (unsigned long long)Eg2System::get().now_ps(), rd ? "READ " : "WRITE",
                  w == (a & ~3u) ? a : w,
                  error_mode ? "TL-UL error" : (rd ? "read as 0" : "dropped"));
    }
    if (rd) std::memset(t.get_data_ptr(), 0, t.get_data_length());
    t.set_response_status(error_mode ? tlm::TLM_ADDRESS_ERROR_RESPONSE : tlm::TLM_OK_RESPONSE);
  }
};

// The perf device (rtl/cheriot_rtos_perf_dev.sv, whose header is its specification; the SoC's
// performance tests, sw/tests/perf_test, not Sonata or OpenTitan): a stand-in for a hash engine's
// message FIFO, and a DMA that can feed it. Its registers, word offsets in a 4 KiB window:
//   0x00 FIFO       W: absorb a word (COUNT + 1, CHECKSUM = rotl(CHECKSUM, 1) ^ word)
//   0x04 COUNT      R: words absorbed; W: clear COUNT and CHECKSUM
//   0x08 CHECKSUM   R
//   0x10 DMA_SRC    RW byte address, word aligned; increments
//   0x14 DMA_DST    RW byte address, word aligned; increments unless DMA_CTRL.dst_fixed
//   0x18 DMA_LEN    RW bytes, a multiple of 4
//   0x1c DMA_CTRL   W: bit 0 start (ignored while busy), bit 1 dst_fixed
//   0x20 DMA_STATUS R: bit 0 busy, bit 1 done, bit 2 error; W: 1 to bit 1 clears done
//   0x24 DMA_WORDS  R: words moved by the last transfer
//   0x28 DMA_CYCLES R: cycles the last transfer was busy, from start to done
// The DMA moves one word at a time, a read then a write, through its own bus host (dma_read,
// dma_write: the data host's devices). done stays set until cleared and is the interrupt (PLIC
// source 30, the perf board's PerfDmaInterrupt). A bus error stops the transfer with done and
// error set. Where the header is silent: an access that is not a whole word, or to an offset with
// no register, answers an error (the RTL's register adapter takes any offset; the firmware makes
// none). The model moves the whole transfer at its end, two cycles a word (the core sleeps on the
// interrupt meanwhile); it does not time the memories, so DMA_CYCLES says nothing about the RTL's.
class RtosPerfDev : public sc_core::sc_module {
 public:
  static constexpr uint32_t kBase = 0x80500000, kSize = 0x1000;
  static constexpr uint16_t kPlicSource = 30;
  tlm_utils::simple_target_socket<RtosPerfDev> socket;
  std::function<bool(uint32_t addr, uint32_t *word)> dma_read;
  std::function<bool(uint32_t addr, uint32_t word)> dma_write;

  explicit RtosPerfDev(sc_core::sc_module_name n) : sc_core::sc_module(n), socket("socket") {
    socket.register_b_transport(this, &RtosPerfDev::b_transport);
  }

 private:
  void absorb(uint32_t w) {
    ++count_;
    checksum_ = ((checksum_ << 1) | (checksum_ >> 31)) ^ w;
  }
  void b_transport(tlm::tlm_generic_payload &t, sc_core::sc_time &) {
    uint32_t off = uint32_t(t.get_address()), v = 0;
    bool ok = t.get_data_length() == 4 && !(off & 3) && !t.get_byte_enable_ptr();
    if (ok && t.is_read()) {
      switch (off) {
        case 0x04: v = count_; break;
        case 0x08: v = checksum_; break;
        case 0x10: v = src_; break;
        case 0x14: v = dst_; break;
        case 0x18: v = len_; break;
        case 0x20: v = (busy_ ? 1u : 0u) | (done_ ? 2u : 0u) | (err_ ? 4u : 0u); break;
        case 0x24: v = words_; break;
        case 0x28: v = cycles_; break;
        case 0x00: case 0x1c: break;
        default: ok = false;
      }
      if (ok) std::memcpy(t.get_data_ptr(), &v, 4);
    } else if (ok) {
      std::memcpy(&v, t.get_data_ptr(), 4);
      switch (off) {
        case 0x00: absorb(v); break;
        case 0x04: count_ = checksum_ = 0; break;
        case 0x10: src_ = v; break;
        case 0x14: dst_ = v; break;
        case 0x18: len_ = v; break;
        case 0x1c: if ((v & 1) && !busy_) start(v & 2); break;
        case 0x20: if (v & 2) set_done(false); break;
        case 0x08: case 0x24: case 0x28: break;
        default: ok = false;
      }
    }
    t.set_response_status(ok ? tlm::TLM_OK_RESPONSE : tlm::TLM_ADDRESS_ERROR_RESPONSE);
  }
  void start(bool dst_fixed) {
    Eg2System &sys = Eg2System::get();
    busy_ = true;
    err_ = false;
    set_done(false);
    uint32_t n = len_ / 4;
    uint64_t c0 = sys.clock_cycles(kClk, sys.now_ps());
    sys.at(sys.clock_time_of_cycle(kClk, c0 + 2 * uint64_t(n ? n : 1)), [this, n, dst_fixed] {
      uint32_t moved = 0;
      for (; moved < n; ++moved) {
        uint32_t w = 0, d = dst_ + (dst_fixed ? 0 : 4 * moved);
        if (!dma_read || !dma_read(src_ + 4 * moved, &w)) break;
        if (d >= kBase && d < kBase + kSize) {
          if (d - kBase != 0) break;  // only the FIFO takes a DMA write
          absorb(w);
        } else if (!dma_write || !dma_write(d, w)) {
          break;
        }
      }
      err_ = moved < n;
      words_ = moved;
      cycles_ = 2 * (n ? n : 1);
      busy_ = false;
      set_done(true);
    });
  }
  void set_done(bool d) {
    done_ = d;
    Eg2System::get().set_plic_source(kPlicSource, d);
  }
  uint32_t count_ = 0, checksum_ = 0, src_ = 0, dst_ = 0, len_ = 0, words_ = 0, cycles_ = 0;
  bool busy_ = false, done_ = false, err_ = false;
};

// The debug module's system bus access (SBA) as the testbench drives it for sw/tests/dbg_host_test
// (dv/tb/cheriot_rtos_dbg_host.sv, whose header is its specification; the SoC has the debug
// module but the model does not: this plays both). The firmware prints "DBGHOST REQ <op> <slot>
// <value> <flag>" (RtosRunMonitor hands it here); then:
//   op 1 (tag clear): debug access enabled; an SBA write of <value> to the word at <slot>.
//   op 2 (locked):    debug access disabled; the same write, which the SoC's debug-access gate
//                     (rtl/cheriot_rtos_dbg_gate.sv, OpenTitan rv_dm's tlul_lc_gate) refuses
//                     with a bus error, sbcs.sberror = 2.
// Then, with debug access enabled, an SBA read of <slot> and an SBA write of
//   0xDB60_0000 | op << 12 | sberror << 8 | readback << 4 | 1
// to <flag>, readback 1 if the read returned <value>. SBA reaches the SRAM and the code RAM
// (rtl/cheriot_rtos_xbar.sv), any other address answers an error; and its writes do not pass the
// memory subsystem's tag filter, so they leave the capability tags as they were (opentitan
// hw/ip/cheriot_mem_sys/doc/theory_of_operation.md, "System Bus Access": debug accesses in CHERIoT
// mode are to go through the core). gate_open is the SoC's dbg_lock_open mutation: the gate lets
// every SBA access through.
class RtosDbgHost {
 public:
  std::vector<Eg2Memory *> mems;
  bool gate_open = false;

  void serve(uint32_t op, uint32_t slot, uint32_t value, uint32_t flag) {
    bool enabled = op == 1 || gate_open;
    uint32_t sberror = enabled && write(slot, value) ? 0 : 2, back = 0;
    bool readback = read(slot, &back) && back == value;
    uint32_t done = 0xdb600000u | (op & 0xf) << 12 | sberror << 8 | (readback ? 1u : 0u) << 4 | 1u;
    bool flagged = write(flag, done);
    std::printf("eg2sim: [cheriot_rtos_dbg_host] op %u: SBA write of 0x%08x to 0x%08x with debug "
                "access %s: sberror %u, readback %u; flag 0x%08x to 0x%08x%s\n",
                op, value, slot, enabled ? "enabled" : "disabled", sberror, readback ? 1 : 0,
                done, flag, flagged ? "" : " FAILED (not in a memory)");
  }

 private:
  bool write(uint32_t a, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    for (Eg2Memory *m : mems) {
      if (!(a & 3) && m->write(a, b, 4)) return true;
    }
    return false;
  }
  bool read(uint32_t a, uint32_t *v) {
    for (Eg2Memory *m : mems) {
      if (!(a & 3) && a >= m->base() && uint64_t(a - m->base()) + 4 <= m->size()) {
        std::memcpy(v, &m->data()[a - m->base()], 4);
        return true;
      }
    }
    return false;
  }
};

// The testbench's part (dv/tb/cheriot_rtos_tb.sv), on the bytes UART0 sends: uartdpi's log (every
// byte, raw, to uart0.log) and its exit string (the RTOS's "Safe to exit simulator." and eight
// bytes, platform-simulation_exit.hh), which ends the run; the line watch: a line with "Test failure
// in test runner" or "One or more tests failed" ends the run 200 character times later (FIRMWARE
// FAILURE), "All tests finished" ends it 1,000,000 cycles later unless a failure line comes first
// (FIRMWARE DONE: a build without the exit string); no byte for idle_cycles after the first ends it
// (UART IDLE); and max_cycles ends it (TIMEOUT). A line with "DBGHOST REQ <op> <slot> <value>
// <flag>" (sw/tests/dbg_host_test) goes to dbg_host kDbgHostCycles later, as the testbench serves
// it over the debug module (RtosDbgHost). Its lines are the testbench's, so that
// cheriot_rtos_verdict.sh reads a model run as it reads a simulation.
class RtosRunMonitor {
 public:
  uint64_t max_cycles = 4800000000ull, idle_cycles = 200000000ull;
  static constexpr uint64_t kDoneWaitCycles = 1000000, kFailWaitCycles = 43 * 200;
  // The testbench's DMI and SBA accesses take a few thousand cycles; the firmware polls its flag
  // once a scheduler tick.
  static constexpr uint64_t kDbgHostCycles = 2000;
  std::function<void(uint32_t op, uint32_t slot, uint32_t value, uint32_t flag)> dbg_host;

  RtosRunMonitor() : sys_(Eg2System::get()) {}
  ~RtosRunMonitor() { close(); }

  bool open(const std::string &path) {
    log_ = std::fopen(path.c_str(), "wb");
    return log_ != nullptr;
  }
  void close() {
    if (log_) std::fclose(log_);
    log_ = nullptr;
  }
  void start() {
    sys_.at(time_of(max_cycles), [this] {
      line("TIMEOUT after " + std::to_string(max_cycles) + " cycles");
      end(false, "cycle limit reached");
    });
  }
  bool exited() const { return exited_; }
  uint64_t bytes() const { return bytes_; }

  void byte(uint8_t b) {
    if (log_) {
      std::fputc(b, log_);
      if (b == '\n') std::fflush(log_);
    }
    ++bytes_;
    last_ = sys_.now_ps();
    if (!idle_armed_ && idle_cycles) arm_idle(last_);
    tail_ += char(b);
    if (tail_.size() > exit_string().size()) tail_.erase(0, tail_.size() - exit_string().size());
    if (tail_ == exit_string()) {
      std::printf("eg2sim: the firmware sent the exit string\n");
      exited_ = true;
      end(true, "exit string");
      return;
    }
    if (b == '\n' || b == '\r') {
      for (const char *f : {"Test failure in test runner", "One or more tests failed"}) {
        if (cur_.find(f) != std::string::npos && !failing_) {
          failing_ = true;
          line("FIRMWARE FAILURE at cycle " + std::to_string(cycle()) + ": \"" + cur_ + "\"");
          sys_.at(time_of(cycle() + kFailWaitCycles), [this] { end(false, "firmware failure"); });
        }
      }
      size_t req = cur_.find("DBGHOST REQ ");
      if (req != std::string::npos && dbg_host) {
        const char *p = cur_.c_str() + req + 12;
        char *e = nullptr;
        uint32_t f[4] = {};
        int n = 0;
        for (; n < 4; ++n, p = e) {
          f[n] = uint32_t(std::strtoul(p, &e, 0));
          if (e == p) break;
        }
        if (n == 4) {
          line("debug host request at cycle " + std::to_string(cycle()) + ": \"" +
               cur_.substr(req) + "\"");
          sys_.at(time_of(cycle() + kDbgHostCycles), [this, f] { dbg_host(f[0], f[1], f[2], f[3]); });
        }
      }
      if (cur_.find("All tests finished") != std::string::npos && !done_seen_) {
        done_seen_ = true;
        sys_.at(time_of(cycle() + kDoneWaitCycles), [this] {
          line("FIRMWARE DONE at cycle " + std::to_string(cycle()) +
               ": \"All tests finished\" and no failure line followed (no exit string: a "
               "non-SIMULATION build)");
          end(true, "firmware done");
        });
      }
      cur_.clear();
    } else if (b >= 0x20 && b < 0x7f) {
      cur_ += char(b);
    }
  }

 private:
  static const std::string &exit_string() {
    static const std::string s = std::string("Safe to exit simulator.") +
                                 "\xd8\xaf\xfb\xa0\xc7\xe1\xa9\xd7";
    return s;
  }
  uint64_t cycle() const { return sys_.clock_cycles(kClk, sys_.now_ps()); }
  uint64_t time_of(uint64_t c) const { return sys_.clock_time_of_cycle(kClk, c); }
  void arm_idle(uint64_t from) {
    idle_armed_ = true;
    sys_.at(time_of(sys_.clock_cycles(kClk, from) + idle_cycles), [this] {
      idle_armed_ = false;
      if (sys_.clock_cycles(kClk, sys_.now_ps()) - sys_.clock_cycles(kClk, last_) >= idle_cycles) {
        line("UART IDLE: no output for " + std::to_string(idle_cycles) + " cycles (last byte at "
             "cycle " + std::to_string(sys_.clock_cycles(kClk, last_)) + ")");
        end(false, "UART idle");
      } else {
        arm_idle(last_);
      }
    });
  }
  void line(const std::string &s) {
    std::printf("eg2sim: [cheriot_rtos_tb] %s\n", s.c_str());
    std::fflush(stdout);
  }
  void end(bool ok, const std::string &why) { sys_.finish(ok, why); }

  Eg2System &sys_;
  FILE *log_ = nullptr;
  std::string tail_, cur_;
  uint64_t bytes_ = 0, last_ = 0;
  bool idle_armed_ = false, failing_ = false, done_seen_ = false, exited_ = false;
};

}  // namespace rtos_soc

#endif  // EG2_MODEL_RTOS_SOC_RTOS_SOC_H_
