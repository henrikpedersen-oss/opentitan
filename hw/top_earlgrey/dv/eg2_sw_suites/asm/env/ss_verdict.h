// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The assembler suite's verdict on Ibex's Simple System: four characters and a newline to the
// console (SimCtrl at 0x20000, one byte per word written), then the run is stopped (bit 0 of
// SimCtrl's control word, 0x20008). run.sh reads the console log (ibex_simple_system.log).

#ifndef SS_VERDICT_H
#define SS_VERDICT_H

#define SS_CONSOLE 0x20000
#define SS_PUTC(c) li x1, c; sw x1, 0(x2);
#define SS_VERDICT(a, b, c, d)                                          \
        fence;                                                          \
        li x2, SS_CONSOLE;                                              \
        SS_PUTC(a) SS_PUTC(b) SS_PUTC(c) SS_PUTC(d) SS_PUTC(10)         \
        li x1, 1;                                                       \
        sw x1, 8(x2);                                                   \
        2:;                                                             \
        j 2b;

// "PASS" and "FAIL"
#define SS_PASS SS_VERDICT(80, 65, 83, 83)
#define SS_FAIL SS_VERDICT(70, 65, 73, 76)

#endif
