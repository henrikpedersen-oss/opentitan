// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CWE-121: Stack Buffer Overflow
//
// This compartment deliberately writes past the end of a 16-element
// stack-allocated array.  CHERIoT hardware detects the bounds violation and
// raises a CHERI exception.  The compartment_error_handler catches it and
// returns ForceUnwind, so the caller receives an invalid return capability.
//
// CHERIoT protection: a stack object whose address escapes gets a capability
// narrowed to its own bounds.  Writing at byte offset 128 of a 64-byte buffer is
// outside those bounds → CHERI exception (mcause = 0x1c, capcause 0x01 bounds
// violation), which the handler requires (../common/expected_fault.hh).  See
// write_past_end() below for why the store lives in a separate function; putting
// it inline with the array lets the optimiser delete the test.

#include "juliet_cwe.h"
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "CWE-121 Stack BOF">;

#include "../common/expected_fault.hh"

extern "C" ErrorRecoveryBehaviour
compartment_error_handler(ErrorState *frame, size_t mcause, size_t mtval)
{
	return expected_fault_handler(frame, mcause, mtval, CHERI::CauseCode::BoundsViolation);
}

// Writes past the end of a caller-supplied buffer. Deliberately a separate,
// non-inlinable function -- that separation is what makes this test work at -O2.
//
// The store used to live directly in test_cwe121_bof() alongside the array. There
// the optimiser could see that buf never escapes and that nothing ever reads the
// stored value, so it removed the whole body: the function compiled to three
// instructions (cmove ca0, cnull / li a1, 0 / cret). No capability was ever
// dereferenced, so no exception could fire, and the test returned its
// "unreachable" nullptr -- indistinguishable in the log from CHERIoT failing to
// catch a stack overflow.
//
// std::launder and __c11_atomic_signal_fence did not prevent that, and were
// removed as misleading: neither makes the object escape, and a signal fence only
// orders accesses that are emitted in the first place.
//
// Passing the array to a noinline callee addresses both halves:
//   - the address escapes, so CHERIoT clang must materialise a real capability
//     for buf, narrowed to the 64-byte object rather than left as a csp offset.
//     Without that narrowing the access would still be inside the stack bounds
//     and would not fault.
//   - p is opaque here, so the store cannot be proven dead.
// volatile is belt-and-braces against dead-store elimination.
__attribute__((noinline)) static void
write_past_end(int *p)
{
	// Byte offset 128, outside the 64-byte bounds of the caller's buffer.
	static_cast<volatile int *>(p)[32] = 0;
}

void *test_cwe121_bof(int)
{
	expected_fault_begin();
	int buf[16]; // 64 bytes
	write_past_end(buf);
	return nullptr; // unreachable: the bounds exception fires in write_past_end
}

// 0 only if test_cwe121_bof ended on a bounds violation.
int test_cwe121_bof_failures()
{
	return expected_fault_failures(0);
}
