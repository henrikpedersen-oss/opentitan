// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// RISC-V architecture-test target for Earl Grey 2. The test begins at RRAM + 0x80, leaves its
// signature in main SRAM and reports completion through the chip DV status word.

#ifndef _COMPLIANCE_MODEL_H
#define _COMPLIANCE_MODEL_H

#include "eg2_verdict.h"

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

#define RVMODEL_HALT EG2_PASS

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

// The tests included by this runner do not request platform interrupt injection.
#define RVMODEL_SET_MSW_INT
#define RVMODEL_CLEAR_MSW_INT
#define RVMODEL_CLEAR_MTIMER_INT
#define RVMODEL_CLEAR_MEXT_INT

#endif
