// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_lc_invalid_encodings (EG2 verification spec REQ_SEC_06, "Security in RISC-V mode"):
 * every life-cycle encoding that is not a defined state decodes to INVALID, with every function
 * disabled, the CPU included.
 *
 * The check is the bench's (chip_sw_eg2_lc_invalid_encodings_vseq): with the CPU disabled, no
 * software runs in the invalid state. The bench writes an encoding of the drawn class (erased,
 * one word of a valid state altered, random) into the LIFE_CYCLE partition by backdoor, resets,
 * and checks over the LC TAP that LC_STATE reads INVALID, that the CPU never fetches and that the
 * debug TAP is not reachable. It then restores the original encoding and resets once more: this
 * image then runs, and checks that it runs only in a defined state (the oracle: lc_ctrl.hjson
 * LC_STATE's values, device life cycle "INVALID": no CPU).
 *
 * Built with EG2_LC_INVALID_ENCODINGS_INJECT_FAIL the software expects INVALID: the run must fail
 * with `EG2CHK FAIL CPU runs only in a defined state`; its dvsim entry also sets
 * +eg2_inject_fail=1, which makes the bench expect a defined state after the invalid encoding
 * (`EG2CHK FAIL lc_ctrl LC_STATE after an invalid encoding`).
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_boot/eg2_boot.h"

EG2_TEST_CONFIG();

bool eg2_test_main(void) {
  uint32_t s = eg2_lc_state_decoded();
  eg2_boot_note("life-cycle state", s);
  // RAW .. RMA: the defined states below SCRAP (the CPU never runs in SCRAP or INVALID).
  bool defined = s < kEg2LcScrap;
#ifdef EG2_LC_INVALID_ENCODINGS_INJECT_FAIL
  defined = !defined;
#endif
  bool ok = eg2_boot_expect("CPU runs only in a defined state", defined, s);
  return eg2_boot_summary() && ok;
}
