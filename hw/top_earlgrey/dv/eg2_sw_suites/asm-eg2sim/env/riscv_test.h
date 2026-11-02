// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// riscv-tests on Earl Grey 2: use Ibex's environment, but send its PASS and FAIL convention to
// the chip DV status word. run.sh also changes the environment's unexpected-trap tohost write into
// a terminal failure write before compiling it.

#include_next <riscv_test.h>
#include "eg2_verdict.h"

#undef RVTEST_PASS
#define RVTEST_PASS EG2_PASS

#undef RVTEST_FAIL
#define RVTEST_FAIL EG2_FAIL

#undef RVTEST_DATA_BEGIN
#define RVTEST_DATA_BEGIN                                               \
        EXTRA_DATA                                                      \
        .align 4; .global begin_signature; begin_signature:
