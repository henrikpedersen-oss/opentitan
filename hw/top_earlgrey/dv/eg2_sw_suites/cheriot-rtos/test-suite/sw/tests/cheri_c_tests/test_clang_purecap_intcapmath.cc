// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_intcapmath:
//   "math on __intcap_t"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-intcapmath">;

// CHERIoT port of the upstream test: volatile operands, so the operators run (see its header).
#include "cheriot_clang_purecap_intcapmath.c"

int __cheri_compartment("cc_intcapmath") run_test_intcapmath(void)
{
	return run_test_clang_purecap_intcapmath();
}
