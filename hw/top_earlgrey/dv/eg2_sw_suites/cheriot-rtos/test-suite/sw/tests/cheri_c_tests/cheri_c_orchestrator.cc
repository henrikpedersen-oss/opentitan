// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Cambridge CHERI C Test Suite — CHERIoT RTOS Orchestrator
//
// Calls each test compartment in sequence and reports pass/fail.
//
// Normal tests  (DECLARE_TEST): compartment returns int; 0 == PASS.
// Fault tests (DECLARE_TEST_FAULT): compartment ForceUnwinds; the switcher
//   returns an untagged capability with address() == -1. A pass also needs
//   the compartment's run_fault_NAME_failures() to return 0: the fault was the
//   expected one and no check before it failed (../common/expected_fault.hh).
//
// Output matches test_runner.py convention:
//   "All tests finished" → PASS
//   "Test(s) Failed"     → FAIL

#include "../common/sim_exit.hh"
#include "cheri_c_compartments.h"
#include <cheri.hh>
#include <compartment.h>
#include <debug.hh>
#include <stdint.h>
#include <thread.h>

using namespace CHERI;
using Debug = ConditionalDebug<true, "CHERI-C tests">;

// ── helpers ─────────────────────────────────────────────────────────────────

static bool check_normal(const char *name, int ret)
{
	if (ret == 0)
	{
		Debug::log("PASS: {}", name);
		return true;
	}
	Debug::log("FAIL: {} — {} assertion(s) failed", name, ret);
	return false;
}

static bool check_fault(const char *name, Capability<void> ret)
{
	if (!ret.is_valid() &&
	    ret.address() == static_cast<ptraddr_t>(-1UL))
	{
		Debug::log("PASS: {} — CHERI fault fired, ForceUnwind confirmed", name);
		return true;
	}
	if (ret.is_valid())
	{
		Debug::log("FAIL: {} — returned a valid capability; expected CHERI fault",
		           name);
	}
	else
	{
		Debug::log("FAIL: {} — ForceUnwind but address is {} (expected -1)",
		           name,
		           ret.address());
	}
	return false;
}

// A fault test: its compartment also counts the checks it makes before and in the fault
// (assertions that ran before it, and the handler's check of the fault's cause). Those are
// lost in the unwind otherwise, so a fault of the wrong kind, or a failed assertion before
// the right one, would still pass check_fault() -- as clang_purecap_array did with the bounds
// check broken, faulting on its overwritten return capability instead. One verdict line either
// way.
static bool check_fault_and_checks(const char *name, Capability<void> ret, int failures)
{
	if (failures != 0)
	{
		Debug::log("FAIL: {} — {} check(s) failed before or in the fault", name, failures);
		return false;
	}
	return check_fault(name, ret);
}

// ── thread entry point ───────────────────────────────────────────────────────

[[noreturn]] void __cheri_compartment("cc_orchestrator") run_cheri_c_tests()
{
	bool allPassed = true;
	int  passed    = 0;
	int  total     = 0;

	Debug::log("=== Cambridge CHERI-C Test Suite (CHERIoT RTOS) ===");

	// ── Normal tests ────────────────────────────────────────────────────────

	Debug::log("-- clang_purecap_init: global pointer initialisation --");
	{
		bool ok = check_normal("clang_purecap_init", run_test_init());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_capcmp: pointer comparison --");
	{
		bool ok = check_normal("clang_purecap_capcmp", run_test_capcmp());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	// clang_purecap_atomic applies C11 atomics to _Atomic(int*), a capability-typed object, so
	// clang emits __atomic_store_cap / __atomic_exchange_cap / __atomic_compare_exchange_cap
	// libcalls. CHERIoT RTOS ships atomic1/2/4/8 plus atomicn and implements none of the
	// capability-width variants, so the firmware link failed with
	//   undefined symbol: __library_export_libcalls___atomic_store_cap
	// and the test was left out. It runs now against atomic_cap.cc, a library of the four in the
	// RTOS's own style (interrupts disabled), in this directory.
	Debug::log("-- clang_purecap_atomic: C11 _Atomic types --");
	{
		bool ok = check_normal("clang_purecap_atomic", run_test_atomic());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_capretaddr: return address is a capability --");
	{
		bool ok = check_normal("clang_purecap_capretaddr", run_test_capretaddr());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_funptr: function pointers --");
	{
		bool ok = check_normal("clang_purecap_funptr", run_test_funptr());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_intcap: __intcap_t arithmetic --");
	{
		bool ok = check_normal("clang_purecap_intcap", run_test_intcap());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_null: NULL pointer representation --");
	{
		bool ok = check_normal("clang_purecap_null", run_test_null());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_smallint: small-integer in low pointer bits --");
	{
		bool ok = check_normal("clang_purecap_smallint", run_test_smallint());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_union: capability tag cleared by integer write --");
	{
		bool ok = check_normal("clang_purecap_union", run_test_union());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_union_struct: union-of-structs with caps --");
	{
		bool ok = check_normal("clang_purecap_union_struct", run_test_union_struct());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_byval_args: structs passed by value are copied --");
	{
		bool ok = check_normal("clang_purecap_byval_args", run_test_byval_args());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_intcapmath: operators on __intcap_t --");
	{
		bool ok = check_normal("clang_purecap_intcapmath", run_test_intcapmath());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_uintcapmath: operators on __uintcap_t --");
	{
		bool ok = check_normal("clang_purecap_uintcapmath", run_test_uintcapmath());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_va_args: ints and capabilities through va_arg --");
	{
		bool ok = check_normal("clang_purecap_va_args", run_test_va_args());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_va_copy: va_copy reads the same arguments --");
	{
		bool ok = check_normal("clang_purecap_va_copy", run_test_va_copy());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	// clang_purecap_va_die is not ported. It reads a ninth int from eight variadic arguments and
	// needs that va_arg to fault on the va_list's bounds. The CHERIoT ABI passes variadic
	// arguments on the stack and va_start is csp plus an offset, with no csetbounds, so the
	// va_list has the stack capability's bounds and the extra read lands in the caller's frame.
	// There is no bound for the core to enforce: a correct core does not fault, and the test would
	// check only what happens to be on the stack. It has no compartment (see xmake.lua).

	Debug::log("-- clang_purecap_capret: return through the capability return register --");
	{
		bool ok = check_normal("clang_purecap_capret", run_test_capret());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_stack_cap: on-stack arrays get disjoint, sufficient bounds --");
	{
		bool ok = check_normal("clang_purecap_stack_cap", run_test_stack_cap());
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	// The (u)intcapmath operators on plain 64-bit integers, and the libc tests (cheri-c-tests
	// libc/). Not run, from libc/: libc_qsort (CHERIoT RTOS has no qsort) and libc_string (it
	// calls strcpy, which the RTOS's string library does not have: strncpy and strlcpy only).
	// Each a compartment call of its own (a cross-compartment entry is called by name, through
	// the import table, not through a plain function pointer).
#define CHERI_C_NORMAL(name, what, call)                                                       \
	do                                                                                         \
	{                                                                                          \
		Debug::log("-- " name ": " what " --");                                               \
		bool ok = check_normal(name, call);                                                    \
		allPassed &= ok;                                                                       \
		if (ok) passed++;                                                                      \
		total++;                                                                               \
	} while (0)
	CHERI_C_NORMAL("clang_purecap_int64math", "operators on int64_t", run_test_int64math());
	CHERI_C_NORMAL("clang_purecap_uint64math", "operators on uint64_t", run_test_uint64math());
	CHERI_C_NORMAL("libc_printf", "sprintf", run_libc_printf());
	CHERI_C_NORMAL("libc_malloc", "malloc and calloc", run_libc_malloc());
	CHERI_C_NORMAL("libc_memcpy", "memcpy keeps aligned capabilities", run_libc_memcpy());
	CHERI_C_NORMAL("libc_memmove", "memmove of overlapping capabilities", run_libc_memmove());
	CHERI_C_NORMAL("libc_setjmp", "setjmp and longjmp", run_libc_setjmp());
#undef CHERI_C_NORMAL

	// ── Fault tests ─────────────────────────────────────────────────────────

	// Each compartment call below is made before its *_failures() call: the second reads what
	// the first left in the compartment's globals.

	Debug::log("-- clang_purecap_array: OOB array → CHERI bounds violation --");
	{
		Capability<void> ret      = run_fault_array();
		int              failures = run_fault_array_failures();
		bool ok = check_fault_and_checks("clang_purecap_array", ret, failures);
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_badcall: call through an integer-derived pointer → CHERI tag violation --");
	{
		Capability<void> ret      = run_fault_badcall();
		int              failures = run_fault_badcall_failures();
		bool ok = check_fault_and_checks("clang_purecap_badcall", ret, failures);
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_noexec_call: jump through a data cap → CHERI execute-permission violation --");
	{
		Capability<void> ret      = run_fault_noexec_call();
		int              failures = run_fault_noexec_call_failures();
		bool ok = check_fault_and_checks("clang_purecap_noexec_call", ret, failures);
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_input: write through read-only cap → store-permission violation --");
	{
		Capability<void> ret      = run_fault_input();
		int              failures = run_fault_input_failures();
		bool ok = check_fault_and_checks("clang_purecap_input", ret, failures);
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_output: read through write-only cap → load-permission violation --");
	{
		Capability<void> ret      = run_fault_output();
		int              failures = run_fault_output_failures();
		bool ok = check_fault_and_checks("clang_purecap_output", ret, failures);
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	Debug::log("-- clang_purecap_va_list_global: va_list stored in a global → tag fault --");
	{
		Capability<void> ret      = run_fault_va_list_global();
		int              failures = run_fault_va_list_global_failures();
		bool ok = check_fault_and_checks("clang_purecap_va_list_global", ret, failures);
		allPassed &= ok;
		if (ok) passed++;
		total++;
	}

	// ── Summary ─────────────────────────────────────────────────────────────

	Debug::log("=== Summary: {} / {} tests passed ===", passed, total);
	if (allPassed)
	{
		Debug::log("All tests finished");
	}
	else
	{
		Debug::log("Test(s) Failed");
	}
	// End the run in simulation (common/sim_exit.hh); without it the loop below idles until
	// the testbench's MAX_CYCLES.
	sim_exit();

	// CHERIoT RTOS threads must not return.
	while (true)
	{
		Timeout t{100};
		thread_sleep(&t);
	}
}
