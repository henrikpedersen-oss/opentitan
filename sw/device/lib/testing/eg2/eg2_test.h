// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_TEST_H_
#define OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_TEST_H_

/**
 * @file
 * @brief The one include of an EG2 framework test, for both execution modes.
 *
 * A test defines its body as `bool eg2_test_main(void)`, states its configuration with
 * `EG2_TEST_CONFIG()` and, if it has knobs, declares them with `EG2_TEST_PARAMS()`
 * (eg2_params.h). Nothing in it names the mode: the RISC-V build runs it under OTTF, the CHERIoT
 * build (platform `CHERIOT_PLATFORM`) under the framework's own start-up code, which switches and
 * locks CHERIoT mode first (doc/eg2_test_framework.md in eg2_top_verification).
 *
 * The framework's `test_main` wraps the body in both modes:
 *   1. print the seed and the parameters, and check each against its range;
 *   2. hand the alert handler to the status module (`eg2_status_init`);
 *   3. run `eg2_test_main`;
 *   4. the final status sweep and the GPIO verdict (`eg2_test_end`).
 * The verdict is the body's and the sweep's together.
 */

#include <stdbool.h>

#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_params.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"

/** The test's body; returns its own verdict. */
bool eg2_test_main(void);

#ifdef __CHERIOT__

/** CHERIoT mode has no OTTF; the configuration is taken but not used yet. */
#define EG2_TEST_CONFIG(...) \
  static const char kEg2TestFile[] __attribute__((unused)) = __FILE__

#else  // RISC-V mode: OTTF.

#include "sw/device/lib/testing/test_framework/ottf_test_config.h"

/**
 * OTTF's test configuration. The status module owns the alert handler, so OTTF's alert catcher
 * (which fails a test on any alert) is off.
 */
#define EG2_TEST_CONFIG(...) OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true, ##__VA_ARGS__)

#endif  // __CHERIOT__

#endif  // OPENTITAN_SW_DEVICE_LIB_TESTING_EG2_EG2_TEST_H_
