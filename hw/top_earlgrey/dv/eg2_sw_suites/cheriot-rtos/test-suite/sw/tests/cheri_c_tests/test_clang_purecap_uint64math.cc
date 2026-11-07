// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_uint64math:
//   "the same operators as (u)intcapmath on plain 64-bit integers" (the control for the
//   capability versions)
//
// DECLARE_TEST — returns 0 on PASS.
//
// Upstream's clang_purecap_uint64math.c is three defines and an include of uintcapmath.c; this does the
// same with the CHERIoT port of that file (volatile operands, so the operators run on the core,
// through the crt library's 64-bit helpers on RV32). _TEST_INT_TYPE is uint64_t: upstream's
// __uint64_t is a FreeBSD name.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>
#include <stdint.h>

using Debug = ConditionalDebug<true, "cc-uint64math">;

#define _TEST_INT_TYPE uint64_t
#define _TEST_REAL_INTS
#include "cheriot_clang_purecap_uintcapmath.c"

int __cheri_compartment("cc_uint64math") run_test_uint64math(void)
{
	return run_test_clang_purecap_uint64math();
}
