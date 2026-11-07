// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_capret:
//   "return addresses are capabilities"
//
// DECLARE_TEST — returns 0 on PASS. A return address that is not a valid, executable capability
// faults in the callee's `cjr cra`, which the default error handler logs as CRASH: before the
// switcher unwinds the call.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-capret">;

// CHERIoT port of the upstream test: RISC-V CHERIoT assembly replaces the MIPS body (see its
// header).
#include "cheriot_clang_purecap_capret.c"

int __cheri_compartment("cc_capret") run_test_capret(void)
{
	return run_test_clang_purecap_capret();
}
