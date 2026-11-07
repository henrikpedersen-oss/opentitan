// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping libc_memcpy:
//   "memcpy keeps the tag of a capability it copies whole and aligned, clears it otherwise,"
//   "and copies the data at every alignment"
//
// DECLARE_TEST — returns 0 on PASS. Upstream calls cheritest_memcpy, an outside function so that the
// compiler cannot replace the library's memcpy with its own copy loop; here it is the RTOS's memcpy
// (the freestanding library), behind a function the compiler may not inline.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>
#include <string.h>

using Debug = ConditionalDebug<true, "cc-libc_memcpy">;

__attribute__((noinline)) void *cheritest_memcpy(void *dst, const void *src, size_t n)
{
	return memcpy(dst, src, n);
}

#include "libc/libc_memcpy.c"

int __cheri_compartment("cc_libc_memcpy") run_libc_memcpy(void)
{
	return run_test_libc_memcpy();
}
