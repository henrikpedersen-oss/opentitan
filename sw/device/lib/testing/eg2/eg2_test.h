// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_TEST_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_TEST_H_

/**
 * @file
 * @brief The one include of an EG2 framework test, for both execution modes.
 *
 * A test defines `bool test_main(void)` and states its configuration with `EG2_TEST_CONFIG()`.
 * Nothing in it names the mode: the RISC-V build runs it under OTTF, the CHERIoT build
 * (platform `CHERIOT_PLATFORM`) under the framework's own start-up code, which switches and
 * locks CHERIoT mode first (doc/eg2_test_framework.md in eg2_top_verification).
 *
 * A test ends with `return eg2_test_end(passed);`: the final status sweep and the GPIO verdict.
 */

#include <stdbool.h>

#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"

#ifdef __CHERIOT__

bool test_main(void);

/** CHERIoT mode has no OTTF; the configuration is taken but not used yet. */
#define EG2_TEST_CONFIG(...) \
  static const char kEg2TestFile[] __attribute__((unused)) = __FILE__

#else  // RISC-V mode: OTTF.

#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/lib/testing/test_framework/ottf_test_config.h"

/**
 * OTTF's test configuration. The status module owns the alert handler, so OTTF's alert catcher
 * (which fails a test on any alert) is off.
 */
#define EG2_TEST_CONFIG(...) OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true, ##__VA_ARGS__)

#endif  // __CHERIOT__

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_TEST_H_
