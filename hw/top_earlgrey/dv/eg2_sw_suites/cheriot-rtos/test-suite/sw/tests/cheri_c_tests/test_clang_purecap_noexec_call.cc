// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment implementing clang_purecap_noexec_call (FAULT test):
//   "jump through a tagged, unsealed capability without Execute → CHERI execute fault"
//
// No upstream file: this is the second half of upstream clang_purecap_badcall (a call through a
// global initialised to a data pointer), which on CHERIoT never runs -- badcall's first call, through
// an untagged pointer, faults on the tag and the ForceUnwind ends the compartment call
// (test_clang_purecap_badcall.cc). Without it no test reached the execute-permission check of a
// capability jump: +cheriot_rtos_mutate=exec_off left the whole CHERIoT RTOS regression green.
//
// The jump is in assembly so the register is known. A capability to a global data object
// (tagged, unsealed, the global's load/store permissions, no Execute) goes to ca0 (c10) and is
// jumped to with `cjalr cra, ca0`. CHERIoT-Sail (cheri_insts.sail, CJALR) checks, on cs1, the
// tag, then the seal against the sentry types a call allows (an unsealed capability passes), then
// Permit_Execute, and raises CapEx_PermitExecuteViolation on cs1: capcause 0x11, register 10
// (mtval 0x151). The handler requires exactly that (../common/expected_fault.hh). Before the
// jump the test checks that the capability really is tagged, unsealed and without Execute; if
// not, it reports that and does not jump.
//
// If the check is broken and the jump is taken, PCC becomes the data capability and the core
// fetches the object's zero words: either the fetch faults on PCC (register 0x20, not ca0) or
// the core executes c.unimp (mcause 2). Both are the wrong fault, so the test reports FAIL.

// This wrapper defines its own compartment_error_handler (cheri_c_test_framework.h).
#define CHERI_C_OWN_ERROR_HANDLER
#include <cheri.hh>
#include <compartment.h>
#include <debug.hh>
#include <stdint.h>
#include "cheriot_perms.h"

using Debug = ConditionalDebug<true, "cc-noexec-call">;

#include "../common/expected_fault.hh"

// This wrapper does not include the CHERI-C framework, so it counts its own failed checks.
static int test_failures = 0;

// The jump target: data, not code. Not const: clang may derive a pointer to read-only data from
// PCC, which has Execute. Zero-initialised, so a jump that is taken by mistake finds c.unimp.
uint32_t notCode[4];

extern "C" ErrorRecoveryBehaviour
compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
{
	return expected_fault_handler(frame,
	                              mcause,
	                              mtval,
	                              CHERI::CauseCode::PermitExecuteViolation,
	                              CHERI::RegisterNumber::CA0);
}

// Jump to `target` through ca0, linking in cra, as a call through a function pointer does.
__attribute__((noinline)) static void jump_through_ca0(void *target)
{
	register void *targetCap asm("ca0") = target;
	__asm__ volatile("cjalr cra, ca0" : : "C"(targetCap) : "cra", "memory");
}

void *__cheri_compartment("cc_noexec_call") run_fault_noexec_call(void)
{
	test_failures = 0;
	expected_fault_begin();
	void *target = static_cast<void *>(notCode);
	// Sealed is a non-zero otype (CGetType). Not __builtin_cheri_sealed_get: the compiler lowers it to
	// CGetSealed (0x7f/rs2 5), which CHERIoT does not have (CHERIoT-Sail's CGet group is perm, type,
	// base, len, tag, addr, high and top), so the core rightly took an illegal instruction here
	// (mcause 2 at the check, 2026-10-14) and the jump was never reached.
	if (!__builtin_cheri_tag_get(target) || __builtin_cheri_type_get(target) != 0 ||
	    (__builtin_cheri_perms_get(target) & __CHERI_CAP_PERMISSION_PERMIT_EXECUTE__) != 0)
	{
		Debug::log("FAIL: cc_noexec_call: the target {} is not a tagged, unsealed capability "
		           "without Execute",
		           target);
		test_failures++;
		return nullptr;
	}
	jump_through_ca0(target);
	Debug::log("WARNING: no CHERI fault triggered in clang_purecap_noexec_call");
	return nullptr;
}

// A failed check of the target, a fault that is not an execute-permission violation on ca0, or
// no fault.
int __cheri_compartment("cc_noexec_call") run_fault_noexec_call_failures(void)
{
	return expected_fault_failures(test_failures);
}
