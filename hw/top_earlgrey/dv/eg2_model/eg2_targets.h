// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// TLM-2.0 targets of the EG2 SystemC model (doc/system_model in eg2_top_verification):
//   Eg2Memory      a memory (ROM, SRAM, RRAM, the revocation bitmap window): b_transport and DMI;
//   Eg2RegBlock    a device's register block (eg2_regblock.h).
// Eg2ModelStop holds the reason a target stops the run (for example software polling a register
// no model updates), so a gap in the model is never mistaken for behaviour.
// Addresses a target sees are offsets into its window (Eg2Router subtracts the base).

#ifndef EG2_MODEL_EG2_TARGETS_H_
#define EG2_MODEL_EG2_TARGETS_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

class Eg2Memory : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<Eg2Memory> socket;

  Eg2Memory(sc_core::sc_module_name name, uint32_t base, uint32_t size)
      : sc_core::sc_module(name), socket("socket"), base_(base), size_(size), mem_(size, 0) {
    socket.register_b_transport(this, &Eg2Memory::b_transport);
    socket.register_get_direct_mem_ptr(this, &Eg2Memory::get_direct_mem_ptr);
  }

  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  // The memory's bytes, for the IP that owns it (sram_ctrl's initialisation, for example). A
  // window onto another memory (share_start_of) gives that memory's bytes.
  std::vector<uint8_t> &data() { return backing_ ? backing_->data() : mem_; }

  // This window is the start of another memory: the same bytes, its own size. The revocation
  // bitmap window cheriot_mem_sys.revbm is the base of the meta SRAM (top_earlgrey.hjson:
  // sram_ctrl_meta's ram "aliases cheriot_mem_sys.revbm on purpose"), so sram_ctrl_meta's
  // initialisation zeroes the bitmap software reads through the window.
  void share_start_of(Eg2Memory &backing) {
    if (backing.size() >= size_) backing_ = &backing;
  }

  // Only full-word writes get through: a write of part of a word is answered with an error and
  // changes nothing. cheriot_mem_sys's revbm window (theory_of_operation.md: "Accesses must be full
  // 32-bit words"; "Access Checkers": word-aligned, a full word, opcode Get or PutFullData;
  // cheriot_access_check.sv). The core's host adapter (tlul_adapter_host) sends every access as an
  // aligned word, so a byte or halfword load is a full-word Get and goes through, while a byte or
  // halfword store is a PutPartialData and is refused. No direct memory pointer then, so every
  // bus access passes the check.
  void set_word_only() { word_only_ = true; }

  // A read-only window: a write is answered with an error and changes nothing. rram_ctrl's host
  // window (README.md "Features": "`host_tl` is a read-only interface limited to the data
  // partition"; 2026-11-07, chip_sw_rram_host_wr_reject: the chip refused the store, the model took
  // it).
  void set_read_only() { read_only_ = true; }
  // The core model sets this around a capability store with its tag set (strict CHERIoT mode), so a
  // read-only window can apply cheriot_mem_sys's WTRC (theory_of_operation.md "Write-to-Read-and-
  // Compare Filter (WTRC)": "The NVM cannot be written through the interconnect"; a capability store
  // to the NVM "is not written: it is turned into a Get of the word it targets ... and succeeds only
  // if the NVM already holds its data"; a store that is not a full-word PutFullData, or whose data
  // differs, gets d_error). SystemC runs one process at a time, so one flag serves.
  static bool &cap_store_tagged() {
    static bool f = false;
    return f;
  }
  // Offsets [lo, hi) of the window that answer every access with an error. rram_ctrl's OTP region
  // in the host window (README.md "RRAM Memory Overview": "host reads to it return a TL-UL error"; pages 4090
  // to 4095, from OtpIntgStartAddr 0x1FF400; 2026-11-07, chip_sw_rram_otp_isolation). No direct
  // memory pointer covers it.
  void set_error_range(uint32_t lo, uint32_t hi) { err_lo_ = lo; err_hi_ = hi; }
  // Whether an access at offset `off` is refused (a write to a read-only window, any access to the
  // error range): the core models without a bus transaction per access ask before using a direct
  // pointer (Eg2System::bus_refuses).
  bool refuses(uint32_t off, bool write) const {
    return (write && read_only_) || (off >= err_lo_ && off < err_hi_);
  }

  // Backdoor write at an absolute address (image loading); false outside the memory.
  bool write(uint32_t addr, const uint8_t *data, size_t len) {
    if (addr < base_ || uint64_t(addr - base_) + len > size_) return false;
    std::memcpy(bytes() + (addr - base_), data, len);
    return true;
  }
  // Backdoor read at an absolute address (the bench's backdoor reads); false outside the memory.
  bool read(uint32_t addr, uint8_t *out, size_t len) {
    if (addr < base_ || uint64_t(addr - base_) + len > size_) return false;
    std::memcpy(out, bytes() + (addr - base_), len);
    return true;
  }

 private:
  uint8_t *bytes() { return data().data(); }

  void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &) {
    uint64_t off = trans.get_address();
    unsigned len = trans.get_data_length();
    if (off + len > size_ || trans.get_byte_enable_ptr() ||
        (word_only_ && trans.is_write() && (off % 4 != 0 || len == 0 || len % 4 != 0)) ||
        (read_only_ && trans.is_write() && !cap_store_tagged()) ||
        (len && off < err_hi_ && off + len > err_lo_)) {
      trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    if (read_only_ && trans.is_write()) {
      // A tagged capability store (WTRC): nothing is written; full words, matching data, or an error.
      bool ok = off % 4 == 0 && len % 4 == 0 &&
                std::memcmp(bytes() + off, trans.get_data_ptr(), len) == 0;
      trans.set_response_status(ok ? tlm::TLM_OK_RESPONSE : tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    if (trans.is_read()) std::memcpy(trans.get_data_ptr(), bytes() + off, len);
    if (trans.is_write()) std::memcpy(bytes() + off, trans.get_data_ptr(), len);
    trans.set_dmi_allowed(!word_only_);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
  }

  bool get_direct_mem_ptr(tlm::tlm_generic_payload &trans, tlm::tlm_dmi &dmi) {
    if (word_only_) return false;
    uint64_t off = trans.get_address();
    uint64_t start = 0, end = size_ - 1;
    if (err_hi_ > err_lo_) {
      if (off >= err_lo_ && off < err_hi_) return false;  // the error range: never a direct pointer
      if (off < err_lo_) end = err_lo_ - 1;
      else start = err_hi_;
    }
    dmi.set_dmi_ptr(bytes() + start);
    dmi.set_start_address(start);
    dmi.set_end_address(end);
    if (read_only_) dmi.allow_read(); else dmi.allow_read_write();
    return true;
  }

  uint32_t base_, size_;
  std::vector<uint8_t> mem_;
  Eg2Memory *backing_ = nullptr;
  bool word_only_ = false;
  bool read_only_ = false;
  uint32_t err_lo_ = 0, err_hi_ = 0;
};

// rram_ctrl's data partition in the host window: 512-byte pages; the OTP region is pages 4090 to
// 4095 (README.md "RRAM Memory Overview": OTP integrity from OtpIntgStartAddr 0x1FF400, then OTP, "Accessible by" otp-hw-if only), unreachable
// from the host.
constexpr uint32_t kRramPageBytes = 512, kRramOtpFirstPage = 4090, kRramOtpLastPage1 = 4096;

// Why a target stopped the run: the first request wins; empty while none.
class Eg2ModelStop {
 public:
  static void request(const std::string &why) {
    if (reason().empty()) reason() = why;
  }
  static std::string &reason() {
    static std::string s;
    return s;
  }
};

#endif  // EG2_MODEL_EG2_TARGETS_H_
