// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_va_args:
//   "capabilities and ints passed through variadic arguments"
//
// Upstream declares this DECLARE_TEST_FAULT, but the test expects no fault: it defines no
// TEST_EXPECTED_FAULTS and ends with assert_eq(faults, 0). It runs here as a normal test and
// returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-va-args">;

// CHERIoT port of the upstream test: no bounds or permission checks on the va_list (see its
// header).
#include "cheriot_clang_purecap_va_args.c"

int __cheri_compartment("cc_va_args") run_test_va_args(void)
{
	return run_test_clang_purecap_va_args();
}
