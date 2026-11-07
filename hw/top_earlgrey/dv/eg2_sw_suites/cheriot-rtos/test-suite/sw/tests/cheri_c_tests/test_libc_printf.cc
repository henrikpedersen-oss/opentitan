// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping libc_printf:
//   "sprintf writes the formatted string, and returns its length"
//
// DECLARE_TEST — returns 0 on PASS. The RTOS's sprintf (sdk/include/stdio.h) is vsnprintf from the
// stdio library, bounded by the output buffer's capability.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-libc_printf">;

#include "libc/libc_printf.c"

int __cheri_compartment("cc_libc_printf") run_libc_printf(void)
{
	return run_test_libc_printf();
}
