// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2RegBlock: a device's register block in the EG2 SystemC model, from its generated description
// (eg2_model_regs.h, from the IP's hjson by reggen). Milestone M2 of the system model plan: every
// register has its reset value and every field its software access; writes are gated by the
// register's REGWEN; the memory windows inside the block are plain memory. What hardware does to a
// register is not modelled yet: a register hardware updates keeps the value software or reset gave
// it. on_read and on_write are the hooks milestone M3 fills, per block, by register index
// (eg2_regs::<block>::k<Register>).
//
// An access to an offset with no register and no window answers TLM_ADDRESS_ERROR_RESPONSE, as the
// register block answers d_error.
//
// Milestone M3 builds the IP models on this block (eg2_ip_model.h): before_read lets a model bring a
// register up to date before software reads it (a timer's count), after_read and on_write let it act
// on the access, before_write lets it refuse a write; on_win_read lets it answer a read of one of the
// block's memory windows itself (a FIFO behind a window pops on a read), on_win_write acts on a
// write into one. A register a model keeps up to date is marked modelled (set_modelled) and no
// longer counts as polled.
//
// Any hook can make the access answer an error (fail_access): the bus then gets
// TLM_ADDRESS_ERROR_RESPONSE, as the block answers d_error, and the core takes its access fault.
//
// To find what to model next, the block counts reads of the registers hardware updates. A read
// loop on one of them (the software polls for hardware that is not there) stops the run once the
// register has been read kPollLimit times, naming it.

#ifndef EG2_MODEL_EG2_REGBLOCK_H_
#define EG2_MODEL_EG2_REGBLOCK_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <map>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

#include "eg2_model_regs.h"
#include "eg2_targets.h"

class Eg2RegBlock : public sc_core::sc_module {
 public:
  static constexpr uint64_t kPollLimit = 100000;

  tlm_utils::simple_target_socket<Eg2RegBlock> socket;

  Eg2RegBlock(sc_core::sc_module_name name, const eg2_regs::Block &desc, uint32_t base)
      : sc_core::sc_module(name), socket("socket"), desc_(desc), base_(base),
        val_(desc.nregs), reads_(desc.nregs, 0) {
    socket.register_b_transport(this, &Eg2RegBlock::b_transport);
    for (uint16_t i = 0; i < desc_.nregs; ++i) by_offset_[desc_.regs[i].offset] = i;
    for (uint16_t i = 0; i < desc_.nwins; ++i) win_mem_.emplace_back(desc_.wins[i].size, 0);
    reset();
  }

  virtual ~Eg2RegBlock() = default;

  void reset() {
    for (uint16_t i = 0; i < desc_.nregs; ++i) val_[i] = desc_.regs[i].resval;
  }

  // A window's memory, by index (the block's Win order).
  std::vector<uint8_t> &win_mem(uint16_t win) { return win_mem_[win]; }
  int win_index(const std::string &name) const {
    for (uint16_t i = 0; i < desc_.nwins; ++i) {
      if (name == desc_.wins[i].name) return i;
    }
    return -1;
  }

  // A register a model keeps up to date: reads of it are not counted as polling.
  void set_modelled(uint16_t reg) { modelled_.resize(desc_.nregs, false); modelled_[reg] = true; }

  const eg2_regs::Block &desc() const { return desc_; }
  uint32_t value(uint16_t reg) const { return val_[reg]; }
  void set_value(uint16_t reg, uint32_t v) { val_[reg] = v; }

  // A register's index by name, as in the hjson (case-insensitive); -1 if there is none.
  int reg_index(const std::string &name) const {
    for (uint16_t i = 0; i < desc_.nregs; ++i) {
      if (strcasecmp(name.c_str(), desc_.regs[i].name) == 0) return i;
    }
    return -1;
  }

  // Reads of the registers hardware updates, per register, for the end-of-run report.
  std::vector<std::pair<std::string, uint64_t>> hw_reads() const {
    std::vector<std::pair<std::string, uint64_t>> r;
    for (uint16_t i = 0; i < desc_.nregs; ++i) {
      if (reads_[i] && (desc_.regs[i].flags & 4))
        r.push_back({std::string(desc_.window) + "." + desc_.regs[i].name, reads_[i]});
    }
    return r;
  }

  // Backdoor write into a memory window of the block, by window name (image loading); false if
  // the block has no such window or the bytes do not fit.
  bool win_write(const std::string &win, uint32_t off, const uint8_t *data, size_t len) {
    for (uint16_t i = 0; i < desc_.nwins; ++i) {
      if (win != desc_.wins[i].name) continue;
      if (uint64_t(off) + len > win_mem_[i].size()) return false;
      std::memcpy(&win_mem_[i][off], data, len);
      return true;
    }
    return false;
  }

  // The first access to an offset with neither a register nor a window (empty while none).
  const std::string &first_error() const { return first_error_; }

 protected:
  // M3 hooks. before_read: a register is about to be read (bring it up to date). after_read: it
  // has been read. on_write: it has been written; wdata is the data written, shifted to its byte
  // lanes, and wmask the lanes written (the register's new value is value(reg)).
  virtual void before_read(uint16_t /*reg*/) {}
  virtual void after_read(uint16_t /*reg*/) {}
  virtual void on_write(uint16_t /*reg*/, uint32_t /*old_value*/, uint32_t /*wdata*/,
                        uint32_t /*wmask*/) {}
  // A register is about to be written: return false to leave it unchanged (call fail_access too
  // for an error answer).
  virtual bool before_write(uint16_t /*reg*/, uint32_t /*wdata*/, uint32_t /*wmask*/) { return true; }
  // A software write of len bytes at off into memory window win.
  virtual void on_win_write(uint16_t /*win*/, uint32_t /*off*/, unsigned /*len*/) {}
  // A software read of len bytes at off (from the window's start) in memory window win: return
  // true after filling data to answer it, false to read the window's memory.
  virtual bool on_win_read(uint16_t /*win*/, uint32_t /*off*/, uint8_t * /*data*/,
                           unsigned /*len*/) {
    return false;
  }
  // From any hook: the access in progress answers an error.
  void fail_access() { fail_ = true; }
  // True if the block cannot answer an access now (its clock is off: eg2_ip_model.h).
  virtual bool access_frozen() { return false; }

 private:
  void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &) {
    if (access_frozen()) {  // the block's clock is off: the access never completes
      trans.set_response_status(tlm::TLM_OK_RESPONSE);
      return;
    }
    fail_ = false;
    access(trans);
    trans.set_response_status(fail_ ? tlm::TLM_ADDRESS_ERROR_RESPONSE : tlm::TLM_OK_RESPONSE);
  }

  void access(tlm::tlm_generic_payload &trans) {
    uint32_t off = uint32_t(trans.get_address());
    unsigned len = trans.get_data_length();
    uint8_t *data = trans.get_data_ptr();
    for (uint16_t i = 0; i < desc_.nwins; ++i) {
      const eg2_regs::Win &w = desc_.wins[i];
      if (off >= w.offset && uint64_t(off - w.offset) + len <= w.size) {
        uint8_t *m = &win_mem_[i][off - w.offset];
        if (trans.is_read()) {
          if (!on_win_read(i, off - w.offset, data, len)) std::memcpy(data, m, len);
        } else if (w.writable) {
          std::memcpy(m, data, len);
          on_win_write(i, off - w.offset, len);
        }
        return;
      }
    }
    auto it = by_offset_.find(off & ~3u);
    if (it == by_offset_.end() || (off & 3u) + len > 4) {
      if (first_error_.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s: %s at 0x%08x, no register there", desc_.window,
                      trans.is_write() ? "store" : "load", unsigned(base_ + off));
        first_error_ = buf;
      }
      fail_ = true;
      return;
    }
    uint16_t r = it->second;
    unsigned shift = 8 * (off & 3u);
    uint32_t bytes_mask = (len >= 4 ? 0xffffffffu : ((1u << (8 * len)) - 1)) << shift;
    if (trans.is_read()) {
      before_read(r);
      uint32_t v = read_reg(r);
      v >>= shift;
      std::memcpy(data, &v, len);
      after_read(r);
    } else {
      uint32_t w = 0;
      std::memcpy(&w, data, len);
      uint32_t old = val_[r];
      if (!before_write(r, w << shift, bytes_mask)) return;
      write_reg(r, w << shift, bytes_mask);
      on_write(r, old, w << shift, bytes_mask);
    }
  }

  uint32_t read_reg(uint16_t r) {
    const eg2_regs::Reg &reg = desc_.regs[r];
    uint32_t out = 0;
    for (uint16_t k = 0; k < reg.nfields; ++k) {
      const eg2_regs::Field &f = desc_.fields[reg.field0 + k];
      uint32_t fm = field_mask(f);
      switch (f.sw) {
        case eg2_regs::kAccWo:
        case eg2_regs::kAccR0w1c:
        case eg2_regs::kAccNone:
          break;
        case eg2_regs::kAccRc:
          out |= val_[r] & fm;
          val_[r] &= ~fm;
          break;
        default:
          out |= val_[r] & fm;
      }
    }
    bool modelled = r < modelled_.size() && modelled_[r];
    if ((reg.flags & 4) && !modelled && ++reads_[r] == kPollLimit) {
      char buf[200];
      std::snprintf(buf, sizeof buf,
                    "software polls %s.%s (read %llu times): hardware updates it, no model yet",
                    desc_.window, reg.name, (unsigned long long)kPollLimit);
      Eg2ModelStop::request(buf);
    }
    return out;
  }

  void write_reg(uint16_t r, uint32_t w, uint32_t bytes_mask) {
    const eg2_regs::Reg &reg = desc_.regs[r];
    if (reg.regwen >= 0 && (val_[reg.regwen] & reg.regwen_mask) != reg.regwen_true) return;
    uint32_t v = val_[r];
    for (uint16_t k = 0; k < reg.nfields; ++k) {
      const eg2_regs::Field &f = desc_.fields[reg.field0 + k];
      uint32_t fm = field_mask(f) & bytes_mask;
      switch (f.sw) {
        case eg2_regs::kAccRw:
        case eg2_regs::kAccWo:
          v = (v & ~fm) | (w & fm);
          break;
        case eg2_regs::kAccRw1s:
          v |= w & fm;
          break;
        case eg2_regs::kAccRw1c:
        case eg2_regs::kAccR0w1c:
          v &= ~(w & fm);
          break;
        case eg2_regs::kAccRw0c:
          v &= ~(~w & fm);
          break;
        default:  // ro, rc, none: writes ignored
          break;
      }
    }
    val_[r] = v;
  }

  static uint32_t field_mask(const eg2_regs::Field &f) {
    return (f.width >= 32 ? 0xffffffffu : ((1u << f.width) - 1)) << f.lsb;
  }

  const eg2_regs::Block &desc_;
  uint32_t base_;
  std::vector<uint32_t> val_;
  std::vector<uint64_t> reads_;
  std::map<uint32_t, uint16_t> by_offset_;
  std::vector<std::vector<uint8_t>> win_mem_;
  std::vector<bool> modelled_;
  std::string first_error_;
  bool fail_ = false;
};

#endif  // EG2_MODEL_EG2_REGBLOCK_H_
