// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CWE-190: Integer Overflow leading to Out-of-Bounds Write
//
// This compartment uses a large (wrapping) integer index to access a
// stack-allocated array well beyond its bounds.  On a 32-bit CHERIoT system,
// the address arithmetic wraps around to ptr - 4, outside the 32-byte bounds of
// buf[].
//
// CHERIoT protection: buf[] has precise bounds of 32 bytes (8 × 4).
// ptr[0xFFFFFFFF] computes address = ptr + 0xFFFFFFFF * 4 = ptr + 0xFFFFFFFC
// (wrapping on 32-bit).  That address is below the representable window of the
// small (E = 0) capability, so the cursor move (CIncAddr) clears the tag and the
// store raises a CHERI tag violation (capcause 0x02), not a bounds violation:
// the store never reaches the bounds check. The handler requires the tag
// violation (../common/expected_fault.hh); the unmutated design raises it on
// the store's address register (mtval 0x142, c10, on 2026-10-11). This test
// therefore covers representability (REQ_CPR_04), not the bounds check, and
// cannot detect +cheriot_rtos_mutate=bounds_off; CWE-121/122 do.
//
// Note: volatile on `idx` prevents constant folding that would allow the
// compiler to prove the access is UB and eliminate it.

#include "juliet_cwe.h"
#include <compartment.h>
#include <debug.hh>
#include <new>
#include <stdint.h>

using Debug = ConditionalDebug<true, "CWE-190 Int Overflow OOB">;

#include "../common/expected_fault.hh"

extern "C" ErrorRecoveryBehaviour
compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
{
	return expected_fault_handler(frame, mcause, mtval, CHERI::CauseCode::TagViolation);
}

// 0 only if test_cwe190_int_overflow ended on a tag violation.
int test_cwe190_int_overflow_failures()
{
	return expected_fault_failures(0);
}

void *test_cwe190_int_overflow(int)
{
	expected_fault_begin();
	int buf[8];
	// std::launder gives the compiler no information about buf's bounds,
	// preventing it from optimising away the OOB access.
	int             *ptr = std::launder(buf);
	volatile uint32_t idx = 0xFFFFFFFFU;
	__c11_atomic_signal_fence(__ATOMIC_SEQ_CST);
	// On 32-bit RISC-V: ptr + idx * sizeof(int) = ptr + 0xFFFFFFFC
	// The resulting address is outside buf's 32-byte CHERI bounds.
	// The store must be volatile. As a plain store it was deleted: writing outside buf is
	// undefined behaviour, buf is otherwise dead, and std::launder does not stop dead-store
	// elimination -- the 2026-09-29 image kept only the load of idx, so no out-of-bounds access
	// was ever made and the test "failed" with a correct core. On CHERIoT, ptr - 4 is also below
	// the representable window of a small (E = 0) capability, so the cursor move itself clears
	// the tag and the trap is a tag violation, not a bounds violation (see the header).
	((volatile int *)ptr)[idx] = 0; // CHERI tag violation: ptr - 4 is unrepresentable
	__c11_atomic_signal_fence(__ATOMIC_SEQ_CST);
	return nullptr; // unreachable
}
