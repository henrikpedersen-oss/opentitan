// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping libc_malloc:
//   "malloc and calloc give tagged, bounded, aligned, zeroed memory with load and store permissions"
//
// DECLARE_TEST — returns 0 on PASS. From the CHERIoT port cheriot_libc_malloc.c (the changes are in
// its header).

// Room for the largest pair the test holds at once (2 x (2^12 + 1) bytes) and the allocator's
// headers.
#define MALLOC_QUOTA 0x4000

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-libc_malloc">;

#include "cheriot_libc_malloc.c"

int __cheri_compartment("cc_libc_malloc") run_libc_malloc(void)
{
	return run_test_libc_malloc();
}
