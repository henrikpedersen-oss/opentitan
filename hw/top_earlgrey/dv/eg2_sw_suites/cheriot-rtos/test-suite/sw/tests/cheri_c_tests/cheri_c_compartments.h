// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Forward declarations for all CHERI-C test compartments.
//
// Normal tests return int: 0 = PASS, >0 = number of assertion failures.
// Fault tests return void*: expected to ForceUnwind (return is an untagged
// capability with address() == -1), confirming a CHERI exception fired. Each
// also has a run_fault_NAME_failures() export, called after it: 0 only if the
// fault was the expected one (capcause, and for noexec_call the register) and
// no check before it failed (../common/expected_fault.hh).

#pragma once

#include <cdefs.h>
#include <compartment.h>

// ── Normal tests (DECLARE_TEST) ─────────────────────────────────────────────

/// Global pointer initialisation and function pointer capability checks.
__cheri_compartment("cc_init")         int run_test_init(void);

/// Pointer comparison and derived capability bounds.
__cheri_compartment("cc_capcmp")       int run_test_capcmp(void);

/// C11 _Atomic types with pointer operands.
__cheri_compartment("cc_atomic")       int run_test_atomic(void);

/// Return address is a valid, executable capability.
__cheri_compartment("cc_capretaddr")   int run_test_capretaddr(void);

/// Function pointer size and callable via capability.
__cheri_compartment("cc_funptr")       int run_test_funptr(void);

/// __intcap_t initialisation, arithmetic, comparison.
__cheri_compartment("cc_intcap")       int run_test_intcap(void);

/// NULL pointer representation in purecap.
__cheri_compartment("cc_null")         int run_test_null(void);

/// Small-integer tagging in low bits of an aligned pointer.
__cheri_compartment("cc_smallint")     int run_test_smallint(void);

/// Unions containing capabilities.
__cheri_compartment("cc_union")        int run_test_union(void);

/// Union-of-structs with capability fields passed by value.
__cheri_compartment("cc_union_struct") int run_test_union_struct(void);

/// Structs passed by value are copied: the callee cannot write the caller's object.
__cheri_compartment("cc_byval_args")   int run_test_byval_args(void);

/// Arithmetic, bitwise and comparison operators on __intcap_t.
__cheri_compartment("cc_intcapmath")   int run_test_intcapmath(void);

/// Arithmetic, bitwise and comparison operators on __uintcap_t.
__cheri_compartment("cc_uintcapmath")  int run_test_uintcapmath(void);

/// Ints and tagged capabilities read back through va_arg.
__cheri_compartment("cc_va_args")      int run_test_va_args(void);

/// va_copy gives a va_list that reads the same arguments.
__cheri_compartment("cc_va_copy")      int run_test_va_copy(void);

/// Return through the capability return register (cjr cra) from assembly.
__cheri_compartment("cc_capret")       int run_test_capret(void);

/// On-stack arrays get non-overlapping bounds at least as long as the array.
__cheri_compartment("cc_stack_cap")    int run_test_stack_cap(void);

/// The (u)intcapmath operators on plain 64-bit integers (the control for the capability versions).
__cheri_compartment("cc_int64math")    int run_test_int64math(void);
__cheri_compartment("cc_uint64math")   int run_test_uint64math(void);

// ── libc tests (cheri-c-tests libc/, DECLARE_TEST) ──────────────────────────
// Entry points run_libc_<test>: upstream's BEGIN_TEST(libc_<test>) takes run_test_libc_<test>.

/// sprintf writes the formatted string and returns its length.
__cheri_compartment("cc_libc_printf")  int run_libc_printf(void);

/// malloc and calloc: tagged, bounded, aligned, zeroed, with load and store permissions.
__cheri_compartment("cc_libc_malloc")  int run_libc_malloc(void);

/// memcpy keeps an aligned capability's tag, clears a misaligned one's, and copies the data.
__cheri_compartment("cc_libc_memcpy")  int run_libc_memcpy(void);

/// memmove of overlapping capabilities keeps them valid.
__cheri_compartment("cc_libc_memmove") int run_libc_memmove(void);

/// longjmp returns to setjmp from a nested call.
__cheri_compartment("cc_libc_setjmp")  int run_libc_setjmp(void);

// ── Fault tests (DECLARE_TEST_FAULT) ────────────────────────────────────────
// These compartments deliberately trigger CHERI hardware exceptions.
// A correct run ForceUnwinds; the orchestrator verifies the return is invalid,
// then calls the matching *_failures() export.

/// On-stack array accessed out-of-bounds → CHERI bounds violation.
__cheri_compartment("cc_array")        void *run_fault_array(void);
__cheri_compartment("cc_array")        int run_fault_array_failures(void);

/// Call through an integer-derived (untagged) function pointer → CHERI tag violation.
__cheri_compartment("cc_badcall")      void *run_fault_badcall(void);
__cheri_compartment("cc_badcall")      int run_fault_badcall_failures(void);

/// Jump through a tagged, unsealed data capability (no Execute) in ca0 →
/// CHERI execute-permission violation on ca0.
__cheri_compartment("cc_noexec_call")  void *run_fault_noexec_call(void);
__cheri_compartment("cc_noexec_call")  int run_fault_noexec_call_failures(void);

/// Write through a read-only (load-only) capability → CHERI store-permission violation.
__cheri_compartment("cc_input")        void *run_fault_input(void);
__cheri_compartment("cc_input")        int run_fault_input_failures(void);

/// Read through a write-only (store-only) capability → CHERI load-permission violation.
__cheri_compartment("cc_output")       void *run_fault_output(void);
__cheri_compartment("cc_output")       int run_fault_output_failures(void);

/// va_list stored in a global loses its tag (local capability) → tag violation on va_arg.
__cheri_compartment("cc_va_list_global") void *run_fault_va_list_global(void);
__cheri_compartment("cc_va_list_global") int run_fault_va_list_global_failures(void);

// ── Orchestrator ─────────────────────────────────────────────────────────────
[[noreturn]] __cheri_compartment("cc_orchestrator") void run_cheri_c_tests(void);
