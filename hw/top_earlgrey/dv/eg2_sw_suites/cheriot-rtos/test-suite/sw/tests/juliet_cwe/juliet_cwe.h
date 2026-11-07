// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Shared header for the Juliet CWE CHERIoT RTOS compartment tests.
//
// Each Juliet CWE inner compartment intentionally triggers a CHERI hardware
// exception and has a compartment_error_handler that returns ForceUnwind.
// The orchestrator calls each inner compartment and checks that the return
// capability is invalid (is_valid() == false, address() == -1), confirming
// that an exception fired and the compartment unwound correctly. It then calls
// the compartment's test_cweNNN_*_failures(), which is 0 only if that exception
// was the expected one: mcause 0x1c and the CWE's capcause
// (../common/expected_fault.hh). Any CHERI fault ends the call, so without that
// check a broken check that let the access through and faulted later -- or
// faulted for another reason -- would read as a pass.

#pragma once

#include <cdefs.h>
#include <compartment.h>

// Forward declarations for each CWE inner compartment.
// The __cheri_compartment annotation causes cross-compartment call sequences
// to be generated at every call site; the callee itself is a plain function.

/// CWE-121: Stack Buffer Overflow — writes beyond a stack-allocated array.
/// Expects a bounds violation.
__cheri_compartment("cwe121_inner") void *test_cwe121_bof(int);
__cheri_compartment("cwe121_inner") int test_cwe121_bof_failures();

/// CWE-122: Heap Buffer Overflow — writes beyond a heap-allocated buffer.
/// Expects a bounds violation.
__cheri_compartment("cwe122_inner") void *test_cwe122_heap_bof(int);
__cheri_compartment("cwe122_inner") int test_cwe122_heap_bof_failures();

/// CWE-416: Use After Free — dereferences a freed pointer after revocation.
/// Expects a tag violation.
__cheri_compartment("cwe416_inner") void *test_cwe416_uaf(int);
__cheri_compartment("cwe416_inner") int test_cwe416_uaf_failures();

/// CWE-415: Double Free — frees a pointer twice then accesses the freed memory.
/// Expects a tag violation.
__cheri_compartment("cwe415_inner") void *test_cwe415_double_free(int);
__cheri_compartment("cwe415_inner") int test_cwe415_double_free_failures();

/// CWE-190: Integer Overflow → OOB write via a 32-bit wrap-around index.
/// Expects a tag violation: the wrapped cursor is unrepresentable.
__cheri_compartment("cwe190_inner") void *test_cwe190_int_overflow(int);
__cheri_compartment("cwe190_inner") int test_cwe190_int_overflow_failures();
