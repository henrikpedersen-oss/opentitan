// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_null:
//   "NULL pointer representation in purecap"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-null">;

// CHERIoT port of the upstream test: NULL has length 0 here (see the file header).
#include "cheriot_clang_purecap_null.c"

int __cheri_compartment("cc_null") run_test_null(void)
{
	return run_test_clang_purecap_null();
}
