// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * OTTF trap entry for CHERIoT mode (replaces ottf_isrs.S in a CHERIoT build).
 *
 * It keeps OTTF's contract with its C handlers (ottf_isrs.c and the handlers tests override):
 *  - each handler gets `uint32_t *exc_info`, OTTF's 30-word frame: [0] the return address
 *    (`mepc`), [1..28] ra, t0-t2, s0-s1, a0-a7, s2-s11, t3-t6, [29] `mstatus`. CHERIoT mode has
 *    x1-x15 only, so the words of x16-x31 read 0;
 *  - for an exception other than an instruction access fault, [0] is already advanced past the
 *    faulting instruction (2 or 4 bytes), so a handler that returns resumes after it;
 *  - on return the address in [0] and the `mstatus` in [29] are used, so a handler can change
 *    where execution resumes, as on RISC-V.
 * Underneath, the registers are saved and restored as capabilities, and `mepcc` keeps its
 * bounds and permissions: only its address comes from [0].
 *
 * Interrupts go to ottf_software_isr (cause 3), ottf_timer_isr (7), ottf_external_isr (11) and
 * ottf_internal_isr (any other); exceptions to ottf_exception_handler. CHERIoT's MTCC has no
 * vectored mode, so the cause is dispatched here. FreeRTOS tasks (`enable_concurrency`) are not
 * supported in CHERIoT mode; the frame is always on the interrupted stack.
 */

#define WORD 4
#define CAP 8
#define FRAME_WORDS 30
// The integer frame, rounded up to capability alignment, then x1-x15 and MEPCC.
#define CAPS_OFFSET 128
#define CAP_OFFSET(x) (CAPS_OFFSET + ((x) - 1) * CAP)
#define MEPCC_OFFSET (CAPS_OFFSET + 15 * CAP)
#define FRAME_SIZE (MEPCC_OFFSET + CAP)

  .section .text.ottf_isrs_cheriot, "ax", @progbits
  .option push
  .option norvc
  .option norelax

  .balign 256
  .globl ottf_cheriot_trap_entry
  .type ottf_cheriot_trap_entry, @function
ottf_cheriot_trap_entry:
  cincoffsetimm csp, csp, -FRAME_SIZE

  // Capabilities x1, x3-x15 (x2, CSP, is restored by arithmetic on exit).
  csc  cra, CAP_OFFSET(1)(csp)
  csc  cgp, CAP_OFFSET(3)(csp)
  csc  ctp, CAP_OFFSET(4)(csp)
  csc  ct0, CAP_OFFSET(5)(csp)
  csc  ct1, CAP_OFFSET(6)(csp)
  csc  ct2, CAP_OFFSET(7)(csp)
  csc  cs0, CAP_OFFSET(8)(csp)
  csc  cs1, CAP_OFFSET(9)(csp)
  csc  ca0, CAP_OFFSET(10)(csp)
  csc  ca1, CAP_OFFSET(11)(csp)
  csc  ca2, CAP_OFFSET(12)(csp)
  csc  ca3, CAP_OFFSET(13)(csp)
  csc  ca4, CAP_OFFSET(14)(csp)
  csc  ca5, CAP_OFFSET(15)(csp)

  // OTTF's integer frame: the addresses of the registers.
  sw   ra,  1 * WORD(csp)
  sw   t0,  2 * WORD(csp)
  sw   t1,  3 * WORD(csp)
  sw   t2,  4 * WORD(csp)
  sw   s0,  5 * WORD(csp)
  sw   s1,  6 * WORD(csp)
  sw   a0,  7 * WORD(csp)
  sw   a1,  8 * WORD(csp)
  sw   a2,  9 * WORD(csp)
  sw   a3, 10 * WORD(csp)
  sw   a4, 11 * WORD(csp)
  sw   a5, 12 * WORD(csp)
  // a6-a7, s2-s11, t3-t6: not in CHERIoT mode.
  addi t0, x0, 13
.L_zero_absent:
  slli t1, t0, 2
  cincoffset ct1, csp, t1
  sw   x0, 0(ct1)
  addi t0, t0, 1
  addi t1, x0, 29
  blt  t0, t1, .L_zero_absent
  csrr t0, mstatus
  sw   t0, 29 * WORD(csp)

  // MEPCC, and the return address in [0].
  cspecialr ct0, mepcc
  csc  ct0, MEPCC_OFFSET(csp)
  csrr t1, mcause
  bltz t1, .L_frame_done           // interrupt: resume at the interrupted instruction
  addi t2, x0, 1
  beq  t1, t2, .L_frame_done       // instruction access fault: no instruction to step over
  // A CHERI fault on PCC itself (mtval register field 0x20, the special register PCC): MEPCC
  // cannot be read through, and there is no instruction to step over.
  addi t2, x0, 28
  bne  t1, t2, .L_read_insn
  csrr t2, mtval
  srli t2, t2, 5
  addi t2, t2, -0x20
  beqz t2, .L_frame_done
.L_read_insn:
  clhu t2, 0(ct0)                  // the faulting instruction's first half
  andi t2, t2, 0x3
  addi t1, x0, 0x3
  beq  t2, t1, .L_step_four
  cincoffsetimm ct0, ct0, 2
  j    .L_frame_done
.L_step_four:
  cincoffsetimm ct0, ct0, 4
.L_frame_done:
  sw   t0, 0(csp)

  // Dispatch on the cause; the handler gets the frame in ca0.
  cmove ca0, csp
  csrr t0, mcause
  bgez t0, .L_exception
  andi t0, t0, 0x1f
  addi t1, x0, 3
  beq  t0, t1, .L_software
  addi t1, x0, 7
  beq  t0, t1, .L_timer
  addi t1, x0, 11
  beq  t0, t1, .L_external
  ccall ottf_internal_isr
  j    .L_exit
.L_software:
  ccall ottf_software_isr
  j    .L_exit
.L_timer:
  ccall ottf_timer_isr
  j    .L_exit
.L_external:
  ccall ottf_external_isr
  j    .L_exit
.L_exception:
  ccall ottf_exception_handler

.L_exit:
  // MEPCC with the address the frame now holds; MSTATUS from the frame.
  clc  ct0, MEPCC_OFFSET(csp)
  lw   t1, 0(csp)
  csetaddr ct0, ct0, t1
  cspecialw mepcc, ct0
  lw   t0, 29 * WORD(csp)
  csrw mstatus, t0

  clc  cra, CAP_OFFSET(1)(csp)
  clc  cgp, CAP_OFFSET(3)(csp)
  clc  ctp, CAP_OFFSET(4)(csp)
  clc  ct0, CAP_OFFSET(5)(csp)
  clc  ct1, CAP_OFFSET(6)(csp)
  clc  ct2, CAP_OFFSET(7)(csp)
  clc  cs0, CAP_OFFSET(8)(csp)
  clc  cs1, CAP_OFFSET(9)(csp)
  clc  ca0, CAP_OFFSET(10)(csp)
  clc  ca1, CAP_OFFSET(11)(csp)
  clc  ca2, CAP_OFFSET(12)(csp)
  clc  ca3, CAP_OFFSET(13)(csp)
  clc  ca4, CAP_OFFSET(14)(csp)
  clc  ca5, CAP_OFFSET(15)(csp)
  cincoffsetimm csp, csp, FRAME_SIZE
  mret

  .size ottf_cheriot_trap_entry, . - ottf_cheriot_trap_entry

/**
 * Points MTCC at `ottf_cheriot_trap_entry`: PCC (the executable root's bounds and permissions,
 * which include access to the special registers) with the entry's address.
 */
  .balign 4
  .globl ottf_cheriot_trap_install
  .type ottf_cheriot_trap_install, @function
ottf_cheriot_trap_install:
  auipcc    ct0, 0
  lui       t1, %hi(ottf_cheriot_trap_entry)
  addi      t1, t1, %lo(ottf_cheriot_trap_entry)
  csetaddr  ct0, ct0, t1
  cspecialw mtcc, ct0
  cret
  .size ottf_cheriot_trap_install, . - ottf_cheriot_trap_install

  .option pop
