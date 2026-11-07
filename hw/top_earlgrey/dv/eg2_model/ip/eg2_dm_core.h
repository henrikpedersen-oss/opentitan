// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rv_dm's debug module as a plain C++ reference model: no SystemC, no chip. eg2sim's Eg2RvDm
// (eg2_rv_dm.h) wraps it, and the DPI library (dpi/eg2_dm_dpi.cc) gives it to a UVM bench, which
// sends each transaction to the RTL and to this model and compares what comes back.
//
// Sources: RISC-V Debug Specification 0.13.2 chapter 3 ("spec"); the PULP debug module's
// hw/vendor/pulp_riscv_dbg/doc/debug-system.md ("pulp"): its register table (DataCount 2, 8
// program buffer words, one hart, BusWidth 32, unimplemented registers read 0), its dmcontrol and
// dmstatus tables, hartinfo, and its Debug Memory Map. Nothing is taken from the RTL.
//
// Ports, as rv_dm's:
//   DMI          dmi_read / dmi_write (false: life cycle gates the DMI, nothing happens).
//   debug_req()  rv_dm's debug_req_o; ndmreset() its ndmreset_o; dmactive() its dmactive_o.
//   Life cycle   set_enables(dmi, full): the DMI is reachable / the debug module may act on the
//                core and the bus (rv_dm theory_of_operation.md "Late Debug Enable Mechanism").
//   unavailable  set_unavailable(): rv_dm's unavailable_i (dmstatus.anyunavail).
//   System bus   an access starts with sbbusy set and a request to the bus (Bus); it ends with
//                sba_complete(rdata, error). eg2sim's bus answers at once; a bench answers when its
//                TL-UL device responds, so sbbusy and sbbusyerror (spec 3.12.18) are visible.
//   Core side    one of:
//     direct     a Hart object (eg2sim's core model) that halts, runs abstract commands and resumes
//                at once;
//     memory     the core's accesses to the debug memory (pulp "Debug Memory Map"): mem_write() of
//                Halted 0x100, Going 0x108, Resuming 0x110, Exception 0x118 and of the data words,
//                mem_read() of data, program buffer and flags. The map is documented as "an
//                implementation detail ... should not be relied on" (ticket); the flag bits (go =
//                bit 0, resume = bit 1 of the hart's flag byte at 0x400) are the Debug ROM's
//                (debug_rom.S), not in debug-system.md (ticket). WhereTo, AbstractCmd and the ROM
//                are not predicted (mem_read gives mask 0).

#ifndef EG2_MODEL_IP_EG2_DM_CORE_H_
#define EG2_MODEL_IP_EG2_DM_CORE_H_

#include <cstdint>
#include <functional>

namespace eg2_model {

// The core as the direct binding needs it (eg2_system.h Eg2DebugHart has the same operations).
class Eg2DmHart {
 public:
  virtual ~Eg2DmHart() = default;
  virtual bool halted() const = 0;
  // Access Register, 32 bits (spec 3.6.1.1): 0 or a cmderr value.
  virtual int access_register(uint32_t regno, bool write, uint32_t *value) = 0;
  // CHERIoT mode, aarsize 3 (rv_dm theory_of_operation.md "CHERIoT Mode"): a GPR's capability
  // (regno 0x1000-0x101f) or an SCR (regno 0x0-0x1f) as address and metadata, without its tag.
  // 0 or a cmderr value; a hart that cannot do it answers 2 (not supported).
  virtual int access_capability(uint32_t, bool, uint32_t *, uint32_t *) { return 2; }
  // The program buffer: 0, a cmderr value, or 1 if it does not end (busy stays set, spec 3.7.2).
  virtual int exec_progbuf() = 0;
  virtual void resume() = 0;
};

class Eg2DmCore {
 public:
  // DMI register addresses (spec 3.12; pulp "Debug Module Registers").
  enum : uint32_t {
    kData0 = 0x04, kDmcontrol = 0x10, kDmstatus = 0x11, kHartinfo = 0x12, kHaltsum1 = 0x13,
    kAbstractcs = 0x16, kCommand = 0x17, kAbstractauto = 0x18, kProgbuf0 = 0x20,
    kSbcs = 0x38, kSbaddress0 = 0x39, kSbdata0 = 0x3c, kHaltsum0 = 0x40
  };
  static constexpr unsigned kDataCount = 2, kProgbufSize = 8;
  // Debug memory offsets (pulp "Debug Memory Map").
  enum : uint32_t {
    kMemHalted = 0x100, kMemGoing = 0x108, kMemResuming = 0x110, kMemException = 0x118,
    kMemProgbuf = 0x360, kMemData = 0x380, kMemFlags = 0x400, kMemFlagsEnd = 0x800
  };
  enum class Binding { kDirect, kMemory };

  struct SbaRequest {
    bool write;
    uint32_t addr, size;  // size in bytes: 1, 2 or 4
    uint32_t wdata;
  };
  // The bus: called when an access starts; it may call sba_complete() before it returns.
  using Bus = std::function<void(const SbaRequest &)>;
  // Data and program buffer words: eg2sim keeps them in rv_dm.mem's registers, where its core
  // model reads them; by default the model keeps them itself.
  using Load = std::function<uint32_t(bool progbuf, unsigned i)>;
  using Store = std::function<void(bool progbuf, unsigned i, uint32_t v)>;

  explicit Eg2DmCore(Binding b = Binding::kMemory) : binding_(b) { reset(); }

  void set_hart(Eg2DmHart *h) { hart_ = h; }
  void set_bus(Bus b) { bus_ = std::move(b); }
  void set_store(Load l, Store s) { load_ = std::move(l); store_ = std::move(s); }
  void set_enables(bool dmi, bool full) { dmi_en_ = dmi; full_en_ = full; }
  void set_unavailable(bool u) { unavail_ = u; }
  // rv_dm's CHERIoT mode: CheriotSupport and cheriot_ena_i MuBi4True ("must not change while a hart
  // is halted").
  void set_cheriot(bool c) { cheriot_ = c; }

  // rv_dm's reset (rst_ni): every register takes its reset value; the core has been reset.
  void reset() {
    dm_reset(false);
    havereset_ = true;
    resumeack_ = false;
    mem_halted_ = false;
    for (auto &w : data_) w = 0;
    for (auto &w : progbuf_) w = 0;
  }
  // A reset of the core only (ndmreset, or any reset that leaves the debug module alone).
  void hart_reset() {
    havereset_ = true;
    resumeack_ = false;
    mem_halted_ = false;
  }

  bool debug_req() const { return dmactive_ && haltreq_ && full_en_; }
  bool ndmreset() const { return dmactive_ && ndmreset_ && full_en_; }
  bool dmactive() const { return dmactive_; }
  uint32_t ndmreset_field() const { return ndmreset_; }  // as written, before the life-cycle gate
  bool dmi_enabled() const { return dmi_en_; }
  bool fully_enabled() const { return full_en_; }
  bool halted() const {
    return binding_ == Binding::kDirect ? (hart_ && hart_->halted()) : mem_halted_;
  }
  bool busy() const { return busy_; }
  bool sbbusy() const { return sb_pending_; }

  // ---- DMI ----
  // rmask: the bits of *v the model predicts. Zero where the value depends on timing the model
  // does not have (a register read while an abstract command is busy, spec 3.12.6).
  bool dmi_read(uint32_t addr, uint32_t *v, uint32_t *rmask = nullptr) {
    *v = 0;
    if (rmask) *rmask = 0xffffffffu;
    if (!dmi_en_) return false;
    if (in_data(addr) || in_progbuf(addr)) {
      bool pb = in_progbuf(addr);
      unsigned i = pb ? addr - kProgbuf0 : addr - kData0;
      *v = word(pb, i);
      if (busy_) {
        if (rmask) *rmask = 0;
        set_cmderr(1);
      } else {
        autoexec(addr);
      }
    } else if (addr == kDmcontrol) {
      *v = (ndmreset_ << 1) | uint32_t(dmactive_);  // haltreq WARZ, hartsel no bits (one hart)
    } else if (addr == kDmstatus) {
      // A hart is in exactly one of non-existent, unavailable, running or halted (Debug spec 3.5):
      // an unavailable hart is reported neither halted nor running.
      bool h = halted() && !unavail_, run = !halted() && !unavail_;
      *v = (uint32_t(havereset_) * 3u << 18) | (uint32_t(resumeack_) * 3u << 16) |
           (uint32_t(unavail_) * 3u << 12) | (uint32_t(run) * 3u << 10) |
           (uint32_t(h) * 3u << 8) | (1u << 7) | 2u;
      if (rmask && havereset_unknown_) *rmask &= ~(3u << 18);
      if (rmask && resumeack_unknown_) *rmask &= ~(3u << 16);
    } else if (addr == kHartinfo) {
      *v = (2u << 20) | (1u << 16) | (kDataCount << 12) | kMemData;
    } else if (addr == kAbstractcs) {
      *v = (kProgbufSize << 24) | (uint32_t(busy_) << 12) | (cmderr_ << 8) | kDataCount;
      // Two errors applied to the last refused command and the documents do not say which one
      // cmderr reports (finding "Which error a refused command reports is not documented").
      if (cmderr_alt_ && rmask) *rmask &= ~(7u << 8);
    } else if (addr == kAbstractauto) {
      *v = abstractauto_;
    } else if (addr == kSbcs) {
      *v = (1u << 29) | (sbbusyerror_ << 22) | (uint32_t(sb_pending_) << 21) |
           (sbreadonaddr_ << 20) | (sbaccess_ << 17) | (sbautoinc_ << 16) |
           (sbreadondata_ << 15) | (sberror_ << 12) | (32u << 5) | 0x7u;
      if (sberror_alt_ && rmask) *rmask &= ~(7u << 12);  // two errors applied: order not documented
    } else if (addr == kSbaddress0) {
      *v = sbaddress_;
    } else if (addr == kSbdata0) {
      if (sb_pending_) {
        sbbusyerror_ = 1;  // spec 3.12.18: a read of sbdata0 while sbbusy
        if (rmask) *rmask = 0;
      } else {
        *v = sbdata_;
        if (rmask) *rmask = sbdata_known_;
        if (dmactive_ && sbreadondata_ && !sberror_ && !sbbusyerror_) sba_start(false);
      }
    } else if (addr == kHaltsum0 || addr == kHaltsum1 || addr == 0x34 || addr == 0x35) {
      // haltsum0-3 (PULP lists all four; Debug spec 3.12.12-15): with one hart, bit 0 of each covers
      // hart 0 (haltsum1-3 bit 0: any hart of the first group of 32, 1024, 32768 halted).
      *v = (halted() && !unavail_) ? 1u : 0u;  // as dmstatus: an unavailable hart is not halted
    }
    // command (write-only) and the registers pulp marks "Not implemented": 0.
    return true;
  }

  bool dmi_write(uint32_t addr, uint32_t v) {
    if (!dmi_en_) return false;
    if (addr == kDmcontrol) {
      write_dmcontrol(v);
    } else if (!dmactive_) {
      // dmactive 0 holds the module in reset: only dmactive can be written (spec 3.12.2).
    } else if (in_data(addr) || in_progbuf(addr)) {
      bool pb = in_progbuf(addr);
      if (busy_) {
        set_cmderr(1);
      } else {
        set_word(pb, pb ? addr - kProgbuf0 : addr - kData0, v);
        autoexec(addr);
      }
    } else if (addr == kAbstractcs) {
      if (busy_) {
        // "1 (busy): An abstract command was executing while command, abstractcs, or abstractauto
        // was written" (Debug spec 3.12.6): no W1C then.
        set_cmderr(1);
      } else {
        cmderr_ &= ~((v >> 8) & 7);
        cmderr_alt_ &= ~((v >> 8) & 7);
        if (cmderr_alt_ == cmderr_) cmderr_alt_ = 0;
      }
    } else if (addr == kCommand) {
      if (busy_) {
        set_cmderr(1);  // busy is written only while cmderr is 0 (spec 3.12.6)
      } else if (!cmderr_) {
        command_ = v;
        run_command();
      }
    } else if (addr == kAbstractauto) {
      if (busy_) set_cmderr(1);
      else abstractauto_ = v & ((((1u << kProgbufSize) - 1) << 16) | ((1u << kDataCount) - 1));
    } else if (addr == kSbcs) {
      if (sb_pending_) {
        // spec 3.12.18: sbcs fields other than the W1C errors keep their values while sbbusy.
        sbbusyerror_ &= ~((v >> 22) & 1);
        sberror_ &= ~((v >> 12) & 7);
        sberror_alt_ &= ~((v >> 12) & 7);
        if (sberror_alt_ == sberror_) sberror_alt_ = 0;
      } else {
        sbbusyerror_ &= ~((v >> 22) & 1);
        sberror_ &= ~((v >> 12) & 7);
        sberror_alt_ &= ~((v >> 12) & 7);
        if (sberror_alt_ == sberror_) sberror_alt_ = 0;
        sbreadonaddr_ = (v >> 20) & 1;
        sbaccess_ = (v >> 17) & 7;
        sbautoinc_ = (v >> 16) & 1;
        sbreadondata_ = (v >> 15) & 1;
      }
    } else if (addr == kSbaddress0) {
      if (sb_pending_) {
        sbbusyerror_ = 1;
      } else {
        sbaddress_ = v;
        if (sbreadonaddr_ && !sberror_ && !sbbusyerror_) sba_start(false);
      }
    } else if (addr == kSbdata0) {
      if (sb_pending_) {
        sbbusyerror_ = 1;
      } else {
        sbdata_ = v;
        sbdata_known_ = 0xffffffffu;
        if (!sberror_ && !sbbusyerror_) sba_start(true);
      }
    }
    return true;
  }

  // ---- System bus ----
  void sba_complete(uint32_t rdata, bool error) {
    if (!sb_pending_) return;
    sb_pending_ = false;
    if (error) {
      sberror_ = 2;
      // A read places "the result" in sbdata (Debug spec 3.12.23); what that is for a failed read
      // no document says (rv_dm keeps the error response's data): not predicted until sbdata0 is
      // written or a read succeeds.
      if (!sb_req_.write) sbdata_known_ = 0;
      return;
    }
    if (!sb_req_.write) {
      sbdata_ = rdata & size_mask(sb_req_.size);
      // "If the width of the read access is less than the width of sbdata, the contents of the
      // remaining high bits may take on any value" (Debug spec 3.12.23): not predicted.
      sbdata_known_ = size_mask(sb_req_.size);
    }
    if (sbautoinc_) sbaddress_ += sb_req_.size;
  }

  // ---- Core side, memory binding ----
  // A core write to the debug memory at offset off (word-aligned, the written bytes in be).
  void mem_write(uint32_t off, uint32_t v, uint32_t be = 0xf) {
    if (binding_ != Binding::kMemory) return;
    if (ndmreset_ && full_en_) return;  // a hart in reset writes nothing (see write_dmcontrol)
    if (off == kMemHalted) {
      mem_halted_ = true;
      // A command has finished once the core took it (Going, or an Exception) and is back in the
      // park loop. A Halted before Going leaves it pending, go still set (the Debug ROM's order).
      if (busy_ && going_seen_) {
        busy_ = false;
        flags_ &= ~1u;
      }
    } else if (off == kMemGoing) {
      flags_ &= ~1u;
      going_seen_ = true;
    } else if (off == kMemResuming) {
      mem_halted_ = false;
      resumeack_ = true;
      resumeack_unknown_ = false;
      flags_ &= ~2u;
    } else if (off == kMemException) {
      set_cmderr(3);
      going_seen_ = true;
    } else if (off >= kMemData && off < kMemData + 4 * kDataCount) {
      unsigned i = (off - kMemData) / 4;
      set_word(false, i, merge(word(false, i), v, be));
    } else if (off >= kMemProgbuf && off < kMemProgbuf + 4 * kProgbufSize) {
      unsigned i = (off - kMemProgbuf) / 4;
      set_word(true, i, merge(word(true, i), v, be));
    }
  }
  // A core read: the value, and in *mask the bits the model predicts (0: not predicted).
  uint32_t mem_read(uint32_t off, uint32_t *mask) {
    *mask = 0xffffffffu;
    if (off >= kMemData && off < kMemData + 4 * kDataCount) return word(false, (off - kMemData) / 4);
    if (off >= kMemProgbuf && off < kMemProgbuf + 4 * kProgbufSize)
      return word(true, (off - kMemProgbuf) / 4);
    if (off >= kMemFlags && off < kMemFlagsEnd) {
      // One flag byte per hart; hart 0 is the only hart.
      return off == kMemFlags ? flags_ : 0;
    }
    *mask = 0;
    return 0;
  }

 private:
  static bool in_data(uint32_t a) { return a >= kData0 && a < kData0 + kDataCount; }
  static bool in_progbuf(uint32_t a) { return a >= kProgbuf0 && a < kProgbuf0 + kProgbufSize; }
  static uint32_t size_mask(uint32_t n) { return n >= 4 ? 0xffffffffu : (1u << (8 * n)) - 1; }
  static uint32_t merge(uint32_t old, uint32_t v, uint32_t be) {
    uint32_t m = 0;
    for (unsigned b = 0; b < 4; ++b) if ((be >> b) & 1) m |= 0xffu << (8 * b);
    return (old & ~m) | (v & m);
  }
  uint32_t word(bool pb, unsigned i) const {
    if (load_) return load_(pb, i);
    return pb ? progbuf_[i] : data_[i];
  }
  void set_word(bool pb, unsigned i, uint32_t v) {
    if (store_) store_(pb, i, v);
    else (pb ? progbuf_[i] : data_[i]) = v;
  }
  // Memory binding: the hart has been reset (an NDM reset): not halted, no command of its own.
  void hart_reset_mem() {
    mem_halted_ = false;
    busy_ = false;
    going_seen_ = false;
    flags_ = 0;
  }
  void set_cmderr(uint32_t e) {
    if (!cmderr_) cmderr_ = e;  // no error overwrites an earlier one (spec 3.12.6)
  }
  // A refused command to which two errors apply: the model reports `e` and keeps `alt` as the
  // other value the documents allow; cmderr is not predicted until it is cleared. A W1C write that
  // clears one candidate but not the other leaves the model with its own choice (rare in the
  // random test; the order itself is a documentation finding).
  void set_cmderr2(uint32_t e, uint32_t alt) {
    if (cmderr_) return;
    cmderr_ = e;
    cmderr_alt_ = alt;
  }

  // dmactive 0, or rv_dm's reset: the module's registers take their reset values.
  void dm_reset(bool clear_buffers) {
    dmactive_ = false;
    haltreq_ = ndmreset_ = 0;
    cmderr_ = 0;
    cmderr_alt_ = 0;
    havereset_unknown_ = resumeack_unknown_ = false;
    busy_ = false;
    abstractauto_ = command_ = 0;
    flags_ = 0;
    going_seen_ = false;
    sbreadonaddr_ = sbautoinc_ = sbreadondata_ = sberror_ = sbbusyerror_ = 0;
    sberror_alt_ = 0;
    sbdata_known_ = 0xffffffffu;
    sbaccess_ = 2;
    sbaddress_ = sbdata_ = 0;
    sb_pending_ = false;
    if (clear_buffers) {
      for (unsigned i = 0; i < kProgbufSize; ++i) set_word(true, i, 0);
      for (unsigned i = 0; i < kDataCount; ++i) set_word(false, i, 0);
    }
  }

  void write_dmcontrol(uint32_t v) {
    if (!(v & 1)) {
      dm_reset(dmactive_);
      return;
    }
    bool was_active = dmactive_;
    dmactive_ = true;
    if (!was_active) return;  // the write that activates the module sets only dmactive
    haltreq_ = v >> 31;
    if ((v >> 28) & 1) havereset_ = false;  // ackhavereset
    uint32_t ndm = (v >> 1) & 1;
    // ndmreset resets the harts (Debug spec 3.12.2), so they have been reset (dmstatus.havereset).
    // eg2sim reports that through hart_reset(); a block bench has no core reset to report it.
    if (binding_ == Binding::kMemory && ndm && !ndmreset_ && full_en_) havereset_ = true;
    ndmreset_ = ndm;
    // The harts are in reset while ndmreset is set (Debug spec 3.12.2: it resets them): neither
    // halted nor running a command, and no go or resume pending for them. The block bench's core
    // stays off the debug memory meanwhile (rv_dm_cmodel_random_vseq; rv_dm blocks it then).
    if (binding_ == Binding::kMemory && ndm && full_en_) {
      hart_reset_mem();
      // When havereset is set for an NDM reset, and what the reset does to resumeack, no document
      // says (finding "What sets havereset"; rv_dm sets havereset when the system acknowledges the
      // reset, which a block bench may or may not do): neither is predicted until known again.
      havereset_unknown_ = resumeack_unknown_ = true;
    } else if (((v >> 28) & 1) && !ndm) {
      havereset_unknown_ = false;  // ackhavereset without a reset in the same write: 0 now
    }
    // resumereq: ignored while haltreq is set (spec 3.12.2); it clears resumeack.
    if (((v >> 30) & 1) && !haltreq_) {
      resumeack_ = false;
      resumeack_unknown_ = false;
      if (!full_en_ || !halted()) return;
      if (binding_ == Binding::kDirect) {
        hart_->resume();
        resumeack_ = true;
      } else {
        flags_ |= 2u;
      }
    }
  }

  void run_command() {
    uint32_t cmd = command_;
    uint32_t size = (cmd >> 20) & 7, regno = cmd & 0xffff;
    bool postinc = (cmd >> 19) & 1, postexec = (cmd >> 18) & 1, transfer = (cmd >> 17) & 1,
         write = (cmd >> 16) & 1;
    bool cap = cheriot_ && size == 3;
    // "Not supported" (cmderr 2), from the documents:
    //  - a command type other than Access Register (PULP lists only Access Register);
    //  - with transfer, a size larger than the register: above 32 bits in RV32I mode, above 64 bits
    //    in CHERIoT mode; 8 and 16 bits are supported ("reads of aarsize less than or equal to the
    //    register's actual size must be supported", Debug spec 3.6.1.1; DM SW plan);
    //  - CHERIoT mode, 64 bits, regno 0x20-0xfff ("Higher regno values in the CSR range are
    //    rejected", rv_dm ToO "CHERIoT Mode"; DM FPV plan).
    // x16-x31 in CHERIoT mode must fail, but whether the debug module refuses them or the core
    // traps is open: the model leaves it to the core (memory binding: the bench's core model).
    bool unsupported = (cmd >> 24) != 0 ||
                       (transfer && size > (cheriot_ ? 3u : 2u)) ||
                       (transfer && cap && regno > 0x1f && regno < 0x1000);
    bool not_halted = !full_en_ || !halted();
    if (unsupported && not_halted) {
      set_cmderr2(2, 4);  // both apply; the order is not documented
      return;
    }
    if (unsupported) {
      set_cmderr(2);
      return;
    }
    if (not_halted) {
      set_cmderr(4);  // halt/resume: the hart is not halted
      return;
    }
    // Custom registers (regno 0xc000-0xffff): what the debug module does with them is not
    // documented; the outcome is not predicted.
    if (transfer && (regno >> 14) != 0) {
      set_cmderr2(2, 3);
      return;
    }
    if (binding_ == Binding::kMemory) {
      // The core runs the command from the debug memory; busy until it is back in the park loop.
      busy_ = true;
      flags_ |= 1u;
      mem_halted_ = true;
      going_seen_ = false;
    } else {
      if (transfer && cap) {
        uint32_t a = word(false, 0), m = word(false, 1);
        int err = hart_->access_capability(regno, write, &a, &m);
        if (err) {
          set_cmderr(uint32_t(err));
          return;
        }
        if (!write) {
          set_word(false, 0, a);
          set_word(false, 1, m);
        }
      } else if (transfer) {
        // 8 and 16 bits: a read writes only the low bytes of data0; a write takes data0's low
        // bytes sign-extended (the Debug ROM's lb/lh; DM SW plan, DM FPV plan).
        uint32_t d0 = word(false, 0), d = d0;
        uint32_t m = size_mask(size == 0 ? 1 : size == 1 ? 2 : 4);
        if (write && m != 0xffffffffu) {
          uint32_t sign = (m >> 1) + 1;
          d = ((d0 & m) ^ sign) - sign;
        }
        int err = hart_->access_register(regno, write, &d);
        if (err) {
          set_cmderr(uint32_t(err));
          return;
        }
        if (!write) set_word(false, 0, (d0 & ~m) | (d & m));
      }
      if (postexec) {
        int err = hart_->exec_progbuf();
        if (err == 1) {
          busy_ = true;
          return;
        }
        if (err) {
          set_cmderr(uint32_t(err));
          return;
        }
      }
    }
    if (postinc && transfer) command_ = (cmd & ~0xffffu) | ((regno + 1) & 0xffff);
  }

  void autoexec(uint32_t addr) {
    if (!dmactive_ || cmderr_ || busy_) return;
    bool d = in_data(addr) && ((abstractauto_ >> (addr - kData0)) & 1);
    bool p = in_progbuf(addr) && ((abstractauto_ >> (16 + addr - kProgbuf0)) & 1);
    if (d || p) run_command();
  }

  void sba_start(bool write) {
    if (!full_en_) {
      sberror_ = 7;
      return;
    }
    if (sbaccess_ > 2) {
      // Size not supported (sbaccess8/16/32 only). An address misaligned for that size is an
      // alignment error as well; the documents do not say which one sberror reports then.
      sberror_ = 4;
      if (sbaccess_ < 31 && (sbaddress_ & ((1u << sbaccess_) - 1))) sberror_alt_ = 3;
      return;
    }
    uint32_t n = 1u << sbaccess_;
    if (sbaddress_ & (n - 1)) {
      sberror_ = 3;  // alignment
      return;
    }
    sb_req_ = {write, sbaddress_, n, sbdata_ & size_mask(n)};
    sb_pending_ = true;
    if (bus_) bus_(sb_req_);
  }

  Binding binding_;
  Eg2DmHart *hart_ = nullptr;
  Bus bus_;
  Load load_;
  Store store_;
  bool dmi_en_ = false, full_en_ = false, unavail_ = false, cheriot_ = false;
  bool dmactive_ = false, havereset_ = true, resumeack_ = false, busy_ = false;
  bool mem_halted_ = false, going_seen_ = false;
  bool havereset_unknown_ = false, resumeack_unknown_ = false;  // after an NDM reset (write_dmcontrol)
  uint32_t cmderr_alt_ = 0;  // the other cmderr value two applicable errors allow (0: none)
  uint32_t haltreq_ = 0, ndmreset_ = 0, cmderr_ = 0, abstractauto_ = 0, command_ = 0, flags_ = 0;
  uint32_t sbreadonaddr_ = 0, sbaccess_ = 2, sbautoinc_ = 0, sbreadondata_ = 0, sberror_ = 0;
  uint32_t sbbusyerror_ = 0, sbaddress_ = 0, sbdata_ = 0;
  uint32_t sbdata_known_ = 0xffffffffu;  // the bits of sbdata0 the model predicts
  uint32_t sberror_alt_ = 0;             // the other sberror two applicable errors allow (0: none)
  bool sb_pending_ = false;
  SbaRequest sb_req_{};
  uint32_t data_[kDataCount] = {}, progbuf_[kProgbufSize] = {};
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_DM_CORE_H_
