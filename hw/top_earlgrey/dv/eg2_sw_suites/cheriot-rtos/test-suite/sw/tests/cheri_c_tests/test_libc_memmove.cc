// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping libc_memmove:
//   "memmove of overlapping capabilities keeps them valid"
//
// DECLARE_TEST — returns 0 on PASS. Upstream calls cheritest_memmove, an outside function so that the
// compiler cannot replace the library's memmove with its own copy loop; here it is the RTOS's memmove
// (the freestanding library), behind a function the compiler may not inline.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>
#include <string.h>

using Debug = ConditionalDebug<true, "cc-libc_memmove">;

__attribute__((noinline)) void *cheritest_memmove(void *dst, const void *src, size_t n)
{
	return memmove(dst, src, n);
}

#include "libc/libc_memmove.c"

int __cheri_compartment("cc_libc_memmove") run_libc_memmove(void)
{
	return run_test_libc_memmove();
}
