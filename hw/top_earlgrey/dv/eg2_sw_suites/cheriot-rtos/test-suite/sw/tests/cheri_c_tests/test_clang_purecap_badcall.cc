// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_badcall (FAULT test):
//   "call through an integer-derived function pointer → CHERI tag fault"
//
// DECLARE_TEST_FAULT.  Upstream makes two bad calls and counts a fault for each
// (FreeBSD resumable-exception model):
//   1. x(), with x = (fnptr)(__uintcap_t)42: a pointer made from an integer, so
//      untagged;
//   2. not_a_function(), a global initialised to &a: a tagged, unsealed data
//      capability without Execute.
// On CHERIoT the first fault ends the compartment call (ForceUnwind), so only
// call 1 runs. CJALR checks the tag first (CHERIoT-Sail cheri_insts.sail,
// CJALR: tag, then seal/sentry type, then Permit_Execute), so the architectural
// result of call 1 is a tag violation (capcause 0x02) on the register holding x
// -- not an execute-permission fault, whatever upstream's name suggests. This
// test therefore checks the tag check on capability jumps, and requires exactly
// that cause (../common/expected_fault.hh); the register is the compiler's
// choice and is not checked. The execute-permission check, call 2's case, is
// clang_purecap_noexec_call (test_clang_purecap_noexec_call.cc).

#define TEST_CUSTOM_FRAMEWORK
// This wrapper defines its own compartment_error_handler (cheri_c_test_framework.h).
#define CHERI_C_OWN_ERROR_HANDLER
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-badcall">;

#include "clang-purecap/clang_purecap_badcall.c"

#include "../common/expected_fault.hh"

extern "C" ErrorRecoveryBehaviour
compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
{
	return expected_fault_handler(frame, mcause, mtval, CHERI::CauseCode::TagViolation);
}

void *__cheri_compartment("cc_badcall") run_fault_badcall(void)
{
	expected_fault_begin();
	run_test_clang_purecap_badcall();
	Debug::log("WARNING: no CHERI fault triggered in clang_purecap_badcall");
	return nullptr;
}

// Assertion failures before the fault, a fault that is not a tag violation, or no fault.
int __cheri_compartment("cc_badcall") run_fault_badcall_failures(void)
{
	return expected_fault_failures(test_failures);
}
