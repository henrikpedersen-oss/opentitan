// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_uintcapmath:
//   "math on __uintcap_t"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-uintcapmath">;

// CHERIoT port of the upstream test: volatile operands, so the operators run (see its header).
#include "cheriot_clang_purecap_uintcapmath.c"

int __cheri_compartment("cc_uintcapmath") run_test_uintcapmath(void)
{
	return run_test_clang_purecap_uintcapmath();
}
