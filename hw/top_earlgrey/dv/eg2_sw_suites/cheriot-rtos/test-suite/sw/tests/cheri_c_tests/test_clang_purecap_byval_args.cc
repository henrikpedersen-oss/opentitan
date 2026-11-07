// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_byval_args:
//   "structs passed by value are copied"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-byval-args">;

// CHERIoT port of the upstream test: a compiler barrier keeps the by-value call (see its header).
#include "cheriot_clang_purecap_byval_args.c"

int __cheri_compartment("cc_byval_args") run_test_byval_args(void)
{
	return run_test_clang_purecap_byval_args();
}
