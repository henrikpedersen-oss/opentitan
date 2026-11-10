// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_otbn.h"

namespace eg2_model {

Eg2OtbnCore::Stop Eg2OtbnCore::step() {
  if (ended_) return Stop::kEnd;
  if (paused_) return Stop::kWfi;
  mai_land();
  const uint32_t pc = pc_;
  const uint32_t w = imem32(pc);
  const uint32_t op = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31, rs1 = (w >> 15) & 31,
                 rs2 = (w >> 20) & 31;

  // URND advances every cycle the processor runs; stopped, only when it is read (URND_CTRL).
  if (urnd_stopped_) {
    bool reads = (op == 0x73 && (f3 == 1 || f3 == 2) && (w >> 20) == kUrnd) ||
                 (op == 0x0b && f3 == 7 && !(w >> 31) && ((w >> 20) & 0xff) == kWUrnd);
    if (reads) {
      urnd_advance();
      urnd_used_while_stopped_ = true;
    }
  } else {
    urnd_advance();
  }

  // The GPR reads (x1 pops the call stack once, however often it is read) and writes (x1
  // pushes) of the instruction, applied at its end.
  bool pop = false, cs_err = false;
  auto rg = [&](uint32_t r) -> uint32_t {
    if (r == 0) return 0;
    if (r == 1) {
      if (cstack_.empty()) {
        cs_err = true;
        return 0;
      }
      pop = true;
      return cstack_.back();
    }
    return gpr_[r];
  };
  struct GprWrite {
    uint32_t r, v;
  } gw[2];
  unsigned ngw = 0;
  auto wg = [&](uint32_t r, uint32_t v) { gw[ngw++] = {r, v}; };

  uint32_t err = 0;
  uint32_t next = pc + 4;
  unsigned cycles = 1;
  bool control = false;  // a jump, branch or loop instruction ("may not appear as the last
                         // instruction in a loop")
  bool wfi = false, ecall = false;
  // Effects beyond the GPRs, WDRs and flags, applied when the instruction succeeds.
  enum { kNoEffect, kStore32, kStore256, kCsrAccess, kLoopPush, kSetAcc, kAccUrnd, kWsrRead,
         kWsrWrite } effect = kNoEffect;
  uint32_t e_addr = 0, e_val = 0, e_csr = 0;
  bool e_reads = false, e_writes = false;
  OtbnWide e_wide;
  Loop e_loop{0, 0, 0};
  int wdr_dst = -1;
  OtbnWide wdr_val;
  int fg_dst = -1;
  uint8_t fg_val = 0;

  auto illegal = [&]() { err |= kIllegalInsn; };

  switch (op) {
    case 0x33: {  // OP: ADD SUB SLL SRL SRA AND OR XOR
      uint32_t f7 = w >> 25;
      uint32_t a = rg(rs1), b = rg(rs2);
      uint32_t r = 0;
      if (f7 == 0x00 && f3 == 0) r = a + b;
      else if (f7 == 0x20 && f3 == 0) r = a - b;
      else if (f7 == 0x00 && f3 == 1) r = a << (b & 31);
      else if (f7 == 0x00 && f3 == 5) r = a >> (b & 31);
      else if (f7 == 0x20 && f3 == 5) r = uint32_t(int32_t(a) >> (b & 31));
      else if (f7 == 0x00 && f3 == 7) r = a & b;
      else if (f7 == 0x00 && f3 == 6) r = a | b;
      else if (f7 == 0x00 && f3 == 4) r = a ^ b;
      else { illegal(); break; }
      if (cs_err) { err |= kCallStack; break; }
      wg(rd, r);
      break;
    }
    case 0x13: {  // OP-IMM: ADDI ANDI ORI XORI SLLI SRLI SRAI
      uint32_t imm = sext(w >> 20, 12);
      uint32_t r = 0;
      bool shift = f3 == 1 || f3 == 5;
      if (shift) {
        // Is: arithmetic (30); bits 31 and 29:25 zero.
        bool arith = (w >> 30) & 1;
        if ((w & 0xbe000000u) || (arith && f3 == 1)) { illegal(); break; }
      } else if (f3 == 2 || f3 == 3) {
        illegal();
        break;
      }
      uint32_t a = rg(rs1);
      if (cs_err) { err |= kCallStack; break; }
      unsigned sh = rs2;
      switch (f3) {
        case 0: r = a + imm; break;
        case 7: r = a & imm; break;
        case 6: r = a | imm; break;
        case 4: r = a ^ imm; break;
        case 1: r = a << sh; break;
        default: r = (w >> 30) & 1 ? uint32_t(int32_t(a) >> sh) : a >> sh; break;
      }
      wg(rd, r);
      break;
    }
    case 0x37:  // LUI
      wg(rd, w & 0xfffff000u);
      break;
    case 0x03: {  // LW
      if (f3 != 2) { illegal(); break; }
      uint32_t a = rg(rs1);
      if (cs_err) { err |= kCallStack; break; }
      uint32_t addr = a + sext(w >> 20, 12);
      if (!valid32(addr)) { err |= kBadDataAddr; break; }
      wg(rd, dmem32(addr));
      cycles = 2;
      break;
    }
    case 0x23: {  // SW
      if (f3 != 2) { illegal(); break; }
      uint32_t a = rg(rs1), v = rg(rs2);
      uint32_t addr = a + sext(((w >> 25) << 5) | ((w >> 7) & 31), 12);
      if (cs_err) err |= kCallStack;
      if (!valid32(addr) && !(cs_err && rs1 == 1)) err |= kBadDataAddr;
      if (err) break;
      effect = kStore32;
      e_addr = addr;
      e_val = v;
      break;
    }
    case 0x63: {  // BEQ BNE
      if (f3 > 1) { illegal(); break; }
      control = true;
      cycles = 2;
      uint32_t a = rg(rs1), b = rg(rs2);
      if (cs_err) { err |= kCallStack; break; }
      uint32_t off = sext(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3f) << 5) |
                              (((w >> 8) & 0xf) << 1),
                          13);
      if ((a == b) == (f3 == 0)) {
        if (!valid_pc(pc + off)) { err |= kBadInsnAddr; break; }
        next = pc + off;
      }
      break;
    }
    case 0x6f: {  // JAL
      control = true;
      cycles = 2;
      uint32_t off = sext(((w >> 31) << 20) | (((w >> 12) & 0xff) << 12) |
                              (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3ff) << 1),
                          21);
      wg(rd, pc + 4);
      if (!valid_pc(pc + off)) err |= kBadInsnAddr;
      next = pc + off;
      break;
    }
    case 0x67: {  // JALR
      if (f3 != 0) { illegal(); break; }
      control = true;
      cycles = 2;
      uint32_t a = rg(rs1);
      if (cs_err) { err |= kCallStack; break; }
      uint32_t t = a + sext(w >> 20, 12);
      wg(rd, pc + 4);
      if (!valid_pc(t)) err |= kBadInsnAddr;
      next = t;
      break;
    }
    case 0x73: {  // SYSTEM: CSRRS CSRRW ECALL WFI
      uint32_t csr = w >> 20;
      if (f3 == 0) {
        if (rd || rs1) illegal();
        else if (csr == 0) ecall = true;
        else if (csr == 0x105) {
          if (!wfi_enabled) illegal();
          else wfi = true;
        } else {
          illegal();
        }
        break;
      }
      if (f3 != 1 && f3 != 2) { illegal(); break; }
      if (!valid_csr(csr)) { illegal(); break; }
      uint32_t s = rg(rs1);
      if (cs_err) { err |= kCallStack; break; }
      bool reads = f3 == 2 || rd != 0;  // CSRRW with grd = x0 does not read the CSR
      bool writes = f3 == 1 || rs1 != 0;
      if (reads && csr == kRnd && !rnd_fill()) return Stop::kWaitEdn;  // stall for EDN
      uint32_t nv = f3 == 1 ? s : csr_peek(csr) | s;
      if (writes && csr == kMaiCtrl && !mai_ctrl_ok(nv)) { err |= kMaiError; break; }
      if (reads) wg(rd, csr_peek(csr));
      effect = kCsrAccess;
      e_csr = csr;
      e_reads = reads;
      e_writes = writes;
      e_val = nv;
      break;
    }
    case 0x7b: {  // custom3: LOOP LOOPI BN.AND BN.OR BN.XOR BN.NOT BN.RSHI
      if (f3 == 0 || f3 == 1) {
        control = true;
        uint32_t iters = f3 == 0 ? rg(rs1) : (rs1 << 5) | rd;
        if (cs_err) { err |= kCallStack; break; }
        if (!iters || lstack_.size() == kStackDepth) { err |= kLoop; break; }
        uint32_t body = (w >> 20) + 1;
        effect = kLoopPush;
        e_loop = Loop{pc + 4, pc + 4 * body, iters - 1};
        break;
      }
      const OtbnWide &a = wdr_[rs1], &b = wdr_[rs2];
      if (f3 == 3 || f3 == 7) {  // BN.RSHI: {wrs1, wrs2} >> imm
        unsigned imm = ((w >> 25) << 1) | ((w >> 14) & 1);
        wdr_val = imm ? OtbnWide{} : b;
        if (imm) {
          OtbnWide lo = OtbnWide::shr(b, imm), hi = OtbnWide::shl(a, 256 - imm);
          for (unsigned i = 0; i < 4; ++i) wdr_val.q[i] = lo.q[i] | hi.q[i];
        }
        wdr_dst = int(rd);
        break;
      }
      unsigned fg = w >> 31;
      if (f3 == 5) {  // BN.NOT: wrs in 24:20
        OtbnWide s = shifted(wdr_[rs2], w);
        for (unsigned i = 0; i < 4; ++i) wdr_val.q[i] = ~s.q[i];
      } else {
        OtbnWide s = shifted(b, w);
        for (unsigned i = 0; i < 4; ++i) {
          wdr_val.q[i] = f3 == 2 ? a.q[i] & s.q[i] : f3 == 4 ? a.q[i] | s.q[i] : a.q[i] ^ s.q[i];
        }
      }
      wdr_dst = int(rd);
      fg_dst = int(fg);
      fg_val = mlz(flags_[fg] & kC, wdr_val);
      break;
    }
    case 0x2b: {  // custom1: BN.ADD BN.SUB BN.ADDC BN.SUBB BN.ADDI BN.SUBI BN.ADDM BN.SUBM
      const OtbnWide &a = wdr_[rs1], &b = wdr_[rs2];
      unsigned fg = w >> 31;
      unsigned c = 0;
      if (f3 <= 3) {
        OtbnWide s = shifted(b, w);
        unsigned cin = (f3 & 2) ? (flags_[fg] & kC) : 0;
        wdr_val = f3 & 1 ? OtbnWide::sub(a, s, cin, &c) : OtbnWide::add(a, s, cin, &c);
      } else if (f3 == 4) {
        OtbnWide imm;
        imm.q[0] = (w >> 20) & 0x3ff;
        wdr_val = (w >> 30) & 1 ? OtbnWide::sub(a, imm, 0, &c) : OtbnWide::add(a, imm, 0, &c);
      } else if (f3 == 5) {
        if ((w >> 30) & 1) {  // BN.SUBM: add MOD when negative
          unsigned neg;
          wdr_val = OtbnWide::sub(a, b, 0, &neg);
          if (neg) wdr_val = OtbnWide::add(wdr_val, mod_, 0, nullptr);
        } else {  // BN.ADDM: subtract MOD when the WLEN+1-bit sum is at least MOD
          unsigned carry;
          wdr_val = OtbnWide::add(a, b, 0, &carry);
          if (carry || !OtbnWide::less(wdr_val, mod_)) {
            wdr_val = OtbnWide::sub(wdr_val, mod_, 0, nullptr);
          }
        }
        wdr_dst = int(rd);
        break;
      } else {
        illegal();
        break;
      }
      wdr_dst = int(rd);
      fg_dst = int(fg);
      fg_val = mlz(c, wdr_val);
      break;
    }
    case 0x3b: {  // custom2: BN.MULQACC, .WO, .SO
      unsigned fg = w >> 31, so = (w >> 30) & 1, wb0 = (w >> 29) & 1;
      unsigned qs2 = (w >> 27) & 3, qs1 = (w >> 25) & 3, sh = ((w >> 13) & 3) * 64;
      unsigned __int128 p = (unsigned __int128)wdr_[rs1].q[qs1] * wdr_[rs2].q[qs2];
      OtbnWide prod;
      prod.q[0] = uint64_t(p);
      prod.q[1] = uint64_t(p >> 64);
      OtbnWide acc = (w >> 12) & 1 ? OtbnWide() : acc_;
      acc = OtbnWide::add(acc, OtbnWide::shl(prod, sh), 0, nullptr);
      effect = kSetAcc;
      e_wide = acc;
      if (!so && wb0) {
        wdr_dst = int(rd);
        wdr_val = acc;
        fg_dst = int(fg);
        fg_val = mlz(flags_[fg] & kC, acc);
      } else if (so) {
        // The low half-word goes to half wb0 of wrd, ACC keeps the high half.
        OtbnWide lo = acc, hi = OtbnWide::shr(acc, 128);
        lo.q[2] = lo.q[3] = 0;
        wdr_val = wdr_[rd];
        wdr_val.q[2 * wb0] = lo.q[0];
        wdr_val.q[2 * wb0 + 1] = lo.q[1];
        wdr_dst = int(rd);
        uint8_t f = flags_[fg];
        if (wb0) {
          f = uint8_t((f & (kC | kL)) | ((lo.q[1] >> 63) ? kM : 0) |
                      ((f & kZ) && lo.zero() ? kZ : 0));
        } else {
          f = uint8_t((f & (kC | kM)) | ((lo.q[0] & 1) ? kL : 0) | (lo.zero() ? kZ : 0));
        }
        fg_dst = int(fg);
        fg_val = f;
        e_wide = hi;
      }
      break;
    }
    case 0x0b: {  // custom0: BN.SEL BN.CMP BN.CMPB BN.LID BN.SID BN.MOV BN.MOVR BN.WSRR BN.WSRW
      unsigned fg = w >> 31;
      switch (f3) {
        case 0: {  // BN.SEL: flag (26:25) C, M, L, Z
          unsigned flag = (w >> 25) & 3;
          wdr_val = (flags_[fg] >> flag) & 1 ? wdr_[rs1] : wdr_[rs2];
          wdr_dst = int(rd);
          break;
        }
        case 1:
        case 3: {  // BN.CMP, BN.CMPB
          unsigned c;
          unsigned bin = f3 == 3 ? flags_[fg] & kC : 0;
          OtbnWide r = OtbnWide::sub(wdr_[rs1], shifted(wdr_[rs2], w), bin, &c);
          fg_dst = int(fg);
          fg_val = mlz(c, r);
          break;
        }
        case 4:
        case 5: {  // BN.LID, BN.SID: offset {11:9, 31:25} << 5; grs1_inc (8), grd/grs2_inc (7)
          bool inc1 = (w >> 8) & 1, inc2 = (w >> 7) & 1;
          if (inc1 && inc2) { illegal(); break; }
          uint32_t off = sext((((w >> 9) & 7) << 7) | (w >> 25), 10) << 5;
          uint32_t a = rg(rs1);
          uint32_t addr = a + off;
          uint32_t g = rg(rs2);
          bool bad1 = cs_err && rs1 == 1, bad2 = cs_err && rs2 == 1;
          if (cs_err) err |= kCallStack;
          if (g > 31 && !bad2) err |= kIllegalInsn;
          if (!valid256(addr) && !bad1) err |= kBadDataAddr;
          if (err) break;
          if (inc2) wg(rs2, g + 1);
          if (inc1) wg(rs1, a + 32);
          cycles = 2;
          if (f3 == 4) {
            wdr_dst = int(g & 31);
            wdr_val = dmem256(addr);
          } else {
            effect = kStore256;
            e_addr = addr;
            e_wide = wdr_[g & 31];
          }
          break;
        }
        case 6: {
          if (!(w >> 31)) {  // BN.MOV
            wdr_val = wdr_[rs1];
            wdr_dst = int(rd);
            break;
          }
          // BN.MOVR: grd (24:20), grs (19:15), grs_inc (9), grd_inc (7)
          bool incs = (w >> 9) & 1, incd = (w >> 7) & 1;
          if (incs && incd) { illegal(); break; }
          uint32_t d = rg(rs2), s = rg(rs1);
          bool badd = cs_err && rs2 == 1, bads = cs_err && rs1 == 1;
          if (cs_err) err |= kCallStack;
          if (d > 31 && !badd) err |= kIllegalInsn;
          if (s > 31 && !bads) err |= kIllegalInsn;
          if (err) break;
          if (incd) wg(rs2, d + 1);
          if (incs) wg(rs1, s + 1);
          wdr_val = wdr_[s & 31];
          wdr_dst = int(d & 31);
          cycles = 2;
          break;
        }
        case 7: {  // BN.WSRR / BN.WSRW: wsr (27:20)
          uint32_t wsr = (w >> 20) & 0xff;
          if (wsr >= kNumWsrs) { illegal(); break; }
          if (!(w >> 31)) {
            if (wsr == kWRnd && !rnd_fill()) return Stop::kWaitEdn;  // stall for EDN
            if (wsr >= kWKeyS0L && wsr <= kWKeyS1H && !key_valid_) { err |= kKeyInvalid; break; }
            effect = kWsrRead;
          } else {
            // The MAI's input WSRs take writes only while INPUT_READY is set.
            if (wsr >= kWMaiIn0S0 && wsr <= kWMaiIn1S1 && !mai_input_ready()) {
              err |= kMaiError;
              break;
            }
            effect = kWsrWrite;
            e_wide = wdr_[rs1];
          }
          e_csr = wsr;
          break;
        }
        default:
          illegal();
          break;
      }
      break;
    }
    case 0x5b: {  // custom4: the vector instructions
      unsigned elen = (w >> 25) & 3;
      const OtbnWide &a = wdr_[rs1], &b = wdr_[rs2];
      switch (f3) {
        case 0: {  // BN.ADDV(M), BN.SUBV(M): .8s only
          if (elen != 0) { illegal(); break; }
          bool sub = (w >> 30) & 1, m = (w >> 28) & 1;
          uint32_t q = uint32_t(mod_.q[0]);
          for (unsigned i = 0; i < 8; ++i) {
            uint32_t x = a.word(i), y = b.word(i), r;
            if (!sub) {
              uint64_t t = uint64_t(x) + y;
              if (m && t >= q) t -= q;
              r = uint32_t(t);
            } else {
              int64_t t = int64_t(x) - int64_t(y);
              if (m && t < 0) t += q;
              r = uint32_t(t);
            }
            wdr_val.set_word(i, r);
          }
          wdr_dst = int(rd);
          break;
        }
        case 3:
        case 4: {  // BN.MULV(L), BN.MULVM(L): lane (30:28), use_lane (27); .8s only
          if (elen != 0) { illegal(); break; }
          bool lane = (w >> 27) & 1;
          uint32_t l = (w >> 28) & 7;
          uint64_t q = uint32_t(mod_.q[0]), mu = uint32_t(mod_.q[0] >> 32);
          for (unsigned i = 0; i < 8; ++i) {
            uint64_t x = a.word(i), y = b.word(lane ? l : i);
            uint64_t c = x * y;
            uint32_t r;
            if (f3 == 3) {
              r = uint32_t(c);
            } else {
              // Montgomery without the final subtraction: [c + [[c]_d * mu]_d * q]^d, d = 32.
              uint64_t t = uint32_t(uint32_t(c) * mu);
              unsigned __int128 s = (unsigned __int128)c + (unsigned __int128)t * q;
              r = uint32_t(uint64_t(s >> 32));
            }
            wdr_val.set_word(i, r);
          }
          wdr_dst = int(rd);
          cycles = f3 == 3 ? 4 : 12;
          // ACC assembles the result and is cleared with URND at the end.
          effect = kAccUrnd;
          break;
        }
        case 5: {  // BN.TRN1, BN.TRN2: odd (30); .8s, .4d, .2q
          if (elen == 3) { illegal(); break; }
          unsigned ew = 1u << elen;  // element size in 32-bit words
          unsigned odd = (w >> 30) & 1;
          for (unsigned e = 0; e < 8 / ew; e += 2) {
            for (unsigned k = 0; k < ew; ++k) {
              wdr_val.set_word(e * ew + k, a.word((e + odd) * ew + k));
              wdr_val.set_word((e + 1) * ew + k, b.word((e + odd) * ew + k));
            }
          }
          wdr_dst = int(rd);
          break;
        }
        case 6: {  // BN.PACK, BN.UNPK: is_pack (30), shift (28:27) * 64
          unsigned sh = ((w >> 27) & 3) * 64;
          uint32_t v[16] = {}, r[16] = {};
          if ((w >> 30) & 1) {
            // {64'd0, dense(wrs1), dense(wrs2), 64'd0} >> shift
            for (unsigned i = 0; i < 8; ++i) {
              put_bits(v, 64 + 24 * i, 24, b.word(i) & 0xffffff);
              put_bits(v, 256 + 24 * i, 24, a.word(i) & 0xffffff);
            }
            for (unsigned i = 0; i + sh / 32 < 16; ++i) r[i] = v[i + sh / 32];
            for (unsigned i = 0; i < 8; ++i) wdr_val.set_word(i, r[i]);
          } else {
            // {wrs1, wrs2} >> shift, eight 24-bit elements zero-extended to 32 bits
            for (unsigned i = 0; i < 8; ++i) {
              v[i] = b.word(i);
              v[8 + i] = a.word(i);
            }
            for (unsigned i = 0; i + sh / 32 < 16; ++i) r[i] = v[i + sh / 32];
            for (unsigned i = 0; i < 8; ++i) wdr_val.set_word(i, bits_of(r, 24 * i, 24));
          }
          wdr_dst = int(rd);
          break;
        }
        case 7: {  // BN.SHV: shift_type (30), shift_bits (19:15), wrs (24:20); .8s only
          if (elen != 0) { illegal(); break; }
          unsigned n = rs1;
          bool right = (w >> 30) & 1;
          for (unsigned i = 0; i < 8; ++i) {
            uint32_t x = wdr_[rs2].word(i);
            wdr_val.set_word(i, right ? x >> n : x << n);
          }
          wdr_dst = int(rd);
          break;
        }
        default:
          illegal();
          break;
      }
      break;
    }
    default:
      illegal();
      break;
  }

  // A jump, branch or loop instruction may not end a loop body (LOOP, LOOPI).
  bool loop_end = !lstack_.empty() && pc == lstack_.back().last;
  if (loop_end && control) err |= kLoop;
  // x1 written: a push onto the call stack, which must not be full unless the instruction
  // popped it too.
  unsigned pushes = 0;
  for (unsigned i = 0; i < ngw; ++i) pushes += gw[i].r == 1;
  if (pushes && cstack_.size() - (pop ? 1 : 0) + pushes > kStackDepth) err |= kCallStack;
  if (!err && loop_end && lstack_.back().restarts) next = lstack_.back().start;
  if (!err && !ecall && !valid_pc(next)) err |= kBadInsnAddr;
  if (err) return end(err);

  // The instruction succeeds: apply its effects.
  if (pop) cstack_.pop_back();
  for (unsigned i = 0; i < ngw; ++i) {
    if (gw[i].r == 1) cstack_.push_back(gw[i].v);
    else if (gw[i].r) gpr_[gw[i].r] = gw[i].v;
  }
  if (wdr_dst >= 0) wdr_[wdr_dst] = wdr_val;
  if (fg_dst >= 0) flags_[fg_dst] = fg_val;
  switch (effect) {
    case kStore32: std::memcpy(&dmem[e_addr], &e_val, 4); break;
    case kStore256: std::memcpy(&dmem[e_addr], e_wide.q, 32); break;
    case kCsrAccess:
      if (e_reads) csr_read(e_csr);
      if (e_writes) csr_write(e_csr, e_val);
      break;
    case kLoopPush: lstack_.push_back(e_loop); break;
    case kSetAcc: acc_ = e_wide; break;
    case kAccUrnd: acc_ = urnd_; break;
    case kWsrRead: wdr_[rd] = wsr_read(e_csr); break;
    case kWsrWrite: wsr_write(e_csr, e_wide); break;
    case kNoEffect: break;
  }
  if (insn_cnt_ != 0xffffffffu) ++insn_cnt_;
  cycles_ += cycles;
  if (ecall) {
    ended_ = true;
    return Stop::kEnd;
  }
  if (ended_) return Stop::kEnd;  // a fatal error the instruction's effect raised
  if (loop_end) {
    Loop &l = lstack_.back();
    if (l.restarts) --l.restarts;
    else lstack_.pop_back();
  }
  pc_ = next;
  if (wfi) {
    paused_ = true;
    return Stop::kWfi;
  }
  return Stop::kBudget;
}

}  // namespace eg2_model
