// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Capability-width atomics for clang_purecap_atomic (cc_atomic): the library calls clang makes
// for C11 atomics on a capability-typed object (_Atomic(int*)): __atomic_load_cap,
// __atomic_store_cap, __atomic_exchange_cap and __atomic_compare_exchange_cap. CHERIoT RTOS has
// none of them (sdk/lib/atomic has 1, 2, 4 and 8 bytes only), so the firmware did not link
// ("undefined symbol: __library_export_libcalls___atomic_store_cap") and the test was left out.
//
// They are written as the RTOS writes its own (sdk/lib/atomic/atomic.hh): a library of plain
// loads and stores run with interrupts disabled, which is atomic on a single core with no other
// bus master writing the object. The capability is moved whole (clc / csc), so its tag goes with
// it. Compare-and-exchange compares the whole capability, tag and metadata included
// (__builtin_cheri_equal_exact), not only its address. The test then checks the core and the
// compiler on C11 atomics over capabilities, through this library; a fix in the RTOS would replace
// it.

#include <cdefs.h>
#include <stdint.h>

#define DECLARE_ATOMIC_CAP_LIBCALL(name, ret, ...)                                               \
	[[cheriot::interrupt_state(disabled)]] CHERIOT_DECLARE_STANDARD_LIBCALL(name, ret, __VA_ARGS__)

DECLARE_ATOMIC_CAP_LIBCALL(__atomic_load_cap, void *, void *const *, int);
void *__atomic_load_cap(void *const *ptr, int)
{
	return *ptr;
}

DECLARE_ATOMIC_CAP_LIBCALL(__atomic_store_cap, void, void **, void *, int);
void __atomic_store_cap(void **ptr, void *value, int)
{
	*ptr = value;
}

DECLARE_ATOMIC_CAP_LIBCALL(__atomic_exchange_cap, void *, void **, void *, int);
void *__atomic_exchange_cap(void **ptr, void *value, int)
{
	void *old = *ptr;
	*ptr      = value;
	return old;
}

DECLARE_ATOMIC_CAP_LIBCALL(__atomic_compare_exchange_cap, int, void **, void **, void *, int, int);
int __atomic_compare_exchange_cap(void **ptr, void **expected, void *desired, int, int)
{
	if (__builtin_cheri_equal_exact(*ptr, *expected))
	{
		*ptr = desired;
		return 1;
	}
	*expected = *ptr;
	return 0;
}
