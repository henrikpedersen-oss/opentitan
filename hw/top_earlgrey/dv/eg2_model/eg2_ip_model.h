// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2IpModel: an IP of the EG2 SystemC model (milestone M3 of the system model plan). It is the
// IP's register block (Eg2RegBlock, from the IP's hjson) plus what every OpenTitan IP does the same
// way, from the top's interrupt and alert lists (eg2_model_irqs.h):
//
//   Interrupts  INTR_STATE, INTR_ENABLE, INTR_TEST (OpenTitan's comportability specification):
//               an event-type interrupt is set by the IP (set_intr) or by INTR_TEST and cleared by
//               software writing 1 to INTR_STATE; a status-type interrupt follows its source
//               (set_intr_level) or-ed with its INTR_TEST bit, which stays until the next INTR_TEST
//               write (prim_intr_hw "Status": test_q, INTR_STATE = event | test_q).
//               Each interrupt's line, state & enable, drives its rv_plic source.
//   Alerts      a 1 written to an ALERT_TEST bit raises that alert; raise_alert raises one from
//               the IP's behaviour.
//
// Every window with a register block is an Eg2IpModel. An IP's behaviour is a subclass, chosen by
// the IP type or window name in eg2_ip_models.cc (make_ip_model). A subclass overrides
//   reg_write(reg, old, wdata, wmask)  after a software write (value(reg) is the new value)
//   reg_before_read(reg)               before a software read: bring the register up to date
//   reg_after_read(reg)                after a software read (read side effects)
//   win_write(win, off, len)           after a software write into a memory window
//   win_read(win, off, data, len)      a software read of a memory window: fill data and return
//                                      true to answer it (a FIFO pops), false for the memory
//   reg_before_write(reg, wdata, wmask) before a software write: false leaves the register as it is
// Any hook may call fail_access() to answer the access with an error (d_error).
//   on_time(now)                       at the time it asked for with schedule(t)
//   before_reset(cause)                when the chip is about to reset, every model still as it was
//   on_reset(cause)                    when the chip resets; the default restores every register's
//                                      reset value (an always-on IP keeps what survives)
// and marks the registers it keeps up to date with set_modelled(reg).
//
// A peripheral model also writes debug lines for software engineers (dbg, dbgf: Eg2System::debug,
// <instance>.debug.log), one per interrupt line that rises once it calls debug_irqs(), and can
// stop a run that waits for a device outside the chip (poll_watch).

#ifndef EG2_MODEL_EG2_IP_MODEL_H_
#define EG2_MODEL_EG2_IP_MODEL_H_

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "eg2_model_irqs.h"
#include "eg2_model_regs.h"
#include "eg2_regblock.h"
#include "eg2_system.h"

class Eg2IpModel : public Eg2RegBlock {
 public:
  Eg2IpModel(sc_core::sc_module_name name, const eg2_regs::Block &desc, uint32_t base)
      : Eg2RegBlock(name, desc, base), sys_(Eg2System::get()) {
    for (size_t i = 0; i < sys_.nips; ++i) {
      if (std::string(desc.window) == sys_.ips[i].window) irq_ = &sys_.ips[i];
    }
    intr_state_ = reg_index("INTR_STATE");
    intr_enable_ = reg_index("INTR_ENABLE");
    intr_test_ = reg_index("INTR_TEST");
    alert_test_ = reg_index("ALERT_TEST");
    if (alert_test_ >= 0) {
      // Reggen's ALERT_TEST has an optional write-zero-to-clear lock in bit
      // 31. Inspect the description so older boards without it still work.
      const auto &test = desc.regs[alert_test_];
      for (uint16_t i = 0; i < test.nfields; ++i) {
        const auto &field = desc.fields[test.field0 + i];
        if (field.lsb == 31 && field.width == 1 && field.sw == eg2_regs::kAccRw0c) {
          alert_test_lock_ = 1u << 31;
        }
      }
    }
    if (intr_state_ >= 0) set_modelled(uint16_t(intr_state_));
    for (const char *a : eg2_irqs::kAonModules) {
      if (std::string(module()) == a) aon_ = true;
    }
    status_level_ = 0;
    status_test_ = 0;
    sys_.add(this);
  }

  const std::string &window() const { return window_ = desc().window; }
  const char *module() const { return irq_ ? irq_->module : desc().ip; }

  // ---- Interrupts ----
  // Set an event-type interrupt by its bit (or its name).
  void set_intr(unsigned bit) {
    if (intr_state_ < 0) return;
    set_value(uint16_t(intr_state_), value(uint16_t(intr_state_)) | (1u << bit));
    update_irqs();
  }
  void set_intr(const char *name) {
    int b = intr_bit(name);
    if (b >= 0) set_intr(unsigned(b));
  }
  // A status-type interrupt's source level.
  void set_intr_level(unsigned bit, bool level) {
    status_level_ = level ? (status_level_ | (1u << bit)) : (status_level_ & ~(1u << bit));
    if (intr_state_ < 0) return;
    uint32_t mask = status_mask();
    uint32_t v = (value(uint16_t(intr_state_)) & ~mask) | ((status_level_ | status_test_) & mask);
    set_value(uint16_t(intr_state_), v);
    update_irqs();
  }
  void set_intr_level(const char *name, bool level) {
    int b = intr_bit(name);
    if (b >= 0) set_intr_level(unsigned(b), level);
  }
  int intr_bit(const char *name) const {
    for (uint8_t i = 0; irq_ && i < irq_->nintrs; ++i) {
      if (std::string(name) == irq_->intrs[i].name) return irq_->intrs[i].bit;
    }
    return -1;
  }
  // The interrupt lines (state & enable) into rv_plic. A subclass whose IP has no standard
  // INTR_STATE / INTR_ENABLE (rv_timer) drives them itself with drive_irq. Lines the top ORs into
  // one source (Sonata: every UART interrupt on one) drive it together. irq_out, when set, also
  // gets each line (an interrupt another block takes: the TBRE's tbre_done into rev_ctl's shim).
  virtual void update_irqs() {
    if (!irq_ || intr_state_ < 0) return;
    uint32_t en = intr_enable_ >= 0 ? value(uint16_t(intr_enable_)) : 0xffffffffu;
    uint32_t lines = value(uint16_t(intr_state_)) & en;
    for (uint8_t i = 0; i < irq_->nintrs; ++i) {
      bool level = false;
      for (uint8_t j = 0; j < irq_->nintrs; ++j) {
        if (irq_->intrs[j].plic_id == irq_->intrs[i].plic_id)
          level = level || ((lines >> irq_->intrs[j].bit) & 1);
      }
      drive_irq(i, level);
      if (irq_out) irq_out(i, (lines >> irq_->intrs[i].bit) & 1);
    }
    if (debug_irqs_) debug_irq_lines(lines);
  }
  void drive_irq(uint8_t i, bool level) { sys_.set_plic_source(irq_->intrs[i].plic_id, level); }
  std::function<void(uint8_t, bool)> irq_out;

  // ---- Alerts ----
  void raise_alert(unsigned bit) {
    for (uint8_t i = 0; irq_ && i < irq_->nalerts; ++i) {
      if (irq_->alerts[i].bit == bit) sys_.raise_alert(irq_->alerts[i].id);
    }
  }
  void raise_alert(const char *name) {
    for (uint8_t i = 0; irq_ && i < irq_->nalerts; ++i) {
      if (std::string(name) == irq_->alerts[i].name) sys_.raise_alert(irq_->alerts[i].id);
    }
  }
  // A fault in one of the IP's countermeasures (the chip bench's sec_cm fault injection,
  // chip_sw_all_escalation_resets_vseq): the IP raises the fatal alert the countermeasure feeds
  // (comportability README "Alerts": fatal alerts "are continuously triggered until reset"). An IP
  // whose documentation gives such a fault a status field overrides this and sets it too. false:
  // the IP has no alert of that name.
  virtual bool fatal_fault(const std::string &alert) {
    for (uint8_t i = 0; irq_ && i < irq_->nalerts; ++i) {
      if (alert == irq_->alerts[i].name) {
        fatal_alerts_.push_back(irq_->alerts[i].id);
        sys_.raise_alert(irq_->alerts[i].id);
        return true;
      }
    }
    return false;
  }
  // The fatal alerts a countermeasure fault holds until the IP is reset (fatal_fault).
  std::vector<uint16_t> fatal_alerts_;

  // ---- Time and reset ----
  virtual void on_time(uint64_t /*now_ps*/) {}
  // A clock's frequency changed (Eg2System::set_clock_hz): re-time what is scheduled by it.
  virtual void on_clock_change() {}
  // The chip is about to reset: every model still has its state (rstmgr captures the crash dump).
  virtual void before_reset(Eg2ResetCause /*cause*/) {}
  // Whether the IP keeps its state through a reset of this cause (rstmgr theory_of_operation.md
  // "Reset Information"): a low-power exit resets "only the non-always-on domains"; a hardware or
  // software reset request "means everything other than power / clock / reset managers have
  // reset" (the "Summary" table: the LC and SYS trees of all domains); a power-on reset resets
  // everything. For NDM reset, pinmux theory_of_operation.md "Non-debug Module Reset" says the
  // strap/TAP module, rv_dm, power, reset and clock managers are the only parts not reset.
  bool keeps_state(Eg2ResetCause cause) const {
    if (cause == Eg2ResetCause::kPor) return false;
    if (cause == Eg2ResetCause::kLowPowerExit) return aon_;
    std::string m = module();
    // A non-debug-module reset resets "all logic except the debug module and associated glue logic"
    // (rstmgr theory_of_operation.md "System Reset Tree"): rv_dm, on the sys tree
    // (top_earlgrey.hjson reset_connections), keeps its state.
    if (cause == Eg2ResetCause::kNdm && m == "rv_dm") return true;
    // clkmgr: only its dividers are on the por domain; "the other functions inside the clock
    // manager operate on the life cycle reset domain", so "a user or escalation initiated reset
    // still restores the clock manager to a default clean slate" (clkmgr theory_of_operation.md
    // "Reset Domains"; rstmgr "Summary": HW and SW reset requests reset the LC tree). The NDM rule
    // above (pinmux) keeps it.
    if (m == "clkmgr") return cause == Eg2ResetCause::kNdm;
    return m == "pwrmgr" || m == "rstmgr";
  }
  virtual void on_reset(Eg2ResetCause cause) {
    if (keeps_state(cause)) return;
    reset();
    status_level_ = 0;
    status_test_ = 0;
    fatal_alerts_.clear();
    polls_.clear();
    sys_.unschedule(this);
    update_irqs();
  }
  bool aon() const { return aon_; }
  // Whether the IP is idle, for clkmgr's transactional clock hints (an IP with work in flight
  // overrides it).
  virtual bool idle() const { return true; }

  // An access while clkmgr has the block's clock off never completes: the core stalls on it until
  // a reset (Eg2System::clock_off; clkmgr's "Access to disabled unit should freeze").
  bool access_frozen() override {
    std::string inst = desc().window;
    inst = inst.substr(0, inst.find('.'));
    if (sys_.clock_off && sys_.clock_off(inst)) {
      sys_.halt_core("clkmgr", "access to " + inst + " while its clock is off");
      return true;
    }
    // Likewise a block rstmgr holds in reset (theory_of_operation.md "Hung Transaction": "accessing
    // a device whose clock is not turned on or is under reset").
    if (sys_.held_in_reset && sys_.held_in_reset(inst)) {
      sys_.halt_core("rstmgr", "access to " + inst + " while it is held in reset");
      return true;
    }
    return false;
  }

 protected:
  // Subclass hooks (see the header comment).
  virtual void reg_write(uint16_t /*reg*/, uint32_t /*old*/, uint32_t /*wdata*/,
                         uint32_t /*wmask*/) {}
  virtual void reg_before_read(uint16_t /*reg*/) {}
  virtual void reg_after_read(uint16_t /*reg*/) {}
  virtual void win_write(uint16_t /*win*/, uint32_t /*off*/, unsigned /*len*/) {}
  virtual bool win_read(uint16_t /*win*/, uint32_t /*off*/, uint8_t * /*data*/, unsigned /*len*/) {
    return false;
  }
  virtual bool reg_before_write(uint16_t /*reg*/, uint32_t /*wdata*/, uint32_t /*wmask*/) {
    return true;
  }

  // A register's index, or a stop with a clear reason if the hjson has no such register.
  uint16_t reg(const char *name) {
    int r = reg_index(name);
    if (r < 0) {
      Eg2ModelStop::request(std::string(desc().window) + ": the model needs register " + name +
                            ", which the hjson does not have");
      return 0;
    }
    return uint16_t(r);
  }
  bool written(uint16_t r, uint32_t wdata, uint32_t wmask, uint32_t bits) const {
    (void)r;
    return (wdata & wmask & bits) != 0;
  }

  // ---- Debug lines (Eg2System::debug) of a peripheral model, under its instance name ----
  std::string inst() const {
    std::string w = desc().window;
    return w.substr(0, w.find('.'));
  }
  bool dbg_on(int level) const { return sys_.debug_on(level); }
  void dbg(int level, const std::string &text) { sys_.debug(inst(), level, text); }
  void dbgf(int level, const char *fmt, ...) __attribute__((format(printf, 3, 4))) {
    if (!sys_.debug_on(level)) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    sys_.debug(inst(), level, b);
  }
  // Whether to print the next level-1 line of a kind that can repeat at a high rate (key names
  // it): the first `max`, then one line saying the rest are not printed; at level 2 all of them.
  bool dbg_count(const std::string &key, unsigned max) {
    if (!dbg_on(1)) return false;
    unsigned n = ++dbg_counts_[key];
    if (n <= max || dbg_on(2)) return true;
    if (n == max + 1) dbg(1, key + ": further lines are not printed (--periph-debug 2 prints them)");
    return false;
  }
  // A debug line each time one of the model's interrupt lines into rv_plic rises (INTR_STATE and
  // INTR_ENABLE both set). A status-type interrupt (a FIFO level) can rise once per byte: at level
  // 1 the first kIrqLines rises of each are printed (kIrqEvents of an event-type one).
  static constexpr unsigned kIrqLines = 4, kIrqEvents = 64;
  void debug_irqs() { debug_irqs_ = true; }

  // Software that waits for something only a device outside the chip can do: a read of one of the
  // model's registers, the kPollLimit-th since the last software write to the block, stops the run
  // with why() (eg2_regblock.h stops a poll of a register no model keeps the same way). For a
  // model whose registers nothing but software and the outside changes.
  void poll_watch(uint16_t r, const std::function<std::string()> &why) {
    poll_watch(r, desc().regs[r].name, why);
  }
  // The same for something else software reads (a FIFO port in a window), by its own key (above
  // the register indices) and name.
  void poll_watch(uint32_t key, const std::string &name, const std::function<std::string()> &why) {
    if (++polls_[key] != kPollLimit) return;
    Eg2ModelStop::request(inst() + ": software polls " + name + " (read " +
                          std::to_string(kPollLimit) + " times with no write to the block): " +
                          why());
  }
  void polls_reset() { polls_.clear(); }

  Eg2System &sys_;
  const eg2_irqs::Ip *irq_ = nullptr;
  int intr_state_ = -1, intr_enable_ = -1, intr_test_ = -1, alert_test_ = -1;
  bool aon_ = false;

 private:
  void debug_irq_lines(uint32_t lines) {
    uint32_t rose = lines & ~irq_lines_;
    irq_lines_ = lines;
    for (uint8_t i = 0; rose && irq_ && i < irq_->nintrs; ++i) {
      const eg2_irqs::Intr &in = irq_->intrs[i];
      if (!((rose >> in.bit) & 1)) continue;
      if (!dbg_count(std::string("interrupt ") + in.name, in.status ? kIrqLines : kIrqEvents))
        continue;
      dbgf(1, "interrupt %s raised (INTR_STATE bit %u, enabled)", in.name, unsigned(in.bit));
    }
  }
  uint32_t status_mask() const {
    uint32_t m = 0;
    for (uint8_t i = 0; irq_ && i < irq_->nintrs; ++i) {
      if (irq_->intrs[i].status) m |= 1u << irq_->intrs[i].bit;
    }
    return m;
  }

  void before_read(uint16_t r) override { reg_before_read(r); }
  void after_read(uint16_t r) override { reg_after_read(r); }
  void on_win_write(uint16_t w, uint32_t off, unsigned len) override { win_write(w, off, len); }
  bool on_win_read(uint16_t w, uint32_t off, uint8_t *data, unsigned len) override {
    return win_read(w, off, data, len);
  }
  bool before_write(uint16_t r, uint32_t wdata, uint32_t wmask) override {
    return reg_before_write(r, wdata, wmask);
  }

  void on_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    polls_.clear();
    if (int(r) == intr_test_ && irq_) {
      uint32_t t = wdata & wmask;
      uint32_t st = status_mask();
      if (intr_state_ >= 0 && (t & ~st)) {
        set_value(uint16_t(intr_state_), value(uint16_t(intr_state_)) | (t & ~st));
      }
      // A status-type interrupt keeps its test bit until the next INTR_TEST write replaces it, and
      // shows it in INTR_STATE (prim_intr_hw "Status": test_q). It was a one-cycle pulse here, so
      // INTR_STATE read 0 after the test (2026-10-29: plic_all_irqs_test_0/10/20, adc_ctrl,
      // i3c0 and spi_host0, "Actual interrupt status = 0").
      status_test_ = t & st;
      if (intr_state_ >= 0) {
        set_value(uint16_t(intr_state_), (value(uint16_t(intr_state_)) & ~st) |
                                             ((status_level_ | status_test_) & st));
      }
      update_irqs();
    } else if (int(r) == alert_test_) {
      // The previous lock value gates this write: clearing the lock and testing
      // an alert in the same write is allowed. Subsequent tests are disabled
      // until reset (util/reggen/reg_block.py, generated ALERT_TEST description).
      // Same-write ordering is confirmed by uart_reg_top.sv's
      // alert_test_gated_we = alert_test_we && alert_test_regwen_qs.
      if (!alert_test_lock_ || (old & alert_test_lock_)) {
        for (uint8_t i = 0; irq_ && i < irq_->nalerts; ++i) {
          if ((wdata & wmask) >> irq_->alerts[i].bit & 1) sys_.raise_alert(irq_->alerts[i].id);
        }
      }
    } else if (int(r) == intr_state_) {
      // Status-type bits follow their source: a write of 1 does not clear them.
      uint32_t st = status_mask();
      set_value(r, (value(r) & ~st) | ((status_level_ | status_test_) & st));
      update_irqs();
    } else if (int(r) == intr_enable_) {
      update_irqs();
    }
    reg_write(r, old, wdata, wmask);
  }

  uint32_t status_level_;
  uint32_t alert_test_lock_ = 0;
  uint32_t status_test_;  // status-type INTR_TEST bits, held until the next INTR_TEST write
  mutable std::string window_;
  bool debug_irqs_ = false;
  uint32_t irq_lines_ = 0;
  std::map<std::string, unsigned> dbg_counts_;
  std::map<uint32_t, uint64_t> polls_;
};

// Each model due by now acts once; what it schedules again is for a later call, so a model that
// asks for a time already past cannot hold the core.
inline void Eg2System::run_due(uint64_t now) {
  std::vector<Eg2IpModel *> due;
  for (auto it = events_.begin(); it != events_.end();) {
    if (it->second <= now) {
      due.push_back(it->first);
      it = events_.erase(it);
    } else {
      ++it;
    }
  }
  for (Eg2IpModel *m : due) m->on_time(now);
  std::vector<std::function<void()>> fns;
  while (!timed_.empty() && timed_.begin()->first <= now) {
    fns.push_back(std::move(timed_.begin()->second));
    timed_.erase(timed_.begin());
  }
  for (auto &f : fns) f();
}

inline void Eg2System::apply_reset(Eg2ResetCause cause) {
  for (Eg2IpModel *m : models_) m->before_reset(cause);
  // A reset releases the old halt and escalation sources before models sample their new reset
  // state. A model such as lc_ctrl may immediately halt the core again for the state decoded from
  // OTP; clearing after on_reset would incorrectly let RAW, TEST_LOCKED and SCRAP execute.
  halts_.clear();
  for (bool &e : esc_) e = false;
  for (Eg2IpModel *m : models_) m->on_reset(cause);
}

inline void Eg2System::clock_changed() {
  for (Eg2IpModel *m : models_) m->on_clock_change();
}

inline Eg2IpModel *Eg2System::model(const std::string &window) const {
  for (Eg2IpModel *m : models_) {
    if (window == m->desc().window) return m;
  }
  return nullptr;
}

// The model of a register window: the IP's behaviour if eg2_ip_models.cc has one, the generic
// model (registers, interrupts, alerts) otherwise.
Eg2IpModel *make_ip_model(const char *inst, const eg2_regs::Block &desc, uint32_t base);

#endif  // EG2_MODEL_EG2_IP_MODEL_H_
