// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_smallint:
//   "small-integer tagging in low bits of an aligned capability pointer"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-smallint">;

// _Alignas is a C11 keyword with no C++ spelling; this wrapper compiles the upstream
// C file as C++, where it parses as a call and yields "error: expected expression"
// plus cascading "undeclared identifier" errors for the variable it qualifies.
// alignas is the C++ equivalent and accepts the same integral argument.
#ifndef _Alignas
#define _Alignas(x) alignas(x)
#endif

#include "clang-purecap/clang_purecap_smallint.c"

int __cheri_compartment("cc_smallint") run_test_smallint(void)
{
	return run_test_clang_purecap_smallint();
}
