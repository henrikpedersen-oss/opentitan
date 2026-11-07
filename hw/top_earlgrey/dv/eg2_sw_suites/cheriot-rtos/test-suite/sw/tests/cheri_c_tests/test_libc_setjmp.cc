// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping libc_setjmp:
//   "longjmp returns to setjmp, from a nested call"
//
// DECLARE_TEST — returns 0 on PASS. From the CHERIoT port cheriot_libc_setjmp.c (the change is in
// its header).


#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-libc_setjmp">;

#include "cheriot_libc_setjmp.c"

int __cheri_compartment("cc_libc_setjmp") run_libc_setjmp(void)
{
	return run_test_libc_setjmp();
}
