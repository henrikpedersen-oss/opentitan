// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Report an assembler test's verdict through Earl Grey 2's DV status word. These values are the
// software test framework's kTestStatusPassed and kTestStatusFailed; eg2sim ends on either write.

#ifndef EG2_VERDICT_H
#define EG2_VERDICT_H

#define EG2_DV_STATUS 0x411f0080
#define EG2_VERDICT(value)                                               \
        fence;                                                           \
        li x2, EG2_DV_STATUS;                                            \
        li x1, value;                                                    \
        sw x1, 0(x2);                                                    \
        2:;                                                              \
        j 2b;

#define EG2_PASS EG2_VERDICT(0x900d)
#define EG2_FAIL EG2_VERDICT(0xbaad)

#endif
