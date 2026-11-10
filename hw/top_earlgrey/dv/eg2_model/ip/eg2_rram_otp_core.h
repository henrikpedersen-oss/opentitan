// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl's OTP hardware plug (rram_ctrl_otp): the reference model of what otp_ctrl sees for each
// command, and what the OTP region of the RRAM holds afterwards. For the OTP plug unit bench
// (hw/ip/rram_ctrl/dv/edagen/rram_ctrl_otp, through dpi/eg2_rram_otp_dpi.cc).
//
// Written from the documentation only, never from the RTL:
//   hw/ip/rram_ctrl/doc/theory_of_operation.md "OTP Hardware Plug" (the commands, read-set-write,
//     MacroWriteBlankError, the integrity scheme: one Hamming(72,64) syndrome per 64 data bits in
//     the integrity page, the readback verification after OtpWrite / OtpWriteRaw, OtpZeroize
//     returning the word read back, the raw commands without the integrity page, uncorrectable
//     RRAM errors answering MacroError), "Memory Protection for OTP Hardware Plug" (the plug reaches
//     the OTP pages only, every other address is rejected), "RRAM Escalation & Disable";
//   hw/ip/rram_ctrl/README.md "Address Map": integrity page 4090 at OtpIntgStartAddr 0x1FF400, OTP
//     data pages 4091-4095 from OtpStartAddr 0x1FF600, 512 bytes a page;
//   hw/top_earlgrey/ip_autogen/otp_ctrl/doc/theory_of_operation.md "Generalized Open-source
//     Interface": a command addresses a native 16-bit OTP word, size + 1 native words, word i in
//     data bits 16i+15:16i, one in-order response per command; otp_ctrl_macro_pkg's OtpAddrShift
//     (1: a native word address is the byte address shifted right by one) and its command and error
//     encodings (the doc's command table is out of date, see status/rtl_tickets.md);
//   hw/ip/prim: prim_secded_hamming_72_64 (the code util/design/secded_gen.py builds for
//     'hamming', k = 64: the parity masks below, the eighth bit the parity of the 71 bits before
//     it; an all-zero word encodes to 0x00, an all-ones word to 0xff, as the documentation says).
//
// The model works on the six OTP pages as bytes: offset 0 is the integrity page (RRAM byte address
// 0x1FF400), offset 512 the first data byte (0x1FF600). Data byte b of the OTP address space is
// region offset 512 + b; its 64-bit block b / 8 has its syndrome at region offset b / 8 (the
// documentation says "at the offset corresponding to that data's position"; one byte per eight data
// bytes is the reading taken here, see status/rtl_tickets.md).
//
// Each answer says how much the documentation decides. Not predicted (the bench counts these and
// copies the bench memory of the command's block back into the model):
//   - a command whose words cross a 64-bit block (the integrity scheme is per block; otp_ctrl
//     aligns its accesses, nothing says what the plug does with one that is not aligned);
//   - the memory after a write that answered MacroWriteBlankError (whether the bits that could be
//     set were set is not documented), and after an RRAM error in the middle of a command;
//   - which error a write answers that both clears a bit (MacroWriteBlankError) and has a corrupted
//     readback (MacroEccUncorrError): only that it answers an error;
//   - OtpZeroize whose readback was corrupted (whether the zeroize readback is checked against the
//     integrity page is not documented);
//   - an undefined command: only that its response carries an error (FAULT_STATUS.otp_op_err), and
//     the memory afterwards;
//   - a command while RRAM access is disabled: only that its response carries an error (rram_phy
//     suppresses every new controller request, so the command cannot be carried out).

#ifndef EG2_RRAM_OTP_CORE_H_
#define EG2_RRAM_OTP_CORE_H_

#include <cstdint>
#include <cstring>

namespace eg2_model {

class RramOtp {
 public:
  static constexpr uint32_t kPageBytes = 512;
  static constexpr uint32_t kIntgPage = 4090;  // OtpStartPage
  static constexpr uint32_t kIntgBytes = kPageBytes;
  static constexpr uint32_t kDataBytes = 5 * kPageBytes;  // pages 4091-4095
  static constexpr uint32_t kRegionBytes = kIntgBytes + kDataBytes;
  static constexpr uint32_t kIntgStartAddr = kIntgPage * kPageBytes;        // 0x1FF400
  static constexpr uint32_t kOtpStartAddr = (kIntgPage + 1) * kPageBytes;   // 0x1FF600

  // otp_ctrl_macro_pkg command and error codes.
  enum Cmd : unsigned {
    kRead = 0x7a, kWrite = 0x49, kReadRaw = 0x54, kWriteRaw = 0x67, kInit = 0x20, kZeroize = 0x3d
  };
  enum Err : int {
    kNoError = 0, kMacroError = 1, kMacroEccCorrError = 2, kMacroEccUncorrError = 3,
    kMacroWriteBlankError = 4,
    // Model-only answers: the response must carry an error, which one is not documented; nothing
    // about the response is documented.
    kAnyError = -2, kUnpredicted = -1
  };

  // What the bench's controller path did during the command (only meaningful when the command ran
  // alone): an RRAM read or write error was returned, a data read after a data write was corrupted.
  struct Faults {
    bool rd_err = false, wr_err = false, readback_corrupt = false;
  };

  struct Result {
    int err = kUnpredicted;      // an Err value
    uint64_t rdata = 0;          // the expected read data
    uint64_t rdata_mask = 0;     // the bits of rdata that are predicted (0: rdata not checked)
    bool mem_predicted = true;   // the region after the command is the model's
    uint32_t blk_lo = 0, blk_hi = 0;  // data block range the command touched, [lo, hi)
  };

  RramOtp() { std::memset(mem_, 0, sizeof mem_); }

  uint8_t peek(uint32_t off) const { return off < kRegionBytes ? mem_[off] : 0; }
  void poke(uint32_t off, uint8_t v) {
    if (off < kRegionBytes) mem_[off] = v;
  }

  static bool is_cmd(unsigned c) {
    return c == kRead || c == kWrite || c == kReadRaw || c == kWriteRaw || c == kInit || c == kZeroize;
  }

  // prim_secded_hamming_72_64: the eight check bits of a 64-bit word.
  static uint8_t hamming72_64(uint64_t d) {
    static const uint64_t kMask[7] = {0xab55555556aaad5bull, 0xcd9999999b33366dull,
                                      0xf1e1e1e1e3c3c78eull, 0x01fe01fe03fc07f0ull,
                                      0x01fffe0003fff800ull, 0x01fffffffc000000ull,
                                      0xfe00000000000000ull};
    uint8_t p = 0;
    for (int i = 0; i < 7; ++i) p |= uint8_t(__builtin_parityll(d & kMask[i]) << i);
    // The eighth: parity of the 64 data bits and the seven check bits before it.
    p |= uint8_t((__builtin_parityll(d) ^ __builtin_parity(p)) << 7);
    return p;
  }

  uint64_t block(uint32_t k) const {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | mem_[kIntgBytes + 8 * k + i];
    return v;
  }
  uint16_t word16(uint32_t b) const { return uint16_t(mem_[kIntgBytes + b] | (mem_[kIntgBytes + b + 1] << 8)); }
  void set_word16(uint32_t b, uint16_t v) {
    mem_[kIntgBytes + b] = uint8_t(v);
    mem_[kIntgBytes + b + 1] = uint8_t(v >> 8);
  }
  uint8_t syndrome(uint32_t k) const { return mem_[k]; }
  void set_syndrome(uint32_t k) { mem_[k] = hamming72_64(block(k)); }

  // One command as otp_ctrl issues it. disabled: RRAM access was disabled when the command was
  // accepted.
  Result command(unsigned cmd, unsigned size, unsigned addr, uint64_t wdata, const Faults &f,
                 bool disabled) {
    Result r;
    const uint32_t n = (size & 3) + 1;
    const uint32_t b = addr << 1;  // OtpAddrShift
    r.blk_lo = b / 8;
    r.blk_hi = (b + 2 * n + 7) / 8;
    if (disabled || !is_cmd(cmd)) {
      r.err = kAnyError;
      r.mem_predicted = disabled;  // nothing reaches the RRAM while disabled
      return r;
    }
    if (cmd == kInit) {
      r.err = kNoError;
      return r;
    }
    // Outside the OTP data pages: rejected by memory protection (HwOtpDataCfg), nothing changes.
    if (b >= kDataBytes) {
      r.err = kMacroError;
      return r;
    }
    if ((b % 8) + 2 * n > 8) {  // crosses a 64-bit block: not predicted
      r.err = kUnpredicted;
      r.mem_predicted = false;
      return r;
    }
    const uint32_t k = b / 8;
    if (f.rd_err || f.wr_err) {
      r.err = kMacroError;
      r.mem_predicted = false;
      return r;
    }
    const uint64_t wmask = n == 4 ? ~0ull : ((1ull << (16 * n)) - 1);
    switch (cmd) {
      case kRead:
      case kReadRaw: {
        uint64_t d = 0;
        for (uint32_t i = 0; i < n; ++i) d |= uint64_t(word16(b + 2 * i)) << (16 * i);
        if (cmd == kRead && hamming72_64(block(k)) != syndrome(k)) {
          r.err = kMacroEccUncorrError;
          return r;
        }
        r.err = kNoError;
        r.rdata = d;
        r.rdata_mask = wmask;
        return r;
      }
      case kWrite:
      case kWriteRaw: {
        bool blank = false;
        for (uint32_t i = 0; i < n; ++i) {
          uint16_t old = word16(b + 2 * i), nw = uint16_t(wdata >> (16 * i));
          if (old & uint16_t(~nw)) blank = true;
        }
        if (blank) {
          // With a corrupted readback as well, both errors happen; which one is answered is not
          // documented.
          r.err = f.readback_corrupt ? kAnyError : kMacroWriteBlankError;
          r.mem_predicted = false;
          return r;
        }
        for (uint32_t i = 0; i < n; ++i) set_word16(b + 2 * i, uint16_t(wdata >> (16 * i)));
        if (cmd == kWrite) set_syndrome(k);
        r.err = f.readback_corrupt ? kMacroEccUncorrError : kNoError;
        return r;
      }
      case kZeroize: {
        for (uint32_t i = 0; i < n; ++i) set_word16(b + 2 * i, 0xffff);
        set_syndrome(k);
        if (f.readback_corrupt) {
          r.err = kUnpredicted;
          return r;
        }
        r.err = kNoError;
        r.rdata = wmask;
        r.rdata_mask = wmask;
        return r;
      }
      default:
        break;
    }
    r.err = kAnyError;
    r.mem_predicted = false;
    return r;
  }

 private:
  uint8_t mem_[kRegionBytes];
};

}  // namespace eg2_model

#endif  // EG2_RRAM_OTP_CORE_H_
