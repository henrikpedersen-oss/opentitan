// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_va_copy:
//   "va_copy copies"
//
// DECLARE_TEST — returns 0 on PASS.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-va-copy">;

#include "clang-purecap/clang_purecap_va_copy.c"

int __cheri_compartment("cc_va_copy") run_test_va_copy(void)
{
	return run_test_clang_purecap_va_copy();
}
