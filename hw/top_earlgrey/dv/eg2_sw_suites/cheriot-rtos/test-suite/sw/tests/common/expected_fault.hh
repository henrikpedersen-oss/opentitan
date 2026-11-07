// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// The expected-fault check of the fault tests in the CHERI-C and Juliet CWE suites.
//
// A fault test's compartment makes one access the architecture must stop with a CHERI exception.
// Its compartment_error_handler returns ForceUnwind, and the caller gets the switcher's unwind
// sentinel (an untagged capability with address -1). The sentinel only says that *some* fault
// ended the call, and a broken check can still end it with one: with the bounds check forced off
// (+cheriot_rtos_mutate=bounds_off), clang_purecap_array wrote past its buffer over its own return
// capability, failed its assertion, and then faulted on the return -- a tag violation on cra --
// and was reported as a pass.
//
// So a fault test passes only if
//   - the fault that ends it is the expected one: mcause 0x1c (CHERI) and the expected capcause
//     (mtval[4:0]); where the test chooses the register (inline assembly), also that register
//     (mtval[10:5]). The register of compiled C is the compiler's choice and is not checked.
//   - the expected fault fired at all, and
//   - no check the test itself makes failed (assertions before the fault).
// The handler records the result in compartment globals, which survive the unwind, and a second
// export of the compartment returns it to the orchestrator after the fault test
// (expected_fault_failures()). A fault of another kind is logged with both causes.
//
// Include after `using Debug = ConditionalDebug<...>`; one fault test per compartment:
//
//   extern "C" ErrorRecoveryBehaviour
//   compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
//   {
//   	return expected_fault_handler(frame, mcause, mtval, CHERI::CauseCode::BoundsViolation);
//   }
//   void *run_fault_x() { expected_fault_begin(); <the access>; return nullptr; }
//   int run_fault_x_failures() { return expected_fault_failures(<the test's own failures>); }

#pragma once

#include <cheri.hh>
#include <compartment.h>
#include <priv/riscv.h>

namespace
{
	/// The expected fault has fired.
	bool expectedFaultSeen = false;
	/// Faults that were not the expected one (at most one: any fault ends the call).
	int unexpectedFaults = 0;

	/// Call at the start of the fault test, before the access.
	[[maybe_unused]] void expected_fault_begin()
	{
		expectedFaultSeen = false;
		unexpectedFaults  = 0;
	}

	ErrorRecoveryBehaviour expected_fault_check(ErrorState            *frame,
	                                            size_t                 mcause,
	                                            size_t                 mtval,
	                                            CHERI::CauseCode       expectedCause,
	                                            bool                   checkRegister,
	                                            CHERI::RegisterNumber  expectedRegister)
	{
		auto [cause, reg] = CHERI::extract_cheri_mtval(mtval);
		Debug::log("CHERI fault caught: mcause={} mtval={} (cause {}, register {}) pcc={}: "
		           "ForceUnwind",
		           mcause,
		           mtval,
		           cause,
		           reg,
		           frame->pcc);
		if (mcause == priv::MCAUSE_CHERI && cause == expectedCause &&
		    (!checkRegister || reg == expectedRegister))
		{
			expectedFaultSeen = true;
		}
		else if (checkRegister)
		{
			Debug::log("FAIL: expected a CHERI {} on register {}, got mcause={} cause {} on "
			           "register {}",
			           expectedCause,
			           expectedRegister,
			           mcause,
			           cause,
			           reg);
			unexpectedFaults++;
		}
		else
		{
			Debug::log("FAIL: expected a CHERI {}, got mcause={} cause {} (register {})",
			           expectedCause,
			           mcause,
			           cause,
			           reg);
			unexpectedFaults++;
		}
		return ErrorRecoveryBehaviour::ForceUnwind;
	}

	/// The handler of a test that expects capcause `expectedCause` on any register.
	[[maybe_unused]] ErrorRecoveryBehaviour
	expected_fault_handler(ErrorState      *frame,
	                       size_t           mcause,
	                       size_t           mtval,
	                       CHERI::CauseCode expectedCause)
	{
		return expected_fault_check(
		  frame, mcause, mtval, expectedCause, false, CHERI::RegisterNumber::Invalid);
	}

	/// The handler of a test that expects capcause `expectedCause` on register
	/// `expectedRegister`.
	[[maybe_unused]] ErrorRecoveryBehaviour
	expected_fault_handler(ErrorState           *frame,
	                       size_t                mcause,
	                       size_t                mtval,
	                       CHERI::CauseCode      expectedCause,
	                       CHERI::RegisterNumber expectedRegister)
	{
		return expected_fault_check(
		  frame, mcause, mtval, expectedCause, true, expectedRegister);
	}

	/// The fault test's failures, for the orchestrator: `ownFailures` (the checks the test made
	/// itself, e.g. assertions before the fault), a fault that was not the expected one, and the
	/// expected fault not firing at all. 0 means the test passed.
	[[maybe_unused]] int expected_fault_failures(int ownFailures)
	{
		int failures = ownFailures + unexpectedFaults;
		if (!expectedFaultSeen && unexpectedFaults == 0)
		{
			Debug::log("FAIL: the expected CHERI fault never fired");
			failures++;
		}
		return failures;
	}
} // namespace
