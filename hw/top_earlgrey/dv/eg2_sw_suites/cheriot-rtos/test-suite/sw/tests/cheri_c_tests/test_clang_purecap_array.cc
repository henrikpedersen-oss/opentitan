// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_array (FAULT test):
//   "on-stack array accessed out-of-bounds → CHERI bounds fault"
//
// DECLARE_TEST_FAULT.  The original test accesses an int* derived from an
// 8-byte char buffer beyond its bounds; the first out-of-bounds int store
// (x[2]) must raise a CHERI bounds violation (capcause 0x01).  In CHERIoT the
// compartment_error_handler returns ForceUnwind, terminating the compartment
// call; the orchestrator verifies the returned capability is invalid
// (address == -1) and then asks run_fault_array_failures() whether the fault
// was a bounds violation and whether an assertion failed before it
// (../common/expected_fault.hh). With the bounds check broken, the loop
// overwrites the frame, the final assert_eq fails and the return faults on a
// tag violation instead: both are now failures.
//
// The original test counts multiple faults (FreeBSD resumable-exception
// model); here the first fault ends the test.

#define TEST_CUSTOM_FRAMEWORK
// This wrapper defines its own compartment_error_handler (cheri_c_test_framework.h).
#define CHERI_C_OWN_ERROR_HANDLER
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-array">;

// _Alignas is a C11 keyword with no C++ spelling; this wrapper compiles the upstream
// C file as C++, where it parses as a call and yields "error: expected expression"
// plus cascading "undeclared identifier" errors for the variable it qualifies.
// alignas is the C++ equivalent and accepts the same integral argument.
#ifndef _Alignas
#define _Alignas(x) alignas(x)
#endif

#include "clang-purecap/clang_purecap_array.c"

#include "../common/expected_fault.hh"

// Called by the CHERIoT switcher on any CHERI hardware exception inside this compartment.
extern "C" ErrorRecoveryBehaviour
compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
{
	return expected_fault_handler(frame, mcause, mtval, CHERI::CauseCode::BoundsViolation);
}

void *__cheri_compartment("cc_array") run_fault_array(void)
{
	expected_fault_begin();
	run_test_clang_purecap_array();
	// Reached only if no fault fires — unexpected for this test.
	Debug::log("WARNING: no CHERI fault triggered in clang_purecap_array");
	return nullptr;
}

// Assertion failures before the fault, a fault that is not a bounds violation, or no fault.
// test_failures is reset by the test body and survives the unwind, as compartment globals do.
int __cheri_compartment("cc_array") run_fault_array_failures(void)
{
	return expected_fault_failures(test_failures);
}
