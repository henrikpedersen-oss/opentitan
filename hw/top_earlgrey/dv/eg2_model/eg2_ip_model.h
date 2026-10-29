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

#ifndef EG2_MODEL_EG2_IP_MODEL_H_
#define EG2_MODEL_EG2_IP_MODEL_H_

#include <cstdint>
#include <functional>
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

  // ---- Time and reset ----
  virtual void on_time(uint64_t /*now_ps*/) {}
  // A clock's frequency changed (Eg2System::set_clock_hz): re-time what is scheduled by it.
  virtual void on_clock_change() {}
  // The chip is about to reset: every model still has its state (rstmgr captures the crash dump).
  virtual void before_reset(Eg2ResetCause /*cause*/) {}
  // An always-on IP keeps its state through every reset but a power-on reset.
  virtual void on_reset(Eg2ResetCause cause) {
    if (aon_ && cause != Eg2ResetCause::kPor) return;
    reset();
    status_level_ = 0;
    status_test_ = 0;
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

  Eg2System &sys_;
  const eg2_irqs::Ip *irq_ = nullptr;
  int intr_state_ = -1, intr_enable_ = -1, intr_test_ = -1, alert_test_ = -1;
  bool aon_ = false;

 private:
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
      for (uint8_t i = 0; irq_ && i < irq_->nalerts; ++i) {
        if ((wdata & wmask) >> irq_->alerts[i].bit & 1) sys_.raise_alert(irq_->alerts[i].id);
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
  uint32_t status_test_;  // status-type INTR_TEST bits, held until the next INTR_TEST write
  mutable std::string window_;
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
