// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment implementing clang_purecap_output (FAULT test):
//   "read through a capability whose LOAD permission has been removed"
//
// DECLARE_TEST_FAULT.  The original test uses the FreeBSD __cheri_output
// qualifier to strip LOAD/LOAD_CAPABILITY from a pointer and then verifies
// that a load traps.  We replicate the semantic using
// __builtin_cheri_perms_and() to narrow the permissions explicitly.
//
// The attempted load must raise a CHERI load-permission violation (capcause
// 0x12: CHERIoT-Sail checks tag, seal, then Permit_Load before bounds). The
// compartment_error_handler returns ForceUnwind; the orchestrator verifies the
// returned capability is invalid (address == -1) and then asks
// run_fault_output_failures() whether the fault was that one, and whether the
// permission check below passed (../common/expected_fault.hh).

// This wrapper defines its own compartment_error_handler (cheri_c_test_framework.h).
#define CHERI_C_OWN_ERROR_HANDLER
#include <compartment.h>
#include <debug.hh>
#include <stdint.h>
#include "cheriot_perms.h"

using Debug = ConditionalDebug<true, "cc-output">;

#include "../common/expected_fault.hh"

// This wrapper does not include the CHERI-C framework, so it counts its own failed checks.
static int test_failures = 0;

extern "C" ErrorRecoveryBehaviour
compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
{
	return expected_fault_handler(frame, mcause, mtval, CHERI::CauseCode::PermitLoadViolation);
}

// Strip LOAD and LOAD_CAPABILITY from *p, then attempt a load through the
// narrowed capability — this must fault with a CHERI load-permission trap.
__attribute__((noinline)) static volatile int
attempt_load_through_writeonly(int *p)
{
	// Build a write-only (STORE-only) capability from p.
	void *wo = __builtin_cheri_perms_and(
	    (void *)p,
	    __CHERI_CAP_PERMISSION_PERMIT_STORE__ |
	    __CHERI_CAP_PERMISSION_PERMIT_STORE_CAPABILITY__);

	// Check the permission really was removed (was a no-op __builtin_expect; see
	// test_clang_purecap_input.cc). If CAndPerm left Load in place, report that and skip the load.
	if ((__builtin_cheri_perms_get(wo) & __CHERI_CAP_PERMISSION_PERMIT_LOAD__) != 0)
	{
		Debug::log("FAIL: cc_output: CAndPerm left Load in place: perms {}", __builtin_cheri_perms_get(wo));
		test_failures++;
		return 0;
	}

	// This load must trigger a CHERI load-permission fault.
	return *(volatile int *)wo;
}

void *__cheri_compartment("cc_output") run_fault_output(void)
{
	test_failures = 0;
	expected_fault_begin();
	int x = 47;
	(void)attempt_load_through_writeonly(&x);
	Debug::log("WARNING: no CHERI fault triggered in cc_output test");
	return nullptr;
}

// A failed permission check, a fault that is not a load-permission violation, or no fault.
int __cheri_compartment("cc_output") run_fault_output_failures(void)
{
	return expected_fault_failures(test_failures);
}
