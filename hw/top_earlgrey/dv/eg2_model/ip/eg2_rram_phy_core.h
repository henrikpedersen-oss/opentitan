// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl PHY: the reference model of rram_phy's data paths, for the PHY unit bench
// (hw/ip/rram_ctrl/dv/edagen/rram_phy, through dpi/eg2_rram_phy_dpi.cc). It predicts the requests
// rram_phy sends to the RRAM macro for a controller write, and the data and error flag of every
// host or controller read, from a model of the macro's contents and the scrambler (reused from
// eg2_rram_scramble_core.h, read only).
//
// From the documentation, never from the RTL:
//   hw/ip/rram_ctrl/doc/theory_of_operation.md
//     "Write Data Path": "Each of the four 32-bit bus words is checked for bus-transmission
//       integrity, then XORed with its bus-word address (address infection). A failed integrity
//       check zero-fills the word before the XOR, rather than aborting the write." "The four
//       infected words are packed into one 128-bit RRAM word." "The 128-bit word is scrambled";
//       "Only full 128-bit RRAM words can be written."
//     "Read Data Path": descramble, "split into four 32-bit bus words"; "The 7-bit bus integrity
//       value is computed for each 32-bit word. For this computation only, the previously inserted
//       address XOR is temporarily removed. The stored/output word itself keeps the address XOR."
//       The XOR is removed outside rram_phy (tlul_adapter_host, rram_ctrl_rd).
//     "Address-XOR": "XORed with its full bus-word address"; disabled for the OTP region
//       (addr_xor_en); host accesses always with it ("Host TL accesses never go to OTP pages").
//     "RRAM ECC Error Handling": the macro's ecc_err 001 / 010 corrected (data returned normally),
//       100 uncorrectable ("The macro marks the data as uncorrectable, and the ECC decoder cannot
//       recover the original data": the model expects the response flagged as an error, never
//       returned as good data).
//     "RRAM Write Data Path": "Its internal word counter is bounded by MaxWrWords = 32 RRAM words
//       (one full page), so a multi-page transaction is issued to the macro as separate per-page
//       writes."
//     "RRAM Scrambling": the XEX construction, the tweak from the word address (eg2_rram_scramble_core.h).
//   hw/ip/rram_ctrl/README.md "RRAM Controller - Macro Interface": wr_req with wr_last low pushes
//     one RRAM word into the store buffer at the word-in-page bits of addr; wr_req with wr_last
//     high commits the store buffer to the page addr selects and carries no data; rd_req reads.
//   hw/ip/tlul doc (bus integrity): TL-UL data integrity is prim_secded_inv_39_32 over the 32 data
//     bits; the C encoder below is OpenTitan's hw/ip/prim/dv/prim_secded/secded_enc.c.
//
// Not in the documentation (findings drafted in status/rtl_tickets.md); the model takes:
//   - packing order: bus word i of an RRAM word (bus-word address bits [1:0] = i) at bits
//     [32i+31:32i];
//   - the address the scrambler's tweak uses: the RRAM word address (bus-word address >> 2), the
//     partition not included (the scrambling unit's finding: info page p and data page p share it);
//   - when the store buffer is committed: after the transaction's last word, after 32 pushes, or
//     after the last word of a page ("RRAM Write Data Path" names the per-page split and the
//     32-word bound; the model expects a commit at each);
//   - the address of a commit beat: only its page bits are compared (README: "the page selected by
//     addr").

#ifndef EG2_RRAM_PHY_CORE_H_
#define EG2_RRAM_PHY_CORE_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "eg2_rram_scramble_core.h"

namespace eg2_model {

struct RramPhy {
  static constexpr uint32_t kAddrW = 17;      // RRAM word address: 12 page + 5 word-in-page bits
  static constexpr uint32_t kWordW = 5;
  static constexpr uint32_t kMaxWrWords = 32;

  // The bus integrity of a 32-bit word (prim_secded_inv_39_32, secded_enc.c).
  static uint8_t parity(uint32_t w, bool inv) {
    return uint8_t((__builtin_popcount(w) & 1) ^ (inv ? 1 : 0));
  }
  static uint8_t intg(uint32_t w) {
    return uint8_t(parity(w & 0x2606bd25, false) << 0 | parity(w & 0xdeba8050, true) << 1 |
                   parity(w & 0x413d89aa, false) << 2 | parity(w & 0x31234ed1, true) << 3 |
                   parity(w & 0xc2c1323b, false) << 4 | parity(w & 0x2dcc624c, true) << 5 |
                   parity(w & 0x98505586, false) << 6);
  }
  static uint64_t with_intg(uint32_t w, uint32_t intg_of) {
    return uint64_t(intg(intg_of)) << 32 | w;
  }

  // One request rram_phy sends to the macro for a write.
  struct MacroWr {
    bool last;          // commit beat
    uint32_t addr;      // RRAM word address (a commit: only the page bits count)
    bool part;
    bool ecc_en;
    rram_u128 data;     // a push only
  };

  RramScramble scr;
  RramPhy(rram_u128 rst_addr_key, rram_u128 rst_data_key) : scr(rst_addr_key, rst_data_key) {}
  // The macro's contents as rram_phy should have written them (key: part << 17 | RRAM address),
  // and the ECC status the bench's macro gives each word (0, 1 corrected, 2 corrected, 4 not).
  std::unordered_map<uint32_t, rram_u128> image;
  std::unordered_map<uint32_t, uint32_t> ecc_status;
  // Write assembly: the words of the current RRAM word, the pushes since the last commit.
  rram_u128 pack = 0;
  uint32_t npack = 0;
  uint32_t npushed = 0;
  std::vector<MacroWr> store;         // pushes the model expects the macro to hold
  std::deque<MacroWr> expected;       // macro write requests still to come, in order
  uint32_t wr_intg_errs = 0;          // write words with a bad integrity (zero-filled)
  std::string error;                  // the last check that failed, for the bench's message

  static uint32_t key(bool part, uint32_t addr) {
    return (uint32_t(part) << kAddrW) | (addr & ((1u << kAddrW) - 1));
  }

  void preload(bool part, uint32_t addr, rram_u128 d) { image[key(part, addr)] = d; }
  void set_status(bool part, uint32_t addr, uint32_t st) { ecc_status[key(part, addr)] = st; }

  // A controller write word, as rram_phy accepts it (ctrl_wr_done). bus_addr: the bus-word address
  // (19 bits); d39: 32 data + 7 integrity bits.
  void ctrl_wr_word(uint32_t bus_addr, uint64_t d39, bool part, bool scramble, bool ecc, bool xr,
                    bool last) {
    uint32_t w = uint32_t(d39);
    if (uint8_t(d39 >> 32) != intg(w)) {
      w = 0;  // "zero-fills the word before the XOR"
      wr_intg_errs++;
    }
    if (xr) w ^= bus_addr;
    uint32_t sel = bus_addr & 3;
    pack |= rram_u128(w) << (32 * sel);
    npack++;
    if (sel != 3) return;
    // A full RRAM word: scramble (if enabled) and push it.
    uint32_t addr = bus_addr >> 2;
    rram_u128 d = scramble ? scr.scramble(addr, pack) : pack;
    MacroWr p{false, addr, part, ecc, d};
    expected.push_back(p);
    store.push_back(p);
    pack = 0;
    npack = 0;
    npushed++;
    bool page_end = (addr & ((1u << kWordW) - 1)) == (1u << kWordW) - 1;
    if (last || npushed == kMaxWrWords || page_end) {
      expected.push_back(MacroWr{true, addr, part, ecc, 0});
      npushed = 0;
    }
  }

  // A macro write request rram_phy made (accepted by the bench's macro with ack). Returns false
  // with `error` set when it is not the one the model expects. A commit writes the store buffer
  // into the model's image.
  bool macro_wr(bool last, uint32_t addr, bool part, bool ecc, rram_u128 data) {
    char buf[320];
    if (expected.empty()) {
      snprintf(buf, sizeof buf, "macro %s to %05x not expected (no controller write pending)",
               last ? "commit" : "push", addr);
      error = buf;
      return false;
    }
    MacroWr e = expected.front();
    expected.pop_front();
    bool ok = e.last == last && e.part == part && e.ecc_en == ecc;
    if (last) {
      ok = ok && (addr >> kWordW) == (e.addr >> kWordW);
    } else {
      ok = ok && addr == e.addr && data == e.data;
    }
    if (!ok) {
      snprintf(buf, sizeof buf,
               "macro write: got %s addr %05x part %d ecc_en %d data %016llx%016llx; expected %s "
               "addr %05x part %d ecc_en %d data %016llx%016llx",
               last ? "commit" : "push", addr, part, ecc, (unsigned long long)(data >> 64),
               (unsigned long long)data, e.last ? "commit" : "push", e.addr, e.part, e.ecc_en,
               (unsigned long long)(e.data >> 64), (unsigned long long)e.data);
      error = buf;
    }
    if (e.last) {
      // The page is written: the model's store buffer into the image; a rewritten word has no
      // ECC error any more (the bench's macro clears its status the same way).
      for (const MacroWr &p : store) {
        image[key(p.part, p.addr)] = p.data;
        ecc_status.erase(key(p.part, p.addr));
      }
      store.clear();
    }
    return ok;
  }

  // The response the model expects for a read of bus_addr with the page's attributes: 32 data bits
  // and 7 integrity bits; *err set for an uncorrectable ECC error (the data is then not compared).
  uint64_t read(uint32_t bus_addr, bool part, bool scramble, bool ecc, bool xr, bool *err) const {
    uint32_t addr = bus_addr >> 2;
    auto it = image.find(key(part, addr));
    rram_u128 stored = it == image.end() ? 0 : it->second;
    auto st = ecc_status.find(key(part, addr));
    *err = ecc && st != ecc_status.end() && st->second == 4;
    rram_u128 word = scramble ? scr.descramble(addr, stored) : stored;
    uint32_t w = uint32_t(word >> (32 * (bus_addr & 3)));
    return with_intg(w, xr ? (w ^ bus_addr) : w);
  }
};

}  // namespace eg2_model

#endif  // EG2_RRAM_PHY_CORE_H_
