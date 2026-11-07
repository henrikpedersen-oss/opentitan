/*-
 * Copyright (c) 2017 Alex Richardson
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
 * CHERIoT port (lowRISC) of cheri-c-tests/clang-purecap/clang_purecap_byval_args.c. One line is
 * added: a compiler barrier after the callee's memset. Everything else is unchanged.
 *
 * Built from the upstream file, the test does nothing at run time. foo_byval() only writes its own
 * copy of the argument, which is dead once it returns, so the optimiser removed the memset, then
 * the call and the by-value copy with it; run_test_byval_args() was reduced to the caller's
 * memset of global_foo followed by `li a0, 0` (llvm-cheriot 17, cheri_c_tests.dump) -- no copy,
 * no callee, and both assertions folded away.
 *
 * The barrier takes f.data as an input and clobbers memory, so the compiler must assume the
 * callee's bytes are read and that any memory, global_foo included, may have changed. It must
 * therefore perform the memset, make the call, give the callee a real copy, and reload
 * global_foo.data[0] afterwards. On CHERIoT the copy is on the caller's stack and the callee gets
 * a capability bounded to it, 1024 bytes (see run_test_clang_purecap_byval_args and foo_byval in
 * the dump; the compiler builds the copy with a memset of zeros, knowing global_foo is all zeros
 * there), so a callee memset that reached global_foo, or a copy that was not made, fails the
 * second assertion. The first assertion is still folded, as it is only about the caller's memset.
 */
#include "cheri_c_test.h"
#include <stdlib.h>
#include <string.h>

struct foo {
	char data[1024];
};

struct foo global_foo;

__attribute__((noinline)) static void
foo_byval(struct foo f)
{
	memset(f.data, 42, sizeof(f.data));
	// CHERIoT: keep the memset, the call and the copy (see above).
	__asm__ volatile("" : : "C"(f.data) : "memory");
}

BEGIN_TEST(clang_purecap_byval_args)
	/* Check that structs passed by value are actually copied */
	memset(global_foo.data, 0, sizeof(global_foo.data));
	assert_eq(global_foo.data[0], 0);
	foo_byval(global_foo);
	assert_eq(global_foo.data[0], 0);
END_TEST
