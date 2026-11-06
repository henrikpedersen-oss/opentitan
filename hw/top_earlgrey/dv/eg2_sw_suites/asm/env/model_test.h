// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The RISC-V architecture tests' target macros for Ibex's Simple System (in place of Ibex's
// vendor/riscv-isa-sim/arch_test_target/spike/model_test.h, which writes the UVM bench's
// signature address). Code starts at the boot address + 0x80 (0x100080), where Ibex starts; the
// end of the test prints PASS and stops the run. The signature is not compared with a reference
// (the vendored tests have none): Spike checks every instruction and every store the core makes.

#ifndef _COMPLIANCE_MODEL_H
#define _COMPLIANCE_MODEL_H

#include "ss_verdict.h"

#if XLEN == 64
  #define ALIGNMENT 3
#else
  #define ALIGNMENT 2
#endif

#define RVMODEL_BOOT                                                    \
        .section .text.init;                                            \
        .org 0x80;                                                      \
        .globl _start;                                                  \
_start:                                                                 \
        .option push;                                                   \
        .option norvc;                                                  \
        j rvtest_init;                                                  \
        .option pop;

#define RVMODEL_HALT SS_PASS

#define RVMODEL_DATA_BEGIN                                              \
  .align 4; .global begin_signature; begin_signature:

#define RVMODEL_DATA_END                                                \
  .align 4; .global end_signature; end_signature:

#define RVMODEL_IO_INIT
#define RVMODEL_IO_WRITE_STR(_R, _STR)
#define RVMODEL_IO_CHECK()
#define RVMODEL_IO_ASSERT_GPR_EQ(_S, _R, _I)
#define RVMODEL_IO_ASSERT_SFPR_EQ(_F, _R, _I)
#define RVMODEL_IO_ASSERT_DFPR_EQ(_D, _R, _I)

// Simple System has no software interrupt register: these are left empty (the interrupt tests
// that need them fail, and say so).
#define RVMODEL_SET_MSW_INT
#define RVMODEL_CLEAR_MSW_INT
#define RVMODEL_CLEAR_MTIMER_INT
#define RVMODEL_CLEAR_MEXT_INT

#endif
