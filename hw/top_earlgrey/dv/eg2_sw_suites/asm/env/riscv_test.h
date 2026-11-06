// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// riscv-tests on Ibex's Simple System: Ibex's own environment (the submodule's
// vendor/riscv-test-env/p/riscv_test.h, found next on the include path), with the verdict sent to
// Simple System's console instead of the UVM bench's signature address, and no .tohost section
// (tohost is SimCtrl's control word, set in link.ld: a write with bit 0 set, as the environment's
// "unexpected trap" path makes, stops the run without a verdict).

#include_next <riscv_test.h>
#include "ss_verdict.h"

#undef RVTEST_PASS
#define RVTEST_PASS SS_PASS

#undef RVTEST_FAIL
#define RVTEST_FAIL SS_FAIL

#undef RVTEST_DATA_BEGIN
#define RVTEST_DATA_BEGIN                                               \
        EXTRA_DATA                                                      \
        .align 4; .global begin_signature; begin_signature:
