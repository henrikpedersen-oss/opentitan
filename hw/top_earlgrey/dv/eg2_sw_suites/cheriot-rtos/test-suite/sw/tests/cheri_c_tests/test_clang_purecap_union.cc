// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_union:
//   "unions containing capabilities: tag cleared by integer word write"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-union">;

// CHERIoT port of the upstream test: exact-length check not applicable (see the file header).
#include "cheriot_clang_purecap_union.c"

int __cheri_compartment("cc_union") run_test_union(void)
{
	return run_test_clang_purecap_union();
}
