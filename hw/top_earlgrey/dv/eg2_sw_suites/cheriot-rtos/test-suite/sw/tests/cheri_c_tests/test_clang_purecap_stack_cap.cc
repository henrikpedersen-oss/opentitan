// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_stack_cap:
//   "on-stack arrays get non-overlapping bounds covering their size"
//
// DECLARE_TEST — returns 0 on PASS. The arrays and frame take about 5.7 KiB of the caller's stack;
// the cheri_c_tests thread stack in xmake.lua is sized for that. Too small a stack makes the
// frame's stores fault on csp's bounds, which the default error handler logs as CRASH:.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-stack-cap">;

// CHERIoT port of the upstream test: array sizes scaled down to fit the stack (see its header).
#include "cheriot_clang_purecap_stack_cap.c"

int __cheri_compartment("cc_stack_cap") run_test_stack_cap(void)
{
	return run_test_clang_purecap_stack_cap();
}
