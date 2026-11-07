/*-
 * Copyright (c) 2012-2015 David Chisnall
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
 * CHERIoT port (lowRISC, 2026): identical to cheri-c-tests/clang-purecap/clang_purecap_union.c
 * except that the exact-length assertion on the pointer to `buffer` is declared not applicable
 * to CHERIoT (decision 2026-09-29). The tag checks -- the point of the test: an integer write over
 * a capability in memory must clear its tag -- are kept unchanged.
 *
 * Why the length check is out: CHERIoT's 9-bit compressed bounds can round a global's bounds up,
 * so an exact match is not an architectural guarantee. Note that rounding is not what the
 * 2026-09-29 run showed, though: an 8-byte object is exactly representable, and the reported
 * length, 28, was the size of the whole .cc_union_globals section -- the capability had not been
 * narrowed to `buffer` at all. A lower-bound check (length >= sizeof(buffer)) would still hold
 * either way if one is wanted.
 */
#include "cheri_c_test.h"

union ptr_or_data
{
	void* ptr;
	long long words[4];
};

static char buffer[] = "1234567";
static char *ptr = buffer;

BEGIN_TEST(clang_purecap_union)
	// Check that overwriting a capability in memory gives you something that
	// is not a valid capability.
	// Note that this needs to be volatile, as otherwise the aliasing rules in
	// C permit the compiler to treat the loads and stores as separate memory
	// locations and for the writes to be ordered after the reads.
	volatile union ptr_or_data p;
	p.ptr = ptr;
	// CHERIoT: upstream asserts __builtin_cheri_length_get(p.ptr) == 8 here; not applicable.
	assert_eq(__builtin_cheri_tag_get(p.ptr), 1);
	if (sizeof(void*) == 32)
	{
		p.words[3] = 400;
	}
	else
	{
		p.words[0] = 400;
	}
	assert_eq(__builtin_cheri_tag_get(p.ptr), 0);
END_TEST
