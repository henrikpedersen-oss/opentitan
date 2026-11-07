/*-
 * Copyright (c) 2015 David Chisnall
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract (FA8750-10-C-0237)
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * CHERIoT port (lowRISC) of cheri-c-tests/clang-purecap/clang_purecap_stack_cap.c. Only the
 * three array sizes differ; the checks are unchanged.
 *
 * Upstream allocates 128 KiB, 256 KiB and 1 MiB on the stack. A CHERIoT RTOS thread stack is at
 * most 8176 bytes (cheriot-rtos/sdk/xmake.lua stack_size_limit: "truncating them in compartment
 * switch may encounter precision errors"), and a compartment call runs on its caller's thread
 * stack, so the three arrays must fit in well under 8 KiB together.
 *
 * The sizes here are 513, 1025 and 4097: roughly upstream's 1:2:8, and each one byte past a
 * power of two, so no length is exactly representable and each needs the padding upstream's
 * check is about ("even with alignment padding, none of the stack allocations overlap").
 * CHERIoT bounds have a
 * 9-bit mantissa (cheriot-sail cheri_cap_common.sail cap_mantissa_width), so a length over
 * 511 << (E-1) takes exponent E, and base and length become multiples of 2^E: 513 needs E = 1
 * and is padded to 514, 1025 needs E = 2 (1028), 4097 needs E = 4 (4112). llvm-cheriot 17
 * requests exactly those padded lengths in its csetbounds and places the next array directly
 * after the padding (do_test in cheri_c_tests.dump), so a core that rounded the bounds further
 * out would make two arrays overlap, and one that rounded them in would fail check_sizes().
 *
 * ptrs[] is a global, reached through cgp, which lacks StoreLocal, so storing a stack pointer
 * there clears its tag (cheriot-sail cheri_insts.sail CSC). check_sizes() only reads the length,
 * which CGetLen decodes from the bounds whether or not the tag is set, so the check is unaffected
 * and is left as upstream wrote it.
 */
#include "cheri_c_test.h"
extern const unsigned int sizes[];
const unsigned int sizes[] = {
	513, 1025, 4097 // CHERIoT: upstream has 131072, 262144, 1048576 (see above)
};
volatile void* ptrs[3];

__noinline static void check_overlap(void *a, void*b)
{
	unsigned long long basea = __builtin_cheri_base_get(a);
	unsigned long long baseb = __builtin_cheri_base_get(b);
	unsigned long long topa = basea + __builtin_cheri_length_get(a);
	unsigned long long topb = baseb + __builtin_cheri_length_get(b);
	assert((basea >= topb) || (baseb >= topa));
}

__noinline static void check_sizes(void)
{
	for (unsigned int i=0 ; i<sizeof(sizes)/sizeof(sizes[0]) ; i++)
	{
		assert(__builtin_cheri_length_get(__DEVOLATILE(void*, ptrs[i])) >= sizes[i]);
	}
}

__noinline static void do_test(void) {
	char foo[sizes[0]], bar[sizes[1]], baz[sizes[2]];
	ptrs[0] = foo;
	ptrs[1] = bar;
	ptrs[2] = baz;
	// Check that, even with alignment padding, none of the stack allocations overlap
	check_overlap(foo, bar);
	check_overlap(bar, baz);
	check_overlap(foo, baz);
	// Check that we have as much space as we asked for.
	check_sizes();
}

BEGIN_TEST(clang_purecap_stack_cap)
	do_test();
END_TEST
