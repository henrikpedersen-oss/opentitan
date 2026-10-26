// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The instruction sequences of the revocation corner cases (eg2_revoke.h, EG2 spec "Revocation",
 * "Corner cases"), each with nothing in between its instructions. CHERIoT mode only.
 *
 * A capability loaded here may be based in a marked granule, so none is held in ct1 or ct2: the
 * expected-trap handler (eg2_probe.S) saves and restores those two through memory on an interrupt,
 * and its restoring CLC would revoke them (the RTOS's preemption case,
 * chip_sw_cheriot_revocation_irq). Only x0-x15 are used.
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_revoke, "ax", @progbits

// void eg2_rv_burst6(void *slots /*ca0*/, void *out /*ca1*/): six CLCs back to back from
// slots[0..5], then six CSCs of the loaded registers to out[0..5].
  .balign 4
  .globl eg2_rv_burst6
  .type eg2_rv_burst6, @function
eg2_rv_burst6:
  cincoffset csp, csp, -16
  csc        cs0, 0(csp)
  csc        cs1, 8(csp)
  clc        ca2, 0(ca0)
  clc        ca3, 8(ca0)
  clc        ca4, 16(ca0)
  clc        ca5, 24(ca0)
  clc        cs0, 32(ca0)
  clc        cs1, 40(ca0)
  csc        ca2, 0(ca1)
  csc        ca3, 8(ca1)
  csc        ca4, 16(ca1)
  csc        ca5, 24(ca1)
  csc        cs0, 32(ca1)
  csc        cs1, 40(ca1)
  clc        cs0, 0(csp)
  clc        cs1, 8(csp)
  cincoffset csp, csp, 16
  cret
  .size eg2_rv_burst6, . - eg2_rv_burst6

// uint32_t eg2_rv_clc_tag(void *slot /*ca0*/): CLC, and CGetTag of the loaded register as the very
// next instruction.
  .balign 4
  .globl eg2_rv_clc_tag
  .type eg2_rv_clc_tag, @function
eg2_rv_clc_tag:
  clc        ca2, 0(ca0)
  cgettag    a0, ca2
  cret
  .size eg2_rv_clc_tag, . - eg2_rv_clc_tag

// void eg2_rv_clc_csc(void *src /*ca0*/, void *dst /*ca1*/): CLC, then CSC of the same register.
  .balign 4
  .globl eg2_rv_clc_csc
  .type eg2_rv_clc_csc, @function
eg2_rv_clc_csc:
  clc        ca2, 0(ca0)
  csc        ca2, 0(ca1)
  cret
  .size eg2_rv_clc_csc, . - eg2_rv_clc_csc

// uint32_t eg2_rv_csc_clc(void *slot /*ca0*/, uint32_t base /*a1*/, uint32_t len /*a2*/): a
// capability over [base, base + len) from MTDC (built here, so no register holds it across a
// call), CSC to the slot, then CLC of the same address straight after; returns the loaded tag.
  .balign 4
  .globl eg2_rv_csc_clc
  .type eg2_rv_csc_clc, @function
eg2_rv_csc_clc:
  cspecialr  ca3, mtdc
  csetaddr   ca3, ca3, a1
  csetbounds ca3, ca3, a2
  csc        ca3, 0(ca0)
  clc        ca4, 0(ca0)
  cgettag    a0, ca4
  cret
  .size eg2_rv_csc_clc, . - eg2_rv_csc_clc

// ------------------------------ Probe stubs (eg2_probe.h) ------------------------------
// ca0, ca1 operands, ca2 the initial value; results in a2 (and a1); ca3 and ca5 are the probe's.

// CLC from ca0, then a word load through the loaded capability (the dependent use).
  .balign 8
  .globl eg2_rv_stub_clc_lw
  .type eg2_rv_stub_clc_lw, @function
eg2_rv_stub_clc_lw:
  clc        ca2, 0(ca0)
  lw         a2, 0(ca2)
  cret
  .size eg2_rv_stub_clc_lw, . - eg2_rv_stub_clc_lw

// A bitmap write and a CLC with nothing in between, then the clear and a CLC: ca0 the bitmap word,
// a1 the word with the mark, ca2 the slot. a1 = the tag after the mark, a2 = the tag after the
// clear (the word written back as 0).
  .balign 8
  .globl eg2_rv_stub_mark_sw
  .type eg2_rv_stub_mark_sw, @function
eg2_rv_stub_mark_sw:
  sw         a1, 0(ca0)
  clc        ca4, 0(ca2)
  cgettag    a1, ca4
  sw         zero, 0(ca0)
  clc        ca4, 0(ca2)
  cgettag    a2, ca4
  cret
  .size eg2_rv_stub_mark_sw, . - eg2_rv_stub_mark_sw

// The same with a halfword and a byte write (a1 the halfword or byte with the mark, at ca0's
// address): the revbm window takes full words only (theory_of_operation.md), so the write is
// expected to fault and the CLCs not to run.
  .balign 8
  .globl eg2_rv_stub_mark_sh
  .type eg2_rv_stub_mark_sh, @function
eg2_rv_stub_mark_sh:
  sh         a1, 0(ca0)
  clc        ca4, 0(ca2)
  cgettag    a1, ca4
  sh         zero, 0(ca0)
  clc        ca4, 0(ca2)
  cgettag    a2, ca4
  cret
  .size eg2_rv_stub_mark_sh, . - eg2_rv_stub_mark_sh

  .balign 8
  .globl eg2_rv_stub_mark_sb
  .type eg2_rv_stub_mark_sb, @function
eg2_rv_stub_mark_sb:
  sb         a1, 0(ca0)
  clc        ca4, 0(ca2)
  cgettag    a1, ca4
  sb         zero, 0(ca0)
  clc        ca4, 0(ca2)
  cgettag    a2, ca4
  cret
  .size eg2_rv_stub_mark_sb, . - eg2_rv_stub_mark_sb

// ------------------------------ Op windows (eg2_rv_ops_run) ------------------------------
// void eg2_rv_run(const uint32_t *ops /*ca0*/, uint32_t n /*a1*/, void *const *auths /*ca2*/,
//                 uint32_t *out /*ca3*/): runs n ops of three words {kind | auth << 8, addr,
// value}, with no call and no other code in between, so that a bitmap bit the window sets is set
// only while it runs (eg2_revoke.h, "Op windows"). Each op's address is set on authority
// auths[auth] (CSetAddr). A loaded capability is held in ca4 only until its CGetTag, then ca4 is
// cleared. Uses x5 and x8-x15 (not ct1, ct2).
  .balign 4
  .globl eg2_rv_run
  .type eg2_rv_run, @function
eg2_rv_run:
  cincoffset csp, csp, -16
  csc        cs0, 0(csp)
  csc        cs1, 8(csp)
.Lrun_next:
  beqz       a1, .Lrun_done
  lw         s0, 0(ca0)
  srli       a5, s0, 8
  andi       a5, a5, 15
  slli       a5, a5, 3
  cincoffset ca4, ca2, a5
  clc        ca4, 0(ca4)
  lw         a5, 4(ca0)
  csetaddr   ca4, ca4, a5
  lw         a5, 8(ca0)
  andi       s0, s0, 0xff
  cincoffset ca0, ca0, 12
  addi       a1, a1, -1
  li         t0, 1
  beq        s0, t0, .Lrun_sw
  li         t0, 2
  beq        s0, t0, .Lrun_lw
  li         t0, 3
  beq        s0, t0, .Lrun_clc
  li         t0, 4
  beq        s0, t0, .Lrun_poll_clear
  li         t0, 5
  beq        s0, t0, .Lrun_poll_eq
  li         t0, 6
  beq        s0, t0, .Lrun_spin
  li         t0, 7
  beq        s0, t0, .Lrun_mcycle
  li         t0, 8
  beq        s0, t0, .Lrun_poll_set
  li         t0, 9
  beq        s0, t0, .Lrun_wfi
  j          .Lrun_next
// 1: store `value` at addr.
.Lrun_sw:
  sw         a5, 0(ca4)
  j          .Lrun_next
// 2: load addr to the next out word.
.Lrun_lw:
  lw         s1, 0(ca4)
.Lrun_out:
  sw         s1, 0(ca3)
  cincoffset ca3, ca3, 4
  j          .Lrun_next
// 3: CLC from addr, its tag to the next out word.
.Lrun_clc:
  clc        ca4, 0(ca4)
  cgettag    s1, ca4
  li         a4, 0
  j          .Lrun_out
// 4: load addr until (word & value) == 0; 8: until (word & value) != 0; 5: until word == value.
// The out word is the polls left of 2^24 (0: it never happened).
.Lrun_poll_clear:
  lui        t0, 0x1000
.Lrun_pc_loop:
  lw         s1, 0(ca4)
  and        s1, s1, a5
  beqz       s1, .Lrun_poll_end
  addi       t0, t0, -1
  bnez       t0, .Lrun_pc_loop
.Lrun_poll_end:
  mv         s1, t0
  j          .Lrun_out
.Lrun_poll_set:
  lui        t0, 0x1000
.Lrun_ps_loop:
  lw         s1, 0(ca4)
  and        s1, s1, a5
  bnez       s1, .Lrun_poll_end
  addi       t0, t0, -1
  bnez       t0, .Lrun_ps_loop
  j          .Lrun_poll_end
.Lrun_poll_eq:
  lui        t0, 0x1000
.Lrun_pe_loop:
  lw         s1, 0(ca4)
  beq        s1, a5, .Lrun_poll_end
  addi       t0, t0, -1
  bnez       t0, .Lrun_pe_loop
  j          .Lrun_poll_end
// 6: `value` loop iterations.
.Lrun_spin:
  beqz       a5, .Lrun_next
  addi       a5, a5, -1
  j          .Lrun_spin
// 7: mcycle to the next out word.
.Lrun_mcycle:
  csrr       s1, mcycle
  j          .Lrun_out
// 9: wfi.
.Lrun_wfi:
  wfi
  j          .Lrun_next
.Lrun_done:
  clc        cs0, 0(csp)
  clc        cs1, 8(csp)
  cincoffset csp, csp, 16
  cret
  .size eg2_rv_run, . - eg2_rv_run

#endif  // __CHERIOT__
