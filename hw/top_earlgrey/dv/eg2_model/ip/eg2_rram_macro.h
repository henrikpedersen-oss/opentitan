// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The RRAM macro as a behavioural model: the physical array (each RRAM word as its 128 stored
// bits and 16 ECC bits) and the documented operations of the macro interface. Not timing accurate
// (no latencies, decision 2026-11-08): each request is accepted and completes at once. Used by
// eg2sim's RRAM controller (Eg2RramCtrl, ip/eg2_mem.h), behind the shared PHY core
// (eg2_rram_phy_core.h: address XOR, scrambling) as rram_phy is behind rram_ctrl.
//
// From the documentation, never from the RTL:
//   hw/ip/rram_macro/README.md: read and write commands; "Each RRAM word is protected by ECC,
//     configurable per request via ecc_en"; "the ECC is two independent Hamming(72,64) SECDED
//     codes (one per 64-bit half of the 128-bit word)";
//   hw/ip/rram_ctrl/README.md "RRAM Controller - Macro Interface": rd_req reads the RRAM word at
//     addr in part; wr_req with wr_last low pushes one RRAM word into the store buffer at the
//     word-in-page bits of addr; wr_req with wr_last high commits the store buffer to the page addr
//     selects and carries no data; ecc_en enables ECC encoding (write) or decoding and correction
//     (read) for the request; err for "a commit with an empty store buffer or a push to a full
//     store buffer"; ecc_err one-hot0: 000 none, 001 one bit corrected, 010 two bits corrected,
//     100 uncorrectable; MaxWrWords = 32 (one page);
//   hw/ip/rram_ctrl/README.md "RRAM Memory Overview" / "Address Map": a 2 MiB data array (4096
//     pages) and a 4 KiB info array (8 pages), 32 RRAM words of 16 bytes a page; the OTP region is
//     data pages 4090-4095 (the integrity page and five OTP pages), physically ordinary rows;
//   doc/theory_of_operation.md "RRAM ECC Error Handling": one corrected error is corr1, two
//     corr2; the shadow read of rram_phy is a second read of the macro, not a macro operation;
//   prim_secded_hamming_72_64 (util/design/secded_gen.py 'hamming', k = 64): the encoder of
//     eg2_rram_otp_core.h (RramOtp::hamming72_64, shared), its eighth bit the parity of the data
//     and the seven check bits; util/design/gen-rram-img.py appends the same two codes above the 128
//     data bits ({ecc_hi, ecc_lo}), which the self-test checks against a build's .scr.vmem.
// Reading taken for the two codes (stated by the README, the encoding by secded_gen): one error in
// a half is corrected; a half with an even, non-zero syndrome, or an odd one that names no bit, is
// uncorrectable. One corrected half is 001, both 010; any uncorrectable half 100.
//
// Not in the documentation (findings in status/rtl_tickets.md / dv_todo.md); the model takes:
//   - what a never-programmed row holds: the model keeps it as "blank" (data and ECC zero, a valid
//     codeword); eg2sim shows a blank row as plaintext zero, as before;
//   - the ECC bits a write with ecc_en low leaves: zero;
//   - erase: none (the documents name read and write only; RRAM is overwritten in place);
//   - the vendor CSR block (prim_tl) and the JTAG, observability and test ports: not modelled.
//
// The OTP plug attaches through the same operations (data partition, rows of pages 4090-4095,
// ecc_en on, no scrambling, no address XOR: eg2_rram_mp_core.h otp_rule); row() and
// backdoor_write() give the plug and the image loader the rows (see the report of 2026-11-08).

#ifndef EG2_RRAM_MACRO_H_
#define EG2_RRAM_MACRO_H_

#include <cstdint>
#include <vector>

#include "eg2_rram_otp_core.h"
#include "eg2_rram_scramble_core.h"

namespace eg2_model {

class RramMacro {
 public:
  static constexpr uint32_t kWordsPerPage = 32, kDataPages = 4096, kInfoPages = 8;
  static constexpr uint32_t kMaxWrWords = 32;
  static constexpr uint32_t kDataRows = kDataPages * kWordsPerPage, kInfoRows = kInfoPages * kWordsPerPage;
  enum EccErr : uint32_t { kEccNone = 0, kEccCorr1 = 1, kEccCorr2 = 2, kEccUncorr = 4 };

  struct Row {
    rram_u128 data = 0;
    uint16_t ecc = 0;  // {ecc_hi[7:0], ecc_lo[7:0]}
    bool programmed = false;
  };
  struct Rd {
    rram_u128 data = 0;
    uint32_t ecc_err = kEccNone;
  };

  RramMacro() : data_(kDataRows), info_(kInfoRows) {}

  // The macro's ECC of a 128-bit word: one Hamming(72,64) code per 64-bit half.
  static uint16_t ecc(rram_u128 d) {
    return uint16_t(RramOtp::hamming72_64(uint64_t(d >> 64)) << 8 | RramOtp::hamming72_64(uint64_t(d)));
  }
  // One half: 0 clean, 1 corrected (in *d), 2 uncorrectable.
  static int decode_half(uint64_t *d, uint8_t e) {
    uint8_t s = uint8_t(RramOtp::hamming72_64(*d) ^ e);
    // The eighth syndrome bit is the parity of all 72 stored bits (the encoder's eighth bit makes
    // a codeword's overall parity even).
    uint8_t low = s & 0x7f;
    bool odd = ((__builtin_parityll(*d) ^ __builtin_parity(e)) & 1) != 0;
    if (s == 0) return 0;
    if (!odd) return 2;          // an even number of errors: detected, not corrected
    if (low == 0) return 1;      // the overall parity bit itself
    if ((low & (low - 1)) == 0) return 1;  // one of the seven check bits
    for (int i = 0; i < 64; ++i) {
      if (column(i) == low) {
        *d ^= uint64_t(1) << i;
        return 1;
      }
    }
    return 2;                    // names no bit: uncorrectable
  }

  // rd_req.
  Rd read(bool part, uint32_t addr, bool ecc_en) const {
    const Row &r = row(part, addr);
    Rd o;
    o.data = r.data;
    if (!ecc_en) return o;
    uint64_t lo = uint64_t(r.data), hi = uint64_t(r.data >> 64);
    int a = decode_half(&lo, uint8_t(r.ecc)), b = decode_half(&hi, uint8_t(r.ecc >> 8));
    if (a == 2 || b == 2) {
      o.ecc_err = kEccUncorr;  // the data as stored
      return o;
    }
    o.data = (rram_u128(hi) << 64) | lo;
    int n = (a == 1) + (b == 1);
    o.ecc_err = n == 0 ? kEccNone : n == 1 ? kEccCorr1 : kEccCorr2;
    return o;
  }

  // wr_req, wr_last low: one RRAM word into the store buffer. false: err (the buffer is full).
  bool push(uint32_t addr, rram_u128 d, bool ecc_en) {
    if (sbuf_.size() >= kMaxWrWords) return false;
    sbuf_.push_back({addr % kWordsPerPage, d, ecc_en});
    return true;
  }
  // wr_req, wr_last high: the store buffer into the page addr selects. false: err (empty buffer).
  // first / count: the rows written (for the owner's view), in buffer order.
  bool commit(bool part, uint32_t addr, std::vector<uint32_t> *rows = nullptr) {
    if (sbuf_.empty()) return false;
    uint32_t page = addr / kWordsPerPage;
    for (const Pushed &p : sbuf_) {
      uint32_t a = page * kWordsPerPage + p.word;
      write_row(part, a, p.data, p.ecc_en);
      if (rows) rows->push_back(a);
    }
    sbuf_.clear();
    return true;
  }
  bool store_buffer_empty() const { return sbuf_.empty(); }

  // A row as a backdoor load puts it (a built image: the data already address-infected and
  // scrambled, the macro's ECC added).
  void backdoor_write(bool part, uint32_t addr, rram_u128 d, bool ecc_en) { write_row(part, addr, d, ecc_en); }
  void backdoor_row(bool part, uint32_t addr, const Row &r) { mut(part, addr) = r; }

  // Faults (a fault injection, or a bench's backdoor flip): bit 0-127 data, 128-143 ECC.
  void flip(bool part, uint32_t addr, unsigned bit) {
    Row &r = mut(part, addr);
    if (bit < 128) r.data ^= rram_u128(1) << bit;
    else if (bit < 144) r.ecc ^= uint16_t(1u << (bit - 128));
    r.programmed = true;
  }
  void swap(bool part, uint32_t a, uint32_t b) {
    Row t = row(part, a);
    mut(part, a) = row(part, b);
    mut(part, b) = t;
  }

  const Row &row(bool part, uint32_t addr) const {
    const std::vector<Row> &v = part ? info_ : data_;
    return v[addr % v.size()];
  }
  static uint32_t rows(bool part) { return part ? kInfoRows : kDataRows; }

 private:
  struct Pushed {
    uint32_t word;
    rram_u128 data;
    bool ecc_en;
  };
  static uint8_t column(int i) {
    static uint8_t col[64];
    static bool init = false;
    if (!init) {
      for (int k = 0; k < 64; ++k) col[k] = RramOtp::hamming72_64(uint64_t(1) << k) & 0x7f;
      init = true;
    }
    return col[i];
  }
  Row &mut(bool part, uint32_t addr) {
    std::vector<Row> &v = part ? info_ : data_;
    return v[addr % v.size()];
  }
  void write_row(bool part, uint32_t addr, rram_u128 d, bool ecc_en) {
    Row &r = mut(part, addr);
    r.data = d;
    r.ecc = ecc_en ? ecc(d) : 0;
    r.programmed = true;
  }
  std::vector<Row> data_, info_;
  std::vector<Pushed> sbuf_;
};

}  // namespace eg2_model

#endif  // EG2_RRAM_MACRO_H_
