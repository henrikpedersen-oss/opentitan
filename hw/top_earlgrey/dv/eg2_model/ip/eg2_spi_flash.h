// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// An SPI NOR flash on spi_host's bus (eg2sim --stim spi_host<N>.peer=flash): the Winbond W25Q01JV,
// the device spi_host's README names as "the primary reference" for the IP. Every rule follows its
// datasheet (W25Q01JV SPI Rev B, 2019-11-13, the file the README links; sections below as 7.x).
//
// Identity: JEDEC ID (9Fh) EF 40 21; Manufacturer/Device ID (90h) EF 20 (7.3.1).
// Array: 1 Gbit, 128 MiB; 256-byte pages, 4 KB sectors, 32 KB and 64 KB blocks; erased bytes are FFh;
// a program only clears bits (a byte becomes old & new). Kept by sector, so only written sectors
// take memory.
// Status registers (7.1): SR1 BUSY S0, WEL S1, BP0-3 S2-5, TB S6, SRP S7; SR2 SRL S8, QE S9, LB1-3
// S11-13, CMP S14, SUS S15; SR3 ADS S16, ADP S17, WPS S18, DRV S21-22. 06h sets WEL, 04h clears it;
// 01h/31h/11h write one status byte (7.4.5): after 06h the non-volatile value, BUSY for tW, WEL
// cleared at the end; after 50h only the volatile value, BUSY stays 0. Only the bits the datasheet
// calls writable change.
// Addresses: 3 bytes, 4 with ADS set (B7h sets it, E9h clears it, 7.4.6-7.4.7) and always 4 for
// the 4-byte opcodes (0Ch 12h 13h 21h 34h 3Ch 6Ch BCh DCh ECh). SFDP (5Ah) has 3 (7.4.40).
// Reads: 03h/13h, 0Bh/0Ch, 3Bh/3Ch, 6Bh/6Ch, BBh/BCh, EBh/ECh give data from the address on,
// wrapping at the end of the array; the mode and dummy bits between address and data are not
// interpreted (any bytes or dummy cycles the host sends there are skipped).
// Program: 02h/12h, 32h/34h (quad) into the 256-byte page of the address, wrapping inside it (7.4.21);
// erase: 20h/21h (4 KB), 52h (32 KB), D8h/DCh (64 KB), C7h/60h (chip); each only with WEL set, done
// when CS rises, then BUSY for its typical time (AC Characteristics: tPP 0.7 ms, tSE 50 ms, tBE1
// 120 ms, tBE2 150 ms, tCE 200 s) and WEL cleared at the end. While BUSY only the status reads are
// accepted ("all other instructions are ignored").
// Quad instructions need QE set (6.1.3: "Quad SPI instructions require the non-volatile Quad Enable
// bit (QE) in Status Register-2 to be set"); without it they are ignored.
// Software reset (6.1.5): 66h then 99h as the next instruction: volatile status values back to the
// non-volatile ones, WEL 0, ADS from ADP.
// SFDP: the datasheet refers to Winbond's "Application Note for SFDP Definition Table", which is
// not available here, so the table is the JESD216B basic flash parameter table built from the
// datasheet's values for what this model implements (fields below).
// Not modelled: erase/program suspend and resume (75h/7Ah), deep power-down (B9h/ABh), continuous
// read (mode bits), block protection (BP, TB, CMP, WPS, SRP and the /WP pin), the security
// registers (44h/42h/48h), the unique ID (4Bh), the dual and quad ID reads (92h/94h), the die
// select of the two-die stack, DTR, the maximum (as opposed to typical) program and erase times.

#ifndef EG2_MODEL_IP_EG2_SPI_FLASH_H_
#define EG2_MODEL_IP_EG2_SPI_FLASH_H_

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace eg2_model {

class Eg2SpiFlash {
 public:
  static constexpr uint32_t kBytes = 128u << 20;  // 1 Gbit
  static constexpr uint32_t kSector = 4096, kPage = 256;

  Eg2SpiFlash() { build_sfdp(); }

  // One byte each way: the host's (unused for an RX-only byte) in, the flash's out.
  uint8_t xfer(bool host_drives, uint8_t mosi, uint64_t now) {
    settle(now);
    if (phase_ == kOpcode) {
      if (!host_drives) return 0xff;
      start(mosi, now);
      return 0xff;
    }
    if (phase_ == kAddr) {
      if (host_drives) {
        addr_ = (addr_ << 8) | mosi;
        if (++addr_n_ == addr_len_) phase_ = kData;
      }
      return 0xff;
    }
    if (phase_ != kData) return 0xff;
    switch (op_) {
      case 0x05: return sr1();
      case 0x35: return sr2();
      case 0x15: return sr3();
      case 0x9f: return out_seq({0xef, 0x40, 0x21});
      case 0x90: return out_seq({0xef, 0x20}, true);
      case 0x5a:  // 8 dummy clocks after the address: bytes the host drives there carry no data
        return host_drives ? 0xff : sfdp_[(addr_ + out_n_++) & 0xff];
      case 0x01: case 0x31: case 0x11:
        if (host_drives && wsr_n_++ == 0) wsr_byte_ = mosi;
        return 0xff;
      case 0x02: case 0x12: case 0x32: case 0x34:
        if (host_drives) pp_.push_back(mosi);
        return 0xff;
      default:
        if (is_read(op_)) {
          // Bytes the host sends after the address are the mode or dummy bits.
          if (host_drives) return 0xff;
          return read((addr_ + out_n_++) % kBytes);
        }
        return 0xff;
    }
  }
  // Dummy cycles (spi_host's dummy segment): nothing moves.
  void dummy(uint64_t now) { settle(now); }
  // CS rises: the instruction ends; programs, erases and status writes start here.
  void cs_release(uint64_t now) {
    settle(now);
    if (phase_ != kOpcode && !ignored_) finish(now);
    prev_op_ = phase_ == kOpcode ? prev_op_ : op_;
    phase_ = kOpcode;
  }

 private:
  enum Phase { kOpcode, kAddr, kData };

  static bool is_4byte_op(uint8_t op) {
    switch (op) {
      case 0x0c: case 0x12: case 0x13: case 0x21: case 0x34: case 0x3c: case 0x6c: case 0xbc:
      case 0xdc: case 0xec: return true;
      default: return false;
    }
  }
  static bool is_read(uint8_t op) {
    switch (op) {
      case 0x03: case 0x13: case 0x0b: case 0x0c: case 0x3b: case 0x3c: case 0x6b: case 0x6c:
      case 0xbb: case 0xbc: case 0xeb: case 0xec: return true;
      default: return false;
    }
  }
  static bool is_quad(uint8_t op) {
    switch (op) {
      case 0x6b: case 0x6c: case 0xeb: case 0xec: case 0x32: case 0x34: return true;
      default: return false;
    }
  }
  static bool has_addr(uint8_t op) {
    return is_read(op) || op == 0x90 || op == 0x5a || op == 0x02 || op == 0x12 || op == 0x32 ||
           op == 0x34 || op == 0x20 || op == 0x21 || op == 0x52 || op == 0xd8 || op == 0xdc;
  }

  void start(uint8_t op, uint64_t now) {
    op_ = op;
    addr_ = 0;
    addr_n_ = 0;
    out_n_ = 0;
    wsr_n_ = 0;
    pp_.clear();
    // While BUSY only the status reads are accepted.
    ignored_ = (busy(now) && op != 0x05 && op != 0x35 && op != 0x15) ||
               (is_quad(op) && !(sr_[1] & 0x02));
    addr_len_ = op == 0x5a ? 3 : is_4byte_op(op) || (sr_[2] & 0x01) ? 4 : 3;
    phase_ = has_addr(op) && !ignored_ ? kAddr : kData;
    if (ignored_) {
      op_ = 0;  // nothing answers: the bus stays at FFh
      return;
    }
    switch (op) {
      case 0x06: wel_ = true; vol_wren_ = false; break;  // 7.4.1
      case 0x50: vol_wren_ = true; break;                // 7.4.2
      case 0x04: wel_ = false; vol_wren_ = false; break; // 7.4.3
      case 0xb7: sr_[2] |= 0x01; break;                  // 7.4.6 ADS
      case 0xe9: sr_[2] &= ~0x01; break;                 // 7.4.7
      case 0x99:
        if (prev_op_ == 0x66) {  // 6.1.5: Enable Reset then Reset
          sr_ = nv_;
          wel_ = vol_wren_ = false;
          sr_[2] = uint8_t((sr_[2] & ~0x01) | ((nv_[2] >> 1) & 0x01));
        }
        break;
      default: break;
    }
  }
  void finish(uint64_t now) {
    switch (op_) {
      case 0x01: case 0x31: case 0x11: {
        if (wsr_n_ == 0 || (!wel_ && !vol_wren_)) break;
        unsigned n = op_ == 0x01 ? 0 : op_ == 0x31 ? 1 : 2;
        // 7.4.5: the writable bits; LB[3:1] (and SRL) cannot go from 1 to 0.
        static constexpr uint8_t kWritable[3] = {0x7c, 0x7b, 0x66};
        uint8_t v = uint8_t((sr_[n] & ~kWritable[n]) | (wsr_byte_ & kWritable[n]));
        if (n == 1) v |= sr_[1] & 0x39;
        if (wel_) {
          nv_[n] = v;
          sr_[n] = v;
          begin_busy(now, 10'000'000'000ull);  // tW 10 ms
        } else {
          sr_[n] = v;
        }
        vol_wren_ = false;
        break;
      }
      case 0x02: case 0x12: case 0x32: case 0x34:
        if (!wel_ || addr_n_ < addr_len_) break;
        for (size_t i = 0; i < pp_.size(); ++i) {
          uint32_t a = (addr_ & ~(kPage - 1)) | ((addr_ + uint32_t(i)) & (kPage - 1));
          uint8_t &b = cell(a % kBytes);
          b = uint8_t(b & pp_[i]);
        }
        begin_busy(now, 700'000'000ull);  // tPP 0.7 ms
        break;
      case 0x20: case 0x21: erase(now, 4096, 50'000'000'000ull); break;      // tSE 50 ms
      case 0x52: erase(now, 32768, 120'000'000'000ull); break;               // tBE1 120 ms
      case 0xd8: case 0xdc: erase(now, 65536, 150'000'000'000ull); break;    // tBE2 150 ms
      case 0xc7: case 0x60:
        if (!wel_) break;
        sectors_.clear();
        begin_busy(now, 200'000'000'000'000ull);  // tCE 200 s
        break;
      default: break;
    }
  }
  void erase(uint64_t now, uint32_t size, uint64_t t_ps) {
    if (!wel_ || addr_n_ < addr_len_) return;
    uint32_t base = (addr_ % kBytes) & ~(size - 1);
    for (uint32_t s = base; s < base + size; s += kSector) sectors_.erase(s / kSector);
    begin_busy(now, t_ps);
  }
  void begin_busy(uint64_t now, uint64_t t_ps) {
    busy_until_ = now + t_ps;
    clear_wel_ = true;
  }
  bool busy(uint64_t now) const { return now < busy_until_; }
  // BUSY ends at its time; WEL is cleared when the operation that needed it ends.
  void settle(uint64_t now) {
    if (clear_wel_ && !busy(now)) {
      wel_ = false;
      clear_wel_ = false;
    }
    now_ = now;
  }
  uint8_t sr1() const {
    return uint8_t((sr_[0] & ~0x03) | (wel_ ? 0x02 : 0) | (busy(now_) ? 0x01 : 0));
  }
  uint8_t sr2() const { return sr_[1]; }
  uint8_t sr3() const { return sr_[2]; }
  uint8_t out_seq(std::vector<uint8_t> seq, bool repeat = false) {
    size_t i = out_n_++;
    if (i < seq.size()) return seq[i];
    return repeat ? seq[i % seq.size()] : 0xff;
  }
  uint8_t read(uint32_t a) const {
    auto it = sectors_.find(a / kSector);
    return it == sectors_.end() ? 0xff : it->second[a % kSector];
  }
  uint8_t &cell(uint32_t a) {
    auto it = sectors_.find(a / kSector);
    if (it == sectors_.end()) it = sectors_.emplace(a / kSector, std::vector<uint8_t>(kSector, 0xff)).first;
    return it->second[a % kSector];
  }

  // JESD216B: the SFDP header, one parameter header and the 16-dword basic flash parameter table
  // (BFPT) at 0x80; every other byte FFh.
  void build_sfdp() {
    sfdp_.fill(0xff);
    const uint8_t hdr[16] = {'S', 'F', 'D', 'P', 0x06, 0x01, 0x00, 0xff,   // rev 1.6, 1 header
                             0x00, 0x06, 0x01, 16, 0x80, 0x00, 0x00, 0xff};  // BFPT, 16 dwords
    for (int i = 0; i < 16; ++i) sfdp_[i] = hdr[i];
    const uint32_t bfpt[16] = {
        // 1: 1-1-4, 1-4-4, 1-2-2 fast reads; no DTR; 3- or 4-byte addresses; 1-1-2 fast read;
        //    4 KB erase 20h; page buffer >= 64 bytes; 4 KB erase supported. Unused bits 1.
        0xfff320e5,
        // 2: density 1 Gbit, zero-based in bits.
        0x3fffffff,
        // 3: 1-1-4 6Bh, 8 dummy clocks; 1-4-4 EBh, 2 mode and 4 dummy clocks.
        0x6b08eb44,
        // 4: 1-2-2 BBh, 4 mode clocks (M7-M0); 1-1-2 3Bh, 8 dummy clocks.
        0xbb803b08,
        // 5-7: no 2-2-2 or 4-4-4 reads.
        0xffffffee, 0x0000ffff, 0x0000ffff,
        // 8: erase type 2 52h 32 KB, type 1 20h 4 KB. 9: type 3 D8h 64 KB.
        0x520f200c, 0x0000d810,
        // 10: typical erase times, units of 16 ms: 4 KB 64 ms, 32 KB 128 ms, 64 KB 160 ms; maximum
        //     = 2 * (6 + 1) times typical (tBE 2000 / 150 ms).
        0x00a53a36,
        // 11: chip erase 256 s (64 s units); page program 704 us (64 us units); page 256 bytes;
        //     maximum program 2 * (2 + 1) times typical (tPP 3.5 / 0.7 ms). Byte program fields 0.
        0xe3002a82,
        // 12-14: no suspend/resume or deep power-down (not modelled); busy polled with 05h bit 0.
        0x80000100, 0x00000000, 0x80000007,
        // 15: Quad Enable QE bit 1 of status register 2, read with 35h, written with 31h (one byte):
        //     JESD216B QER 110b. 7.4.5: 01h writes status register 1 only, so not 101b (the
        //     two-byte 01h write). No 0-4-4 or 4-4-4.
        0xff600000,
        // 16: enter 4-byte addressing B7h, exit E9h; soft reset 66h then 99h; status register 1
        //     non-volatile with 06h, volatile with 50h.
        0x01005088,
    };
    for (int w = 0; w < 16; ++w)
      for (int b = 0; b < 4; ++b) sfdp_[0x80 + 4 * w + b] = uint8_t(bfpt[w] >> (8 * b));
  }

  std::array<uint8_t, 256> sfdp_{};
  std::unordered_map<uint32_t, std::vector<uint8_t>> sectors_;
  std::array<uint8_t, 3> sr_{}, nv_{};  // the status registers: current and non-volatile
  bool wel_ = false, vol_wren_ = false, clear_wel_ = false, ignored_ = false;
  Phase phase_ = kOpcode;
  uint8_t op_ = 0, prev_op_ = 0, wsr_byte_ = 0;
  unsigned addr_n_ = 0, addr_len_ = 3, wsr_n_ = 0;
  uint32_t addr_ = 0, out_n_ = 0;
  std::vector<uint8_t> pp_;
  uint64_t busy_until_ = 0, now_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_SPI_FLASH_H_
