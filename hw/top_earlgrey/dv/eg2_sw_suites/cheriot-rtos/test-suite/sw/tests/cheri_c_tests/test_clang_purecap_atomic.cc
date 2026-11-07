// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT compartment wrapping clang_purecap_atomic:
//   "C11 _Atomic types including _Atomic(int*) with pointer operands"
//
// DECLARE_TEST — returns 0 on PASS.
//
// Note: clang_purecap_atomic.c includes <stdlib.h> for NULL/size_t; both
// are available in the CHERIoT freestanding environment.

#define TEST_CUSTOM_FRAMEWORK
#include <compartment.h>
#include <debug.hh>

using Debug = ConditionalDebug<true, "cc-atomic">;

// clang_purecap_atomic.c stores into `_Atomic(int*) p` through a (void*) cast at
// lines 80, 82 and 85. C converts void* to int* implicitly; C++ does not, and this
// wrapper compiles the file as C++:
//
//   error: cannot initialize a parameter of type 'int *' with an rvalue of type 'void *'
//
// The cast is applied here rather than in cheri-c-tests, which is a clean CTSRD
// submodule -- an edit there would be reverted by the next submodule update with no
// sign it had gone, the same way the crt_cheriot.S local patch is exposed.
//
// A function-like macro is not re-expanded within its own expansion, so naming the
// builtin on both sides is well defined. In this file all three builtins are applied
// only to &p, so casting unconditionally to int* cannot disturb the char/short/int/
// long long atomics alongside it.
#define __c11_atomic_compare_exchange_strong(obj, exp, des, succ, fail)                \
	__c11_atomic_compare_exchange_strong(obj, exp, (int *)(des), succ, fail)
#define __c11_atomic_exchange(obj, des, ord)                                           \
	__c11_atomic_exchange(obj, (int *)(des), ord)
#define __c11_atomic_store(obj, des, ord) __c11_atomic_store(obj, (int *)(des), ord)

#include "clang-purecap/clang_purecap_atomic.c"

int __cheri_compartment("cc_atomic") run_test_atomic(void)
{
	return run_test_clang_purecap_atomic();
}
