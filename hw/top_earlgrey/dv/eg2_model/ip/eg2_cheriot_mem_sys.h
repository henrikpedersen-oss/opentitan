// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_CHERIOT_MEM_SYS_H_
#define EG2_MODEL_IP_EG2_CHERIOT_MEM_SYS_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_core_system.h"
#include "hw/ip/cheriot_mem_sys/dv/cms/cmodel/cms_core.h"

namespace eg2_model {

// What the subsystem's sweep needs of the top: its meta SRAM and the regions it covers
// (cheriot_mem_sys.sv's address-map parameters). Given with configure(), the TBRE sweeps; without
// it the model is the stand-in described below. The bitmap covers [heap_base, heap_base +
// 64 * revbm_bytes), the C model's "SRAM" (cms::Map); the tagged regions outside it are its "NVM".
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

// cheriot_mem_sys.regs (hw/ip/cheriot_mem_sys/data/cheriot_mem_sys.hjson): the revocation engine.
//
// What the engine decides comes from the subsystem's reference model, cms::Model
// (hw/ip/cheriot_mem_sys/dv/cms/cmodel/cms_core.h), the one its block benches check the RTL
// against. The model has no clock and no memories of its own that eg2sim could use, so the work is
// split:
//   cms::Model  the TBRE registers (TBRE_REGWEN, _BASE_ADDR, _NUM_CAPS, _STATUS, _EPOCH: every
//               write goes to it, every value comes from it, mirror()); the start checks and
//               start_err (CHERIoT mode, NUM_CAPS zero, a base outside the tagged regions); the
//               verdict on each capability (may_revoke: sealing, the base, the bitmap bit, a forced
//               revoke after a failed lookup); sweep_err and the epoch at the sweep's end; the
//               fatal_fault alerts the bench's faults raise (alerts_expected).
//   eg2sim      when things happen: the sweep runs min(TBRE_NUM_CAPS, (top - TBRE_BASE_ADDR) / 8)
//               capabilities in address order, cycles_per_cap clock cycles each (the doc gives no
//               rate: "up to two words are in flight", so at best one word a cycle), on Earl Grey
//               2's layout (eg2_layout) or a board's (configure; the RTOS test SoC, rtos_soc/); it
//               reads each tagged capability and the bitmap word of its base from the real memories
//               at its turn, hands them to the model (backdoor_data, backdoor_revbm), and clears the
//               tag in the meta SRAM when the model says revoke; tbre_done at the end; INTR_* and
//               ALERT_TEST (Eg2IpModel's CIP registers).
// Each capability is read and cleared at one instant, so the doc's watch of core writes in flight
// has nothing to do here. A fault sets TBRE_STATUS.sweep_err when it arrives. If software clears
// that bit during the sweep and its value at completion is unspecified, the model stops rather
// than choosing a value (status/dv_tickets.md, "TBRE sweep_err cleared during a sweep").
//
// Errors ("Error Handling", programmers_guide.md "Errors") come from the bench's faults
// (tbre_fault_arm), given to the model: an error response to a word read keeps that capability and
// sets sweep_err (sweep_read_error); an integrity fault on it raises fatal_fault too (sweep_fault);
// an error or integrity fault on a bitmap lookup counts as revoked and raises fatal_fault
// (sweep_fault on the lookup).
//
// The core's bitmap port (corerevbm_tl) and its faults: below, core_revbm (eg2sim's own, not yet the
// model's).
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
    // The model knows the registers by their offsets in the hjson; a renamed or moved register
    // would make every value wrong, so stop with the reason.
    const std::pair<uint16_t, uint32_t> kOffs[] = {
        {regwen_, cms::kTbreRegwen}, {base_addr_, cms::kTbreBase}, {num_, cms::kTbreNum},
        {start_, cms::kTbreStart},   {status_, cms::kTbreStatus},  {epoch_, cms::kTbreEpoch}};
    for (const auto &ro : kOffs) {
      if (desc().regs[ro.first].offset != ro.second) {
        Eg2ModelStop::request(std::string("cheriot_mem_sys: register ") + desc().regs[ro.first].name +
                              " is not at the offset cms_core.h has for it");
      }
    }
    sys_.core_revbm = [this](uint32_t off, uint8_t *b) { return core_revbm(off, b); };
    bench_ops();
  }

  // ---- The core's bitmap port, corerevbm_tl (Earl Grey 2) ----
  // theory_of_operation.md: the revocation bitmap is the base of the meta SRAM (3 KiB at offset 0,
  // "reachable from revbm_tl_d, corerevbm_tl, revocation engine"); bit b of word w covers the 8
  // heap bytes at MainSramBaseAddr + (w * 32 + b) * 8. Ibex cheriot.rst: only capabilities based
  // below the heap base plus 64 times the bitmap size are looked up (CheriotRevBitmapSizeBytes).
  // A failed lookup revokes ("Revocation Engine": the TRVK filter revokes a capability "whose
  // bitmap lookup fails"). The response faults are the bench's (revbm_err_arm), each with the result
  // the documents give it:
  //   bus_err  the core's port answers d_error with correct integrity: the lookup fails. No
  //            document says it raises an alert (status/dv_tickets.md, "core bitmap d_error and
  //            fatal_hw_err"), so the model raises none.
  //   intg     the core's port response with a bit of its response integrity, data integrity or
  //            data flipped: a bus integrity error at the core (rv_core_ibex interfaces.md
  //            fatal_hw_err "A bus integrity error is seen"; ERR_STATUS.FATAL_INTG_ERR), and the
  //            lookup fails.
  //   mem_err  the meta SRAM's response to the lookup with a data bit flipped, its integrity
  //            unchanged: cheriot_mem_sys's fatal_fault ("Error Handling": "Integrity fault on a meta
  //            SRAM response"); the arbitration is "on transactions that are already
  //            integrity-protected end to end", so the word reaches the core with the same broken
  //            integrity: a bus integrity error there too, and the lookup fails.
  static constexpr uint32_t kRevbmBytes = 0xc00;  // MemSizeRevbm (the meta SRAM map)
  static constexpr uint32_t kHeapBase = 0x10000000;  // MainSramBaseAddr (cheriot_mem_sys.hjson)
  bool core_revbm(uint32_t off, uint8_t *byte) {
    *byte = 0;
    if (off >= kRevbmBytes) return true;  // not looked up
    auto it = sys_.memories.find("sram_ctrl_meta.ram");
    if (it == sys_.memories.end()) return true;
    *byte = it->second->data()[off];
    uint32_t word = off / 4;
    if (hold_.armed && !hold_.held && word == hold_.word) return hold_lookup(word);
    if (!err_.armed || word != err_.word) return true;
    err_.armed = false;
    err_.hit = true;
    static const char *kKind[] = {"d_error on the core's bitmap port (corerevbm_tl)",
                                  "integrity broken on the core's bitmap port response",
                                  "a data bit flipped in the meta SRAM's response"};
    char what[200];
    std::snprintf(what, sizeof what, "%s, field %u, bitmap word %u", kKind[err_.kind % 3],
                  err_.field, word);
    sys_.fault_applied("revbm_err", what);
    if (err_.kind == 2) {
      raise_alert("fatal_fault");
      err_.cms_alert = true;
    }
    if (err_.kind != 0) {
      if (sys_.core_bus_intg_error) sys_.core_bus_intg_error("revocation-bitmap response integrity");
      err_.core_alert = true;
    }
    // A lookup is made only for a tagged capability that is not a sealing one (the load barrier):
    // the failed lookup is the revoked verdict, and the capability reaches the core untagged.
    err_.revoked = true;
    sys_.at(sys_.now_ps() + 1, [this] { err_finish(); });
    return false;
  }

 private:
  struct RevbmErr {
    bool armed = false, done = false, hit = false, revoked = false, core_alert = false,
         cms_alert = false, end_waiting = false;
    uint32_t kind = 0, field = 0, word = 0, end_n = 0;
    uint64_t gen = 0;
  } err_;
  struct RevbmHold {
    bool armed = false, held = false, reset_seen = false, done = false, end_waiting = false;
    uint32_t word = 0, kind = 0, end_n = 0;
  } hold_;
  // The bench's hold before its power-on or NDM reset (chip_sw_eg2_revbm_errors_vseq hold_cycles,
  // 1-400 main clock cycles: --stim bench.hold_cycles=<n>).
  uint32_t hold_cycles_ = 100;
  // The vseq's bounds, in main clock cycles: ArmTimeout for the read, then the run's end 1 ms after
  // the fetch is off.
  static constexpr uint64_t kArmTimeout = 2000000;
  uint64_t main_cycles_ps(uint64_t n) const {
    return sys_.clock_time_of_cycle(Eg2System::Clk::kMain,
                                    sys_.clock_cycles(Eg2System::Clk::kMain, sys_.now_ps()) + n);
  }
  uint32_t word_of(uint32_t heap_addr) const { return ((heap_addr - kHeapBase) >> 8) & 0x3ff; }
  bool check(const std::string &what, bool ok, const std::string &detail = "") {
    return sys_.bench_check ? sys_.bench_check(what, ok, detail) : ok;
  }

  void bench_ops() {
    sys_.bench_ops["revbm_err_arm"] = [this](uint32_t n, uint32_t a, uint32_t b) {
      err_ = RevbmErr{};
      err_.armed = true;
      err_.kind = a & 0xff;
      err_.field = (a >> 8) & 0xff;
      err_.word = word_of(b);
      uint64_t gen = ++err_gen_;
      err_.gen = gen;
      if (sys_.bench_ack) sys_.bench_ack(n, 0);
      sys_.at(main_cycles_ps(kArmTimeout), [this, gen] {
        if (err_.gen != gen || err_.hit || err_.done) return;
        err_.armed = false;
        check("bitmap read seen", false, "(corerevbm, word " + std::to_string(err_.word) + ")");
        err_done();
      });
    };
    sys_.bench_ops["revbm_err_end"] = [this](uint32_t n, uint32_t, uint32_t) {
      if (err_.done) {
        if (sys_.bench_ack) sys_.bench_ack(n, err_result());
        return;
      }
      err_.end_waiting = true;
      err_.end_n = n;
    };
    sys_.bench_ops["revbm_hold_reset"] = [this](uint32_t n, uint32_t a, uint32_t b) {
      hold_ = RevbmHold{};
      hold_.armed = true;
      hold_.word = word_of(a);
      hold_.kind = b;
      if (sys_.bench_ack) sys_.bench_ack(n, 0);
    };
    sys_.bench_ops["revbm_hold_end"] = [this](uint32_t n, uint32_t, uint32_t) {
      // After the reset: what boot 1 recorded (the model keeps it across the reset, as the
      // bench's own state does).
      if (sys_.bench_ack) {
        sys_.bench_ack(n, uint32_t(hold_.held) | (uint32_t(hold_.reset_seen) << 1));
      }
    };
    // chip_sw_eg2_revocation_corner_bench_vseq: one fault on the response to a TBRE request, kind 0
    // d_error / 1 integrity on the read of swept word <addr>, 2 / 3 the same on the bitmap lookup
    // of the word covering granule <addr>; tbre_fault_end answers whether it took effect.
    sys_.bench_ops["tbre_fault_arm"] = [this](uint32_t n, uint32_t a, uint32_t b) {
      tf_ = TbreFault{};
      tf_.armed = true;
      tf_.kind = a;
      tf_.addr = b;
      tf_.word = word_of(b);
      if (sys_.bench_ack) sys_.bench_ack(n, 0);
    };
    sys_.bench_ops["tbre_fault_end"] = [this](uint32_t n, uint32_t, uint32_t) {
      tf_.armed = false;
      if (sys_.bench_ack) sys_.bench_ack(n, tf_.hit ? 1 : 0);
    };
    sys_.stim_handlers["bench.hold_cycles"] = [this](const std::string &v) {
      hold_cycles_ = uint32_t(std::stoul(v, nullptr, 0));
      return true;
    };
  }

  uint32_t err_result() const {
    return uint32_t(err_.hit) | (uint32_t(err_.revoked) << 1) | (uint32_t(err_.core_alert) << 3) |
           (uint32_t(err_.cms_alert) << 4);
  }
  // The checks the vseq makes once the fault has been taken.
  void err_finish() {
    bool meta = err_.kind == 2;
    check("bitmap read seen", true, "(corerevbm, word " + std::to_string(err_.word) + ")");
    check("fault taken", err_.hit, "(the lookup took the changed response)");
    check("verdict within the bound (no hang)", true);
    check("failed check revoked", err_.revoked != sys_.bench_inject_fail,
          std::string("(revoked ") + (err_.revoked ? "1" : "0") + ", tagged 0 at the verdict)");
    if (meta) {
      check("cheriot_mem_sys fatal_fault requested", err_.cms_alert,
            std::string("(rv_core_ibex fatal_hw_err: ") + (err_.core_alert ? "1" : "0") + ")");
    } else {
      check("rv_core_ibex fatal_hw_err requested", err_.core_alert,
            std::string("(cheriot_mem_sys fatal_fault: ") + (err_.cms_alert ? "1" : "0") + ")");
    }
    err_done();
  }
  void err_done() {
    err_.done = true;
    if (err_.end_waiting && sys_.bench_ack) sys_.bench_ack(err_.end_n, err_result());
    end_if_fetch_off();
  }
  // With the core's fetch off (theory_of_operation.md: fatal_hw_err disables it until reset) the
  // software never finishes: the bench's verdict ends the run, 1 ms on.
  void end_if_fetch_off() {
    sys_.at(main_cycles_ps(100), [this] {
      if (!sys_.core_halted()) return;
      sys_.note("bench: the CPU's fetch is off; the bench ends the run");
      sys_.at(sys_.now_ps() + 1000000000ull, [this] {
        if (sys_.bench_end) sys_.bench_end("the CPU's fetch is off after the bitmap fault");
      });
    });
  }

  // The lookup of the held word: its response is held (no answer reaches the core), then the
  // reset of the chosen kind comes while it is outstanding.
  bool hold_lookup(uint32_t word) {
    hold_.held = true;
    hold_.armed = false;
    check("bitmap read held", true, "(word " + std::to_string(word) + ")");
    sys_.hold_core("bench", "the core's bitmap read of word " + std::to_string(word) +
                                " is held (no response)");
    sys_.fault_applied("revbm_hold", "the core's bitmap read held until the reset");
    if (hold_.kind == 1) {
      sys_.at(main_cycles_ps(hold_cycles_), [this] {
        sys_.note("bench: power-on reset during the bitmap read");
        hold_release();
        sys_.apply_stim("rstmgr.por", "1");
      });
    } else if (hold_.kind == 2) {
      sys_.at(main_cycles_ps(hold_cycles_), [this] {
        sys_.note("bench: NDM reset (rv_dm dmcontrol.ndmreset over JTAG) during the bitmap read");
        hold_release();
        sys_.apply_stim("debugger.dmactive", "1");
        sys_.apply_stim("debugger.ndmreset", "1");
        // ndm_reset: ndmreset back to 0 5-20 AON cycles later.
        uint64_t aon = sys_.clock_cycles(Eg2System::Clk::kAon, sys_.now_ps());
        sys_.at(sys_.clock_time_of_cycle(Eg2System::Clk::kAon, aon + 10),
                [this] { sys_.apply_stim("debugger.ndmreset", "0"); });
      });
    } else {
      // The software's watchdog bites (armed before the load): the chip resets by itself.
      watch_reset_ = true;
    }
    return true;
  }
  void hold_release() {
    hold_.reset_seen = true;
    sys_.release_core("bench");
    check("reset while the bitmap read was held", true,
          "(kind " + std::to_string(hold_.kind) + ")");
    // Written during the reset, before any software reads it.
    sys_.set_test_param("reset_done", 1);
  }

 public:
  void before_reset(Eg2ResetCause c) override {
    Eg2IpModel::before_reset(c);
    if (watch_reset_) {
      watch_reset_ = false;
      hold_.reset_seen = true;
      sys_.release_core("bench");
      check("reset while the bitmap read was held", true, "(the watchdog's bite)");
      sys_.set_test_param("reset_done", 1);
    }
  }

  // The board's layout, and the model built on it: its "SRAM" is the span the bitmap covers, its
  // "NVM" the first tagged region outside that span (the NVM on Earl Grey 2, the code RAM on the
  // RTOS test SoC). Called before any TBRE register is written (reg_write builds Earl Grey 2's
  // first), since a new model starts from reset.
  void configure(const Eg2TbreLayout &l) {
    layout_ = l;
    swept_ = true;
    uint32_t sram_top = l.heap_base + 64 * l.revbm_bytes, nvm_base = 0, nvm_top = 0;
    for (const Eg2TbreLayout::Region &r : l.regions) {
      if (r.base >= l.heap_base && r.top <= sram_top) continue;
      nvm_base = r.base;
      nvm_top = r.top;
      break;
    }
    cms_ = cms::Model(cms::Map{l.heap_base, sram_top, nvm_base, nvm_top, 0});
    cms_alerts_ = 0;
    err_shown_ = false;
    mirror();
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
    // The model: busy and REGWEN back, sweep_err, the epoch ("a sweep during which any error sets
    // TBRE_STATUS.sweep_err is not counted", theory_of_operation.md TBRE_EPOCH).
    cms_.sweep_end();
    mirror();
    set_intr("tbre_done");
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    if (keeps_state(c)) return;
    left_ = 0;
    cms_.dut_reset();
    err_shown_ = false;
    mirror();
  }

 protected:
  // A write to a TBRE register goes to the model, whose values then replace the register block's.
  // The block has already applied the write (or dropped it, for a register TBRE_REGWEN gates).
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    if (r != base_addr_ && r != num_ && r != start_ && r != status_) return;
    if (!swept_) eg2_layout();
    const eg2_regs::Reg &d = desc().regs[r];
    if (d.regwen >= 0 && (value(uint16_t(d.regwen)) & d.regwen_mask) != d.regwen_true) {
      mirror();  // dropped: the block's value stays the model's
      return;
    }
    bool was_active = cms_.sweep_active();
    if (r == start_ && (wdata & wmask & 1)) {
      // The mode the start is checked against: rv_core_ibex.cfg's lock, or the board's.
      bool cheriot;
      if (swept_) {
        cheriot = layout_.cheriot && layout_.cheriot();
      } else {
        auto *cfg = dynamic_cast<Eg2RvCoreIbexCfg *>(sys_.model("rv_core_ibex.cfg"));
        cheriot = cfg && cfg->cheriot();
      }
      cms_.set_mode(cheriot ? cms::kMuBi4True : 0);
    }
    // TBRE_STATUS is rw1c: software clearing sweep_err clears what this sweep's fault showed.
    if (r == status_ && (wdata & wmask) >> 9 & 1) err_shown_ = false;
    cms_.update_csr_write(d.offset, wdata & wmask);
    mirror();
    if (was_active || !cms_.sweep_active()) return;  // no sweep started (or start_err)

    uint32_t a = value(base_addr_), top = region_top(a);
    uint64_t caps = top > a ? std::min<uint64_t>(value(num_), (top - a) / 8) : 0;
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
  // Mirror the core's register predictions. While a sweep is active, this
  // adapter knows when its injected fault arrived. At completion, an open
  // sweep_err value is a documentation gap, not a choice for the adapter.
  void mirror() {
    for (uint16_t r : {regwen_, base_addr_, num_, epoch_}) {
      set_value(r, cms_.predict_csr_read(desc().regs[r].offset).rdata);
    }
    cms::CsrExp s = cms_.predict_csr_read(cms::kTbreStatus);
    uint32_t open = ~s.rdata_care & (1u << 9);
    if (open && !cms_.sweep_active()) {
      Eg2ModelStop::request("undocumented: TBRE_STATUS.sweep_err after software clears it "
                           "during a faulty sweep (dv_tickets.md, TBRE sweep_err cleared "
                           "during a sweep)");
    }
    set_value(status_, (s.rdata & ~open) | (err_shown_ ? open : 0));
    // The alerts the model raised since the last look (a fault in the sweep).
    while (cms_alerts_ < cms_.alerts_expected()) {
      ++cms_alerts_;
      raise_alert("fatal_fault");
    }
  }
  // A fault hit the sweep: the model's verdict, sweep_err on show from now.
  void fault_hit() {
    err_shown_ = true;
    mirror();
  }

  // The top (exclusive) of the tagged region holding a, 0 outside every one.
  uint32_t region_top(uint32_t a) const {
    if (swept_ && !spans_.empty()) {
      for (const auto &sp : spans_) {
        if (a >= sp.first && a < sp.second) return sp.second;
      }
      return 0;
    }
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
      // The engine reads both words of every capability (theory_of_operation.md "Revocation
      // Engine"); the bench's fault on the response to one of them (tbre_fault_arm kind 0, 1).
      if (tf_.armed && tf_.kind < 2 && (tf_.addr & ~3u) >= a && (tf_.addr & ~3u) < a + 8) {
        tf_.armed = false;
        tf_.hit = true;
        char what[120];
        std::snprintf(what, sizeof what, "%s on the response to the TBRE's read of 0x%08x",
                      tf_.kind == 0 ? "d_error" : "an integrity bit flipped", tf_.addr);
        sys_.fault_applied("tbre_fault", what);
        if (tf_.kind == 0) {
          // "Error response to a read of the revocation engine": word not cleared, sweep_err (no
          // alert: "an error response to a read of the revocation engine ... only fails the sweep").
          cms_.sweep_read_error(a);
          fault_hit();
          return;
        }
        // "Any other error, or an integrity fault, on a response to the revocation engine":
        // sweep_err and fatal_fault. Whether the capability is still resolved (its tag cleared or
        // kept) no document says: status/dv_tickets.md "TBRE word integrity fault and the tag".
        cms_.sweep_fault(false, a);
        fault_hit();
        Eg2ModelStop::request("undocumented: whether the TBRE resolves a capability whose word "
                              "read had an integrity fault (dv_tickets.md, TBRE word integrity "
                              "fault and the tag)");
        return;
      }
      if (!((tags >> (g % 8)) & 1)) return;
      const std::vector<uint8_t> &m = *r.data;
      uint32_t off = a - r.base, w0 = 0, w1 = 0;
      for (int i = 0; i < 4; ++i) {
        w0 |= uint32_t(m[off + i]) << (8 * i);
        w1 |= uint32_t(m[off + 4 + i]) << (8 * i);
      }
      // The model judges the capability as the engine reads it now: its two words, and the bitmap
      // word its base looks up (a sealing capability, or a base outside the bitmap, has no lookup;
      // Ibex cheriot.rst: only bases below the heap base plus 64 times the bitmap size).
      cms_.backdoor_data(a, w0);
      cms_.backdoor_data(a + 4, w1);
      const cms::Map &mp = cms_.map();
      uint32_t base = cms::cap_base(w0, w1);
      if (!cms::cap_sealing(w1) && mp.in_sram(base)) {
        uint32_t word = (base - mp.sram_base) >> 8, bm = 0;
        for (int i = 0; i < 4; ++i) bm |= uint32_t((*layout_.meta)[word * 4 + i]) << (8 * i);
        cms_.backdoor_revbm(word, bm);
        // The bench's fault on the engine's bitmap lookup of this word (tbre_fault_arm kind 2, 3):
        // "Error, integrity fault or malformed response on a revocation engine bitmap lookup:
        // Capability counted as revoked, TBRE_STATUS.sweep_err and fatal_fault alert".
        if (tf_.armed && tf_.kind >= 2 && word == tf_.word) {
          tf_.armed = false;
          tf_.hit = true;
          char what[120];
          std::snprintf(what, sizeof what, "%s on the TBRE's bitmap lookup of word %u",
                        tf_.kind == 2 ? "d_error" : "an integrity bit flipped", tf_.word);
          sys_.fault_applied("tbre_fault", what);
          cms_.sweep_fault(true, a);
          fault_hit();
        }
      }
      if (!cms_.may_revoke(mp.granule(a))) return;
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
  uint64_t err_gen_ = 0;
  bool watch_reset_ = false;
  cms::Model cms_;              // the subsystem's reference model (Earl Grey's map until configure)
  unsigned cms_alerts_ = 0;     // the model's alerts raised so far
  bool err_shown_ = false;      // a fault of this sweep shows sweep_err, until software clears it
  std::vector<std::pair<uint32_t, uint32_t>> spans_;  // the tagged regions [base, top)
  struct TbreFault {
    bool armed = false, hit = false;
    uint32_t kind = 0, addr = 0, word = 0;
  } tf_;
  // Earl Grey 2's engine (cheriot_mem_sys.sv's address-map parameters, the meta SRAM map of
  // theory_of_operation.md): main and sec SRAM, one tagged region from MainSramBaseAddr with its
  // tags at 0x8C00; the NVM with its tags at 0x0C00; the bitmap at the meta SRAM's base, 3 KiB; the
  // mode is rv_core_ibex.cfg's lock; the main clock.
  void eg2_layout() {
    auto mem = [this](const char *n) -> Eg2Memory * {
      auto it = sys_.memories.find(n);
      return it == sys_.memories.end() ? nullptr : it->second;
    };
    Eg2Memory *main = mem("sram_ctrl_main.ram"), *sec = mem("sram_ctrl_sec.ram"),
              *nvm = mem("rram_ctrl.host"), *meta = mem("sram_ctrl_meta.ram");
    if (!main || !meta) return;
    Eg2TbreLayout l;
    l.meta = &meta->data();
    l.heap_base = main->base();
    l.revbm_bytes = kRevbmBytes;
    l.regions.push_back({main->base(), main->base() + main->size(), 0x8c00, &main->data()});
    uint32_t heap_top = main->base() + main->size();
    if (sec && sec->base() == heap_top) {
      l.regions.push_back({sec->base(), sec->base() + sec->size(), 0x8c00 + main->size() / 64,
                           &sec->data()});
      heap_top = sec->base() + sec->size();
    }
    spans_.push_back({main->base(), heap_top});
    if (nvm) {
      l.regions.push_back({nvm->base(), nvm->base() + nvm->size(), 0x0c00, &nvm->data()});
      spans_.push_back({nvm->base(), nvm->base() + nvm->size()});
    }
    l.cheriot = [this] {
      auto *cfg = dynamic_cast<Eg2RvCoreIbexCfg *>(sys_.model("rv_core_ibex.cfg"));
      return cfg && cfg->cheriot();
    };
    l.clk = Eg2System::Clk::kMain;
    l.cycles_per_cap = 2;
    configure(l);
  }
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_CHERIOT_MEM_SYS_H_
