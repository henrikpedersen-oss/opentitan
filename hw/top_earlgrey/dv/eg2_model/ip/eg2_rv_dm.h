// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rv_dm's debug module, and the debugger outside the chip that drives it, in the EG2 SystemC model.
//
// Eg2RvDm (window rv_dm.regs; it also keeps rv_dm.mem's program buffer and data registers): the
// Debug Module Registers of the RISC-V Debug Specification 0.13.2 (section 3.12) that the PULP
// debug module implements (hw/vendor/pulp_riscv_dbg/doc/debug-system.md "Debug Module Registers"),
// reached over the Debug Module Interface (DMI) only, never over TL-UL (rv_dm
// theory_of_operation.md). One hart; halt summary and hart array masks as debug-system.md says:
//   dmcontrol   dmactive (0 resets the debug module: every other register takes its reset value and
//               only dmactive can be written), ndmreset, haltreq (a level: rv_dm's debug_req_o, the
//               core enters debug mode), resumereq (the halted core resumes; ignored while haltreq is
//               set), ackhavereset; hartsel has no bits (one hart: written ones read back 0, spec
//               3.12.2 WARL), hartreset and the reset-halt requests do nothing (debug-system.md).
//   dmstatus    version 2 (0.13), authenticated, impebreak 0, allhalted/anyhalted (the core is in
//               debug mode), allrunning/anyrunning, allresumeack/anyresumeack (set when a resume
//               request took the core out of debug mode), allhavereset/anyhavereset (set by every
//               reset of the core, cleared by ackhavereset), unavail never set (rv_dm's unavailable
//               input is not connected in top_earlgrey.hjson).
//   hartinfo    nscratch 2, dataaccess 1, datasize 2, dataaddr 0x380 (debug-system.md "hartinfo";
//               datasize from rv_dm.hjson DATAADDR count 2).
//   abstractcs  progbufsize 8 (rv_dm.hjson PROGRAM_BUFFER count 8), datacount 2, busy 0 (a command
//               completes at once here), cmderr (write 1 to clear).
//   command     Access Register (cmdtype 0) of 32 bits: the transfer between data0 and the
//               register (Eg2DebugHart::access_register), then the program buffer if postexec; the
//               core must be halted (cmderr 4 otherwise); aarsize other than 32 bits is cmderr 2;
//               aarpostincrement adds 1 to regno. Other command types are cmderr 2: debug-system.md
//               lists no Quick Access or Access Memory. A command written while cmderr is set is
//               ignored. abstractauto re-runs the command on an access to data0-1 or progbuf0-7.
//   data0-1, progbuf0-7   rv_dm.mem's DATAADDR_0-1 and PROGRAM_BUFFER_0-7: the same storage the
//               core reaches in the debug memory (rv_dm.hjson, PULP "Debug Memory Map").
//   sbcs, sbaddress0, sbdata0, haltsum0   System Bus Access (spec 3.12.18-3.12.23): 8-, 16- and
//               32-bit accesses on the system bus (Eg2System::sba), sbreadonaddr, sbreadondata,
//               sbautoincrement, sberror (3 misaligned, 4 size not supported, 2 an error response:
//               rv_dm's interfaces.md says bus errors go to the status, not which code), sbbusyerror
//               never set (an access completes at once). debug-system.md does not list the access
//               sizes; the model takes those up to its 32-bit BusWidth.
// The core waits in debug mode at rv_dm's halt address and the program buffer runs from rv_dm.mem
// offset 0x360 (eg2_spike_cpu.h); the debug ROM's own code and the HALTED/GOING/RESUMING/FLAGS
// handshake are rv_dm's implementation, which the model replaces by the core's debug interface.
//
// Life cycle (rv_dm theory_of_operation.md "Late Debug Enable Mechanism", the truth table): the TAP
// reaches the debug module in TEST_UNLOCKED*, DEV and RMA (lc_ctrl's HW_DEBUG_EN); it is fully
// ungated in TEST_UNLOCKED* and RMA, and in DEV only if OTP's DIS_RV_DM_LATE_DEBUG is kMuBi8True
// (HW_CFG1, otp_ctrl_mmap.md offset 0x6D1) or LATE_DEBUG_ENABLE holds kMuBi32True. rv_dm.hjson
// DM_EN.CTRL.LC_GATED says this control gates "the DMI attached to the TAP" as well as debug_req,
// NDM reset and SBA. The model therefore rejects every DMI access while gated. The documents say
// neither what value a debugger observes from an isolated/gated DMI nor how its transport reports
// the rejection; returning false and a zero read value are model assumptions. The pinmux strap/TAP
// selection is not modelled: assuming that a reachable life-cycle state selected the RISC-V TAP.
//
// Resets: rv_dm is on the sys reset tree (top_earlgrey.hjson rst_ni: sys). Pinmux
// theory_of_operation.md "Non-debug Module Reset" says rv_dm remains active while all modules
// outside the reset, power and clock managers reset: through NDM reset rv_dm keeps its state and
// records that the core reset (havereset). Every other reset (and a deep-sleep exit:
// rv_dm is in the main power domain) resets it. ndmreset 1 requests the NDM reset and holds the
// chip in it (the core does not run) until the debugger writes 0 (spec 3.12.2 ndmreset: "the
// debugger writes 1, and then writes 0 to deassert the reset").
//
// Eg2DebugHost: the debugger, as eg2sim's --stim drives it (the chip bench's JTAG agent and
// jtag_rv_debugger, OpenOCD with opentitanlib on FPGA and silicon). Each key is one debugger action
// at the stimulus's time (--stim debugger.<key>=<value>@log:<message>+<delay>), through the DMI as a
// debugger does it; a check that fails ends the run as failed, as the bench's DV_CHECK does. Numbers
// are decimal or 0x hexadecimal; <regno> is an abstract command register number (0x1000 + n: GPR xn;
// a CSR number); parts of a value are separated by ";" (a "," makes eg2sim's value list). Each
// action prints "eg2sim: debugger: ...".
//   debugger.dmi_write=<addr>:<value>         a DMI register write
//   debugger.dmi_expect=<addr>:<value>[/<m>]  a DMI register read, compared under mask m
//   debugger.dmactive=<0|1>, debugger.haltreq=<0|1>, debugger.resumereq=1, debugger.ackhavereset=1,
//   debugger.ndmreset=<0|1>                   dmcontrol, with dmactive and the other fields as last
//                                             written (jtag_rv_debugger set_dmactive, set_haltreq...)
//   debugger.reg_read=<regno>[:<count>]       Access Register reads of count registers from regno,
//                                             logged; cmderr other than 0 fails the run
//   debugger.reg_write=<regno>:<value>        an Access Register write
//   debugger.reg_expect=<regno>:<value>[/<m>] an Access Register read, compared under mask m
//   debugger.progbuf=<w0>[;<w1>...]           the program buffer, from progbuf0
//   debugger.exec_progbuf=1                   a command with postexec only; cmderr fails the run
//   debugger.sba_write=<addr>:<value>, debugger.sba_read=<addr>, debugger.sba_expect=<addr>:<value>[/<m>]
//                                             a 32-bit System Bus Access; <addr> is a number or
//                                             <window>.<REGISTER> (lc_ctrl.regs.DEVICE_ID_0)
//   debugger.sba_expect_otp=<addr>:<OTP offset>[:<words>]
//                                             SBA reads compared with the OTP array, as the bench
//                                             compares them with OTP read through its backdoor
//   debugger.sram_program=<elf>[;wait[;<regno>:<value>...]]
//       opentitanlib's load_and_execute_sram_program (sw/host/opentitanlib/src/test_utils/
//       load_sram_program.rs) on the halted core: each loadable segment written over SBA (word
//       aligned, no gaps), the text section holding the entry point read back and compared, the
//       CRC-32 (ISO-HDLC) of the segments; prepare_epmp (pmpcfg3 region 15 "L NAPOT X W R" over
//       the main SRAM, pmpcfg2 region 11 "L TOR X W R" over the MMIO range in pmpaddr10-11);
//       prepare_sram_ctrl (sram_ctrl_main EXEC = MuBi4True over SBA); ra = 0xdeadbeef, a0 = the
//       CRC; then OpenOCD's resume at the entry point: dpc = entry, fence.i through the program
//       buffer (opentitanlib: "OpenOCD takes care of invalidating the cache"), haltreq 0,
//       resumereq. "wait" is ExecutionMode::JumpAndWait: dcsr.ebreakm is set first, as OpenOCD
//       does by default (riscv set_ebreakm), so the program's final ebreak halts the core (without
//       "wait" dcsr stays as it is: the harness then disconnects OpenOCD, and no document says what
//       dcsr holds after that); the debugger polls dmstatus every 10 us until the core halts, then
//       requires sp == SRAM_MAGIC_SP_EXECUTION_DONE (0xcafebabe, sram_start.h) and each
//       <regno>:<value> given (a host harness's own check, e.g. the ISA test's a0 == 1), and ends
//       the run with that verdict.

#ifndef EG2_MODEL_IP_EG2_RV_DM_H_
#define EG2_MODEL_IP_EG2_RV_DM_H_

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_mem_otp.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

namespace eg2_model {

class Eg2RvDm;

// The debugger outside the chip (above).
class Eg2DebugHost {
 public:
  explicit Eg2DebugHost(Eg2RvDm &dm);

 private:
  bool dmcontrol(uint32_t set, uint32_t clear);
  bool command(uint32_t cmd, const std::string &what);
  bool reg_rw(uint32_t regno, bool write, uint32_t *value);
  bool sba_rw(bool write, uint32_t addr, uint32_t *value);
  bool addr_of(const std::string &s, uint32_t *addr);
  bool expect(const std::string &what, uint32_t got, uint32_t want, uint32_t mask);
  bool fail(const std::string &why);
  bool sram_program(const std::string &path, bool wait);
  void poll_halt(uint64_t t_ps);
  bool halted();
  void log(const std::string &s) { std::printf("eg2sim: debugger: %s\n", s.c_str()); }

  Eg2RvDm &dm_;
  uint32_t dmcontrol_ = 0;  // dmcontrol as this debugger last wrote it
  // JumpAndWait: the registers to check when the core halts.
  std::vector<std::pair<uint32_t, uint32_t>> wait_checks_;
  bool waiting_ = false;
};

class Eg2RvDm : public Eg2IpModel {
 public:
  // DMI register addresses (spec 3.12; debug-system.md "Debug Module Registers").
  enum : uint32_t {
    kData0 = 0x04, kDmcontrol = 0x10, kDmstatus = 0x11, kHartinfo = 0x12, kAbstractcs = 0x16,
    kCommand = 0x17, kAbstractauto = 0x18, kProgbuf0 = 0x20, kSbcs = 0x38, kSbaddress0 = 0x39,
    kSbdata0 = 0x3c, kHaltsum0 = 0x40
  };
  static constexpr unsigned kDataCount = 2, kProgbufSize = 8;
  static constexpr uint32_t kDisRvDmLateDebug = 0x6D1;  // OTP HW_CFG1 (otp_ctrl_mmap.md)

  Eg2RvDm(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    late_en_ = reg("LATE_DEBUG_ENABLE");
    host_ = std::make_unique<Eg2DebugHost>(*this);
  }

  // ---- The DMI (the debugger's side) ----
  // A read: false if life cycle gates the DMI (the value is then 0 by the assumption above).
  bool dmi_read(uint32_t addr, uint32_t *v) {
    *v = 0;
    if (!dmi_enabled()) return false;
    update_req();
    if (addr >= kData0 && addr < kData0 + kDataCount) {
      *v = mem_get("DATAADDR_" + std::to_string(addr - kData0));
      if (abstract_busy_) set_busy_error();
      else autoexec_access(addr);
    } else if (addr >= kProgbuf0 && addr < kProgbuf0 + kProgbufSize) {
      *v = mem_get("PROGRAM_BUFFER_" + std::to_string(addr - kProgbuf0));
      if (abstract_busy_) set_busy_error();
      else autoexec_access(addr);
    } else if (addr == kDmcontrol) {
      *v = (ndmreset_ << 1) | uint32_t(dmactive_);  // haltreq is WARZ (debug-system.md)
    } else if (addr == kDmstatus) {
      // Debug Spec 0.13.2 dmstatus: running means "not halted and not unavailable". Earl Grey's
      // top_earlgrey.gen.hjson leaves rv_dm.unavailable at its default 0, including during NDM
      // reset, so the DM continues to report the selected hart as running while reset is held.
      bool h = hart_halted(), run = !h;
      *v = (uint32_t(havereset_) * 0x3u << 18) | (uint32_t(resumeack_) * 0x3u << 16) |
           (uint32_t(run) * 0x3u << 10) | (uint32_t(h) * 0x3u << 8) | (1u << 7) | 2u;
    } else if (addr == kHartinfo) {
      *v = (2u << 20) | (1u << 16) | (kDataCount << 12) | 0x380u;
    } else if (addr == kAbstractcs) {
      *v = (kProgbufSize << 24) | (uint32_t(abstract_busy_) << 12) | (cmderr_ << 8) | kDataCount;
    } else if (addr == kCommand) {
      *v = 0;  // write-only (spec 3.12.7: reads return 0)
    } else if (addr == kAbstractauto) {
      *v = abstractauto_;
    } else if (addr == kSbcs) {
      *v = (1u << 29) | (sbreadonaddr_ << 20) | (sbaccess_ << 17) | (sbautoinc_ << 16) |
           (sbreadondata_ << 15) | (sberror_ << 12) | (32u << 5) | 0x7u;
    } else if (addr == kSbaddress0) {
      *v = sbaddress_;
    } else if (addr == kSbdata0) {
      *v = sbdata_;
      if (dmactive_ && sbreadondata_ && !sberror_) sba_access(false);
    } else if (addr == kHaltsum0) {
      *v = hart_halted() ? 1u : 0u;
    }
    return true;
  }

  // A write: false if life cycle gates the DMI (nothing happens).
  bool dmi_write(uint32_t addr, uint32_t v) {
    if (!dmi_enabled()) return false;
    if (addr == kDmcontrol) {
      write_dmcontrol(v);
    } else if (!dmactive_) {
      // While dmactive is 0 the debug module is held in reset: only dmactive can be written.
    } else if (addr >= kData0 && addr < kData0 + kDataCount) {
      if (abstract_busy_) set_busy_error();
      else {
        mem_set("DATAADDR_" + std::to_string(addr - kData0), v);
        autoexec_access(addr);
      }
    } else if (addr >= kProgbuf0 && addr < kProgbuf0 + kProgbufSize) {
      if (abstract_busy_) set_busy_error();
      else {
        mem_set("PROGRAM_BUFFER_" + std::to_string(addr - kProgbuf0), v);
        autoexec_access(addr);
      }
    } else if (addr == kAbstractcs) {
      cmderr_ &= ~((v >> 8) & 7);
    } else if (addr == kCommand) {
      last_cmd_ = v;
      // Debug Spec 0.13.2 abstractcs.cmderr: a command written while busy sets busy only when
      // cmderr is 0; no abstract-command error overwrites an earlier one.
      if (abstract_busy_) set_busy_error();
      else if (!cmderr_) run_command(v);
    } else if (addr == kAbstractauto) {
      abstractauto_ = v & ((((1u << kProgbufSize) - 1) << 16) | ((1u << kDataCount) - 1));
    } else if (addr == kSbcs) {
      sberror_ &= ~((v >> 12) & 7);
      sbreadonaddr_ = (v >> 20) & 1;
      sbaccess_ = (v >> 17) & 7;
      sbautoinc_ = (v >> 16) & 1;
      sbreadondata_ = (v >> 15) & 1;
    } else if (addr == kSbaddress0) {
      sbaddress_ = v;
      if (sbreadonaddr_ && !sberror_) sba_access(false);
    } else if (addr == kSbdata0) {
      sbdata_ = v;
      if (!sberror_) sba_access(true);
    }
    update_req();
    return true;
  }

  // ---- Life cycle gating ----
  bool tap_reachable() const {
    const Eg2LcCtrl *lc = lc_ctrl();
    return lc && lc->hw_debug_en();
  }
  bool dmi_enabled() const { return tap_reachable() && fully_enabled(); }
  bool fully_enabled() const {
    const Eg2LcCtrl *lc = lc_ctrl();
    if (!lc || !lc->hw_debug_en()) return false;
    if (lc->dft_en()) return true;  // TEST_UNLOCKED*, RMA
    if (dis_late_debug_) return true;
    return value(late_en_) == 0x96969696u;                       // kMuBi32True
  }
  bool hart_halted() const { return sys_.hart && sys_.hart->debug_mode(); }
  Eg2System &sys() { return sys_; }

  void start_of_simulation() override { latch_otp(); }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    havereset_ = true;  // every reset of the chip resets the core
    resumeack_ = false;
    if (c != Eg2ResetCause::kNdm) {
      dm_reset();
      latch_otp();
    }
    update_req();
  }

 protected:
  void reg_write(uint16_t, uint32_t, uint32_t, uint32_t) override { update_req(); }

 private:
  const Eg2LcCtrl *lc_ctrl() const {
    return dynamic_cast<const Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
  }
  Eg2IpModel *mem() const { return sys_.model("rv_dm.mem"); }
  void latch_otp() {
    // otp_ctrl_mmap.hjson HW_CFG1 has variant "Buffered" and contains DIS_RV_DM_LATE_DEBUG;
    // rv_dm theory_of_operation.md "Late Debug Enable Mechanism" samples that value at reset.
    auto *o = dynamic_cast<Eg2OtpCtrl *>(sys_.model("otp_ctrl.core"));
    dis_late_debug_ = o && o->byte(kDisRvDmLateDebug) == 0x96;  // kMuBi8True
  }
  uint32_t mem_get(const std::string &r) const {
    Eg2IpModel *m = mem();
    int i = m ? m->reg_index(r) : -1;
    return i >= 0 ? m->value(uint16_t(i)) : 0;
  }
  void mem_set(const std::string &r, uint32_t v) {
    Eg2IpModel *m = mem();
    int i = m ? m->reg_index(r) : -1;
    if (i < 0) return;
    m->set_value(uint16_t(i), v);
    m->set_modelled(uint16_t(i));
  }

  // The debug module's reset (dmactive 0, or a reset of the sys tree): its registers take their
  // reset values; the program buffer and data registers are rv_dm.mem's, reset with that block.
  void dm_reset() {
    dmactive_ = false;
    haltreq_ = ndmreset_ = 0;
    cmderr_ = 0;
    abstract_busy_ = false;
    abstractauto_ = last_cmd_ = 0;
    sbreadonaddr_ = sbautoinc_ = sbreadondata_ = sberror_ = 0;
    sbaccess_ = 2;
    sbaddress_ = sbdata_ = 0;
    sys_.release_core("rv_dm");
  }

  void write_dmcontrol(uint32_t v) {
    bool active = v & 1;
    if (!active) {
      if (dmactive_) {
        for (unsigned i = 0; i < kProgbufSize; ++i) mem_set("PROGRAM_BUFFER_" + std::to_string(i), 0);
        for (unsigned i = 0; i < kDataCount; ++i) mem_set("DATAADDR_" + std::to_string(i), 0);
      }
      dm_reset();
      return;
    }
    bool was_active = dmactive_;
    dmactive_ = true;
    if (!was_active) return;  // the write that activates the module sets only dmactive
    haltreq_ = v >> 31;
    if ((v >> 28) & 1) havereset_ = false;  // ackhavereset
    uint32_t ndm = (v >> 1) & 1;
    if (ndm != ndmreset_) {
      ndmreset_ = ndm;
      if (fully_enabled()) {
        if (ndm) {
          sys_.request_reset(Eg2ResetCause::kNdm, "rv_dm ndmreset (non-debug-module reset)");
          sys_.hold_core("rv_dm", "ndmreset asserted: the chip is held in the NDM reset");
        } else {
          sys_.release_core("rv_dm");
        }
      }
    }
    // resumereq: the halted core resumes once; ignored while haltreq is set (spec 3.12.2).
    if (((v >> 30) & 1) && !haltreq_) {
      resumeack_ = false;
      if (hart_halted() && fully_enabled()) {
        sys_.hart->resume();
        resumeack_ = true;
      }
    }
  }

  // rv_dm's debug_req_o: haltreq, while the module is active and debug is enabled.
  void update_req() { sys_.debug_req = dmactive_ && haltreq_ && fully_enabled() && sys_.hart; }

  void run_command(uint32_t cmd) {
    uint32_t type = cmd >> 24;
    if (type != 0) {
      cmderr_ = 2;  // Quick Access, Access Memory: not supported
      return;
    }
    if (!hart_halted() || !fully_enabled()) {
      cmderr_ = 4;  // halt/resume: the hart is not halted
      return;
    }
    uint32_t size = (cmd >> 20) & 7, regno = cmd & 0xffff;
    bool postinc = (cmd >> 19) & 1, postexec = (cmd >> 18) & 1, transfer = (cmd >> 17) & 1,
         write = (cmd >> 16) & 1;
    if (transfer) {
      if (size != 2) {
        cmderr_ = 2;  // only 32-bit register access
        return;
      }
      uint32_t data = mem_get("DATAADDR_0");
      int err = sys_.hart->access_register(regno, write, &data);
      if (err) {
        cmderr_ = uint32_t(err);
        return;
      }
      if (!write) mem_set("DATAADDR_0", data);
    }
    if (postexec) {
      int err = sys_.hart->exec_progbuf(kProgbufAddr);
      if (err == 1) {
        // Debug Spec 0.13.2 Program Buffer: if execution does not terminate, busy stays set.
        // The finite instruction guard used to identify that condition is a model assumption.
        abstract_busy_ = true;
        return;
      }
      if (err) {
        cmderr_ = uint32_t(err);
        return;
      }
    }
    if (postinc && transfer) last_cmd_ = (cmd & ~0xffffu) | ((regno + 1) & 0xffff);
  }
  // The program buffer in the debug memory (rv_dm.hjson PROGRAM_BUFFER at 0x360).
  static constexpr uint32_t kProgbufAddr = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR + 0x360;
  void autoexec_access(uint32_t addr) {
    if (!dmactive_ || cmderr_ || abstract_busy_) return;
    bool data = addr >= kData0 && addr < kData0 + kDataCount &&
                ((abstractauto_ >> (addr - kData0)) & 1);
    bool pb = addr >= kProgbuf0 && addr < kProgbuf0 + kProgbufSize &&
              ((abstractauto_ >> (16 + addr - kProgbuf0)) & 1);
    if (data || pb) run_command(last_cmd_);
  }

  // Debug Spec 0.13.2 abstractcs.cmderr: busy is written only if cmderr currently contains 0.
  void set_busy_error() {
    if (!cmderr_) cmderr_ = 1;
  }

  void sba_access(bool write) {
    if (!fully_enabled()) {
      sberror_ = 7;
      return;
    }
    // top_earlgrey xbar_main.hjson connects rv_dm.sba to every target in CHERIoT mode. Its
    // "not supported" warning is about writes bypassing the SRAM capability-tag filter, not a
    // bus error; ordinary SBA reads and writes therefore still reach the selected target.
    if (sbaccess_ > 2) {
      sberror_ = 4;  // size not supported
      return;
    }
    uint32_t n = 1u << sbaccess_;
    if (sbaddress_ & (n - 1)) {
      sberror_ = 3;  // alignment
      return;
    }
    uint8_t b[4] = {uint8_t(sbdata_), uint8_t(sbdata_ >> 8), uint8_t(sbdata_ >> 16),
                    uint8_t(sbdata_ >> 24)};
    if (!sys_.sba || !sys_.sba(write, sbaddress_, n, b)) {
      sberror_ = 2;
      return;
    }
    if (!write) {
      sbdata_ = 0;
      for (unsigned i = 0; i < n; ++i) sbdata_ |= uint32_t(b[i]) << (8 * i);
    }
    if (sbautoinc_) sbaddress_ += n;
  }

  uint16_t late_en_;
  bool dmactive_ = false, havereset_ = true, resumeack_ = false;
  bool abstract_busy_ = false, dis_late_debug_ = false;
  uint32_t haltreq_ = 0, ndmreset_ = 0, cmderr_ = 0, abstractauto_ = 0, last_cmd_ = 0;
  uint32_t sbreadonaddr_ = 0, sbaccess_ = 2, sbautoinc_ = 0, sbreadondata_ = 0, sberror_ = 0;
  uint32_t sbaddress_ = 0, sbdata_ = 0;
  std::unique_ptr<Eg2DebugHost> host_;
};

// ---- The debugger ----

namespace eg2_rv_dm_detail {
// CSR numbers (RISC-V privileged specification; Debug Specification 4.8 for dcsr and dpc).
enum : uint32_t {
  kPmpcfg2 = 0x3a2, kPmpcfg3 = 0x3a3, kPmpaddr10 = 0x3ba, kPmpaddr11 = 0x3bb, kPmpaddr15 = 0x3bf,
  kDcsr = 0x7b0, kDpc = 0x7b1
};
inline uint32_t num(const std::string &s) { return uint32_t(std::stoul(s, nullptr, 0)); }
// "<a>:<b>[/<mask>]"
inline bool pair(const std::string &v, uint32_t *a, uint32_t *b, uint32_t *mask = nullptr) {
  size_t c = v.find(':');
  if (c == std::string::npos) return false;
  size_t sl = v.find('/', c);
  *a = num(v.substr(0, c));
  *b = num(v.substr(c + 1, sl == std::string::npos ? std::string::npos : sl - c - 1));
  if (mask) *mask = sl == std::string::npos ? 0xffffffffu : num(v.substr(sl + 1));
  return true;
}
inline std::vector<std::string> split(const std::string &v, char sep) {
  std::vector<std::string> parts;
  for (size_t p = 0; p <= v.size();) {
    size_t c = v.find(sep, p);
    if (c == std::string::npos) c = v.size();
    parts.push_back(v.substr(p, c - p));
    p = c + 1;
  }
  return parts;
}
inline uint32_t crc32_iso_hdlc(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return crc;
}
inline std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}
}  // namespace eg2_rv_dm_detail

// Every handler returns false only for a value it cannot parse; a debugger action that fails ends
// the run as failed (fail), as the bench's checks do.
inline Eg2DebugHost::Eg2DebugHost(Eg2RvDm &dm) : dm_(dm) {
  using namespace eg2_rv_dm_detail;
  auto &h = dm_.sys().stim_handlers;
  h["debugger.dmi_write"] = [this](const std::string &v) {
    uint32_t a, d;
    if (!pair(v, &a, &d)) return false;
    log("DMI write " + v);
    if (!dm_.dmi_write(a, d)) fail("the TAP does not reach rv_dm (life cycle)");
    return true;
  };
  h["debugger.dmi_expect"] = [this](const std::string &v) {
    uint32_t a, want, mask, got;
    if (!pair(v, &a, &want, &mask)) return false;
    if (!dm_.dmi_read(a, &got)) log("the TAP does not reach rv_dm (life cycle): the read gives 0");
    char w[48];
    std::snprintf(w, sizeof w, "DMI register 0x%02x", a);
    expect(w, got, want, mask);
    return true;
  };
  auto ctl = [this](const char *key, uint32_t bit, bool level) {
    dm_.sys().stim_handlers[std::string("debugger.") + key] = [this, key, bit,
                                                               level](const std::string &v) {
      bool one = num(v) != 0;
      log(std::string("dmcontrol.") + key + " = " + (one ? "1" : "0"));
      if (one) dmcontrol(bit, 0);
      else dmcontrol(0, bit);
      if (!level) dmcontrol_ &= ~bit;  // a write-1 field: not repeated by the next write
      return true;
    };
  };
  ctl("dmactive", 1u << 0, true);
  ctl("ndmreset", 1u << 1, true);
  ctl("haltreq", 1u << 31, true);
  ctl("resumereq", 1u << 30, false);
  ctl("ackhavereset", 1u << 28, false);
  h["debugger.reg_read"] = [this](const std::string &v) {
    size_t c = v.find(':');
    uint32_t regno = num(v.substr(0, c)), n = c == std::string::npos ? 1 : num(v.substr(c + 1));
    std::string out;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t d = 0;
      if (!reg_rw(regno + i, false, &d)) return true;
      char b[40];
      std::snprintf(b, sizeof b, "%s0x%04x = 0x%08x", out.empty() ? "" : ", ", regno + i, d);
      out += b;
    }
    log("registers read: " + out);
    return true;
  };
  h["debugger.reg_write"] = [this](const std::string &v) {
    uint32_t regno, d;
    if (!pair(v, &regno, &d)) return false;
    log("register write " + v);
    reg_rw(regno, true, &d);
    return true;
  };
  h["debugger.reg_expect"] = [this](const std::string &v) {
    uint32_t regno, want, mask, got = 0;
    if (!pair(v, &regno, &want, &mask)) return false;
    char w[40];
    std::snprintf(w, sizeof w, "register 0x%04x", regno);
    if (reg_rw(regno, false, &got)) expect(w, got, want, mask);
    return true;
  };
  h["debugger.progbuf"] = [this](const std::string &v) {
    std::vector<std::string> words = split(v, ';');
    for (size_t i = 0; i < words.size(); ++i) {
      if (!dm_.dmi_write(Eg2RvDm::kProgbuf0 + uint32_t(i), num(words[i]))) {
        fail("the TAP does not reach rv_dm (life cycle)");
        return true;
      }
    }
    log("program buffer written: " + v);
    return true;
  };
  h["debugger.exec_progbuf"] = [this](const std::string &) {
    log("run the program buffer");
    command((2u << 20) | (1u << 18), "program buffer");
    return true;
  };
  h["debugger.sba_write"] = [this](const std::string &v) {
    std::vector<std::string> p = split(v, ':');
    uint32_t a;
    if (p.size() != 2 || !addr_of(p[0], &a)) return false;
    uint32_t d = num(p[1]);
    log("SBA write " + hex(a) + " = " + hex(d));
    sba_rw(true, a, &d);
    return true;
  };
  h["debugger.sba_read"] = [this](const std::string &v) {
    uint32_t a, d = 0;
    if (!addr_of(v, &a)) return false;
    if (sba_rw(false, a, &d)) log("SBA read " + hex(a) + " = " + hex(d));
    return true;
  };
  h["debugger.sba_expect"] = [this](const std::string &v) {
    size_t c = v.find(':');
    uint32_t a, want, mask, got = 0, dummy;
    if (c == std::string::npos || !addr_of(v.substr(0, c), &a) ||
        !pair("0" + v.substr(c), &dummy, &want, &mask)) {
      return false;
    }
    if (sba_rw(false, a, &got)) expect("SBA read at " + hex(a), got, want, mask);
    return true;
  };
  // The chip bench compares SBA reads with OTP's contents read through its backdoor
  // (chip_sw_rv_dm_ndm_reset_when_cpu_halted_vseq: lc_ctrl DEVICE_ID against OTP's DeviceIdOffset):
  // <addr>:<OTP byte offset>[:<words>].
  h["debugger.sba_expect_otp"] = [this](const std::string &v) {
    std::vector<std::string> p = split(v, ':');
    uint32_t a;
    if (p.size() < 2 || !addr_of(p[0], &a)) return false;
    uint32_t off = num(p[1]), n = p.size() > 2 ? num(p[2]) : 1;
    auto *otp = dynamic_cast<Eg2OtpCtrl *>(dm_.sys().model("otp_ctrl.core"));
    if (!otp) return false;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t got = 0;
      if (!sba_rw(false, a + 4 * i, &got) ||
          !expect("SBA read at " + hex(a + 4 * i) + " against OTP word " + hex(off + 4 * i), got,
                  otp->word(off + 4 * i), 0xffffffffu)) {
        break;
      }
    }
    return true;
  };
  h["debugger.sram_program"] = [this](const std::string &v) {
    std::vector<std::string> parts = split(v, ';');
    wait_checks_.clear();
    for (size_t i = 2; i < parts.size(); ++i) {
      uint32_t r, d;
      if (!pair(parts[i], &r, &d)) return false;
      wait_checks_.push_back({r, d});
    }
    sram_program(parts[0], parts.size() > 1 && parts[1] == "wait");
    return true;
  };
}

// A bus address: a number, or <window>.<REGISTER> (lc_ctrl.regs.DEVICE_ID_0): the register's
// address from the IP's hjson.
inline bool Eg2DebugHost::addr_of(const std::string &s, uint32_t *addr) {
  if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) {
    *addr = eg2_rv_dm_detail::num(s);
    return true;
  }
  size_t dot = s.rfind('.');
  Eg2IpModel *m = dot == std::string::npos ? nullptr : dm_.sys().model(s.substr(0, dot));
  int r = m ? m->reg_index(s.substr(dot + 1)) : -1;
  if (r < 0) return false;
  *addr = m->base() + m->desc().regs[r].offset;
  return true;
}

inline bool Eg2DebugHost::fail(const std::string &why) {
  log("FAIL: " + why);
  dm_.sys().finish(false, "debugger: " + why);
  return false;
}

inline bool Eg2DebugHost::expect(const std::string &what, uint32_t got, uint32_t want,
                                 uint32_t mask) {
  char b[160];
  std::snprintf(b, sizeof b, "%s = 0x%08x, expected 0x%08x under mask 0x%08x", what.c_str(), got,
                want, mask);
  if ((got & mask) != (want & mask)) return fail(b);
  log(std::string("check ") + b + ": ok");
  return true;
}

inline bool Eg2DebugHost::dmcontrol(uint32_t set, uint32_t clear) {
  dmcontrol_ = (dmcontrol_ | set | 1u) & ~clear;
  if (clear & 1u) dmcontrol_ = 0;
  if (!dm_.dmi_write(Eg2RvDm::kDmcontrol, dmcontrol_)) {
    return fail("the TAP does not reach rv_dm (life cycle)");
  }
  return true;
}

inline bool Eg2DebugHost::halted() {
  uint32_t s = 0;
  dm_.dmi_read(Eg2RvDm::kDmstatus, &s);
  return (s >> 9) & 1;  // allhalted
}

// One abstract command; a cmderr fails the run (the bench requires AbstractCmdErrNone) and is
// cleared, as the debugger does before its next command.
inline bool Eg2DebugHost::command(uint32_t cmd, const std::string &what) {
  if (!dm_.dmi_write(Eg2RvDm::kCommand, cmd)) return fail("the TAP does not reach rv_dm (life cycle)");
  uint32_t cs = 0;
  dm_.dmi_read(Eg2RvDm::kAbstractcs, &cs);
  uint32_t err = (cs >> 8) & 7;
  if (!err) return true;
  dm_.dmi_write(Eg2RvDm::kAbstractcs, err << 8);
  static const char *kErr[8] = {"none", "busy", "not supported", "exception", "halt/resume",
                                "bus", "6", "other"};
  return fail("abstract command (" + what + ") ended with cmderr " + std::to_string(err) + " (" +
              kErr[err] + ")");
}

inline bool Eg2DebugHost::reg_rw(uint32_t regno, bool write, uint32_t *value) {
  if (write && !dm_.dmi_write(Eg2RvDm::kData0, *value)) {
    return fail("the TAP does not reach rv_dm (life cycle)");
  }
  char w[40];
  std::snprintf(w, sizeof w, "access register 0x%04x", regno);
  uint32_t cmd = (2u << 20) | (1u << 17) | (uint32_t(write) << 16) | (regno & 0xffff);
  if (!command(cmd, w)) return false;
  if (!write) dm_.dmi_read(Eg2RvDm::kData0, value);
  return true;
}

// A 32-bit System Bus Access; an sberror fails the run.
inline bool Eg2DebugHost::sba_rw(bool write, uint32_t addr, uint32_t *value) {
  // sbaccess 2 (32 bits), sbreadonaddr for a read, sberror cleared.
  if (!dm_.dmi_write(Eg2RvDm::kSbcs, (2u << 17) | (uint32_t(!write) << 20) | (7u << 12))) {
    return fail("the TAP does not reach rv_dm (life cycle)");
  }
  dm_.dmi_write(Eg2RvDm::kSbaddress0, addr);
  if (write) dm_.dmi_write(Eg2RvDm::kSbdata0, *value);
  else dm_.dmi_read(Eg2RvDm::kSbdata0, value);
  uint32_t cs = 0;
  dm_.dmi_read(Eg2RvDm::kSbcs, &cs);
  uint32_t err = (cs >> 12) & 7;
  if (!err) return true;
  return fail(std::string("SBA ") + (write ? "write" : "read") + " at " +
              eg2_rv_dm_detail::hex(addr) + ": sberror " + std::to_string(err));
}

inline bool Eg2DebugHost::sram_program(const std::string &path, bool wait) {
  using namespace eg2_rv_dm_detail;
  if (!halted()) return fail("sram_program: the core is not halted");
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1) {
    return fail("sram_program: " + path + " is not a 32-bit ELF");
  }
  log("load the SRAM program " + path);
  uint32_t entry = rd(24, 4), phoff = rd(28, 4), phent = rd(42, 2), phnum = rd(44, 2);
  uint32_t crc = 0xffffffffu, last = 0;
  bool have_last = false;
  for (uint32_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phent;
    uint32_t type = rd(ph, 4), off = rd(ph + 4, 4), addr = rd(ph + 8, 4), fsz = rd(ph + 16, 4),
             align = rd(ph + 28, 4);
    if (type != 1 || fsz == 0) continue;  // PT_LOAD with data
    if (addr % 4 || fsz % 4) return fail("sram_program: a segment is not word aligned");
    if (align > 256) return fail("sram_program: not linked with nmagic (segment alignment)");
    if (have_last && addr != last) return fail("sram_program: a gap between segments");
    if (size_t(off) + fsz > elf.size()) return fail("sram_program: a segment is outside the file");
    // The words over SBA with sbautoincrement, as a debugger writes a block of memory.
    dm_.dmi_write(Eg2RvDm::kSbcs, (2u << 17) | (1u << 16) | (7u << 12));
    dm_.dmi_write(Eg2RvDm::kSbaddress0, addr);
    for (uint32_t w = 0; w < fsz; w += 4) dm_.dmi_write(Eg2RvDm::kSbdata0, rd(off + w, 4));
    uint32_t cs = 0;
    dm_.dmi_read(Eg2RvDm::kSbcs, &cs);
    if ((cs >> 12) & 7) return fail("sram_program: an SBA error while writing a segment");
    crc = crc32_iso_hdlc(crc, &elf[off], fsz);
    log("segment of " + std::to_string(fsz) + " bytes written at " + hex(addr));
    last = addr + fsz;
    have_last = true;
  }
  crc ^= 0xffffffffu;
  // The text section that holds the entry point, read back and compared.
  uint32_t shoff = rd(32, 4), shent = rd(46, 2), shnum = rd(48, 2);
  bool found = false;
  for (uint32_t i = 0; i < shnum && !found; ++i) {
    size_t sh = shoff + size_t(i) * shent;
    uint32_t type = rd(sh + 4, 4), flags = rd(sh + 8, 4), addr = rd(sh + 12, 4),
             off = rd(sh + 16, 4), size = rd(sh + 20, 4);
    if (type != 1 || !(flags & 4) || entry < addr || entry >= addr + size) continue;
    found = true;
    for (uint32_t w = 0; w + 4 <= size; w += 4) {
      uint32_t got = 0;
      if (!sba_rw(false, addr + w, &got)) return false;
      if (got != rd(off + w, 4)) return fail("sram_program: the entry's section reads back wrong");
    }
  }
  if (!found) return fail("sram_program: no text section holds the entry point");
  // prepare_epmp: region 15 over the main SRAM, region 11 over the MMIO range.
  const uint32_t ram = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR,
                 ram_size = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES;
  uint32_t cfg3 = 0, cfg2 = 0;
  uint32_t a15 = (ram >> 2) | ((ram_size - 1) >> 3);
  uint32_t a10 = TOP_EARLGREY_MMIO_BASE_ADDR >> 2,
           a11 = (TOP_EARLGREY_MMIO_BASE_ADDR + TOP_EARLGREY_MMIO_SIZE_BYTES) >> 2;
  if (!reg_rw(kPmpcfg3, false, &cfg3)) return false;
  cfg3 = (cfg3 & 0x00ffffffu) | 0x9f000000u;
  if (!reg_rw(kPmpcfg3, true, &cfg3) || !reg_rw(kPmpaddr15, true, &a15)) return false;
  if (!reg_rw(kPmpcfg2, false, &cfg2)) return false;
  cfg2 = (cfg2 & 0x00ffffffu) | 0x8f000000u;
  if (!reg_rw(kPmpcfg2, true, &cfg2) || !reg_rw(kPmpaddr10, true, &a10) ||
      !reg_rw(kPmpaddr11, true, &a11)) {
    return false;
  }
  log("ePMP: region 15 L NAPOT X W R over the main SRAM, region 11 L TOR X W R over MMIO");
  // prepare_sram_ctrl: EXEC = MuBi4True.
  Eg2IpModel *sram = dm_.sys().model("sram_ctrl_main.regs");
  int exec_reg = sram ? sram->reg_index("EXEC") : -1;
  if (exec_reg < 0) return fail("sram_program: no sram_ctrl_main EXEC register");
  uint32_t exec = 0x6;  // MuBi4True
  uint32_t exec_addr = TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR + sram->desc().regs[exec_reg].offset;
  if (!sba_rw(true, exec_addr, &exec)) return false;
  log("sram_ctrl_main EXEC = MuBi4True");
  uint32_t ra = 0xdeadbeefu;
  if (!reg_rw(0x1001, true, &ra) || !reg_rw(0x100a, true, &crc)) return false;
  log("ra = 0xdeadbeef, a0 = " + hex(crc) + " (the CRC-32 of the segments)");
  if (wait) {
    uint32_t dcsr = 0;
    if (!reg_rw(kDcsr, false, &dcsr)) return false;
    dcsr |= 1u << 15;  // ebreakm
    if (!reg_rw(kDcsr, true, &dcsr)) return false;
  }
  // OpenOCD's resume at the entry point: dpc, fence.i through the program buffer, the resume.
  if (!reg_rw(kDpc, true, &entry)) return false;
  dm_.dmi_write(Eg2RvDm::kProgbuf0, 0x0000100fu);      // fence.i
  dm_.dmi_write(Eg2RvDm::kProgbuf0 + 1, 0x00100073u);  // ebreak
  if (!command((2u << 20) | (1u << 18), "fence.i")) return false;
  if (!dmcontrol(0, 1u << 31) || !dmcontrol(1u << 30, 0)) return false;
  dmcontrol_ &= ~(1u << 30);
  if (!halted()) {
    log("resumed at the entry point " + hex(entry) +
        (wait ? "; waiting for the program to halt" : ""));
  } else {
    return fail("sram_program: the core did not resume");
  }
  if (wait) {
    waiting_ = true;
    poll_halt(dm_.sys().now_ps());
  }
  return true;
}

// JumpAndWait: dmstatus polled every 10 us until the core halts, then the checks and the verdict.
inline void Eg2DebugHost::poll_halt(uint64_t t_ps) {
  dm_.sys().at(t_ps + 10000000ull, [this] {
    if (!waiting_ || dm_.sys().finished()) return;
    if (!halted()) {
      poll_halt(dm_.sys().now_ps());
      return;
    }
    waiting_ = false;
    uint32_t sp = 0;
    if (!reg_rw(0x1002, false, &sp)) return;
    log("the program halted: sp = " + eg2_rv_dm_detail::hex(sp));
    if (sp != 0xcafebabeu) {
      fail(sp == 0xbaddcafeu ? "the SRAM program reports a CRC mismatch (sp 0xbaddcafe)"
                             : "the SRAM program did not end with sp = 0xcafebabe");
      return;
    }
    for (auto &c : wait_checks_) {
      uint32_t v = 0;
      char w[40];
      std::snprintf(w, sizeof w, "register 0x%04x", c.first);
      if (!reg_rw(c.first, false, &v) || !expect(w, v, c.second, 0xffffffffu)) return;
    }
    log("PASS: the SRAM program ran to its end and every check held");
    dm_.sys().finish(true, "the debugger's checks held (the program writes no DV status)");
  });
}

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_RV_DM_H_
