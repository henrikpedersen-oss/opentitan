/*-
 * Copyright (c) 2015 David Chisnall
 * Copyright (c) 2015 SRI International
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
 * CHERIoT port (lowRISC) of cheri-c-tests/clang-purecap/clang_purecap_va_args.c. The va_arg
 * loop -- ints and tagged capabilities read back through a va_list, compared with the values
 * passed -- is unchanged. Two things differ:
 *
 * 1. The three checks on the va_list capability itself are removed: its length equals the
 *    variadic area, and it has neither Store nor StoreCapability. Those are properties of the
 *    128-bit CHERI-RISC-V purecap ABI, in which the caller passes a bounded, read-only capability
 *    to the variadic area. The CHERIoT ABI passes variadic arguments on the stack, and va_start
 *    is the callee's incoming csp moved by cincoffset, with no csetbounds and no candperm
 *    (llvm-cheriot 17, printstuff in cheri_c_tests.dump: `cincoffset ca?, csp, <frame>` stored
 *    as the va_list). The va_list therefore has the stack capability's bounds and permissions,
 *    so on a correct core the length check fails and both permission checks fail.
 * 2. `char *p = va_arg(ap, void*)` gains a cast. The wrapper compiles this file as C++, which
 *    has no implicit conversion from void * to char *.
 *
 * check_fp() is unchanged; it tests nothing unless INCLUDE_XFAIL is defined, as upstream.
 */
#include <stdarg.h>
#include "cheri_c_test.h"

static char str[] = "012345678901234567890";
static volatile void *ptrs[] =
{
	&str[0],
	&str[1],
	&str[2],
	&str[3],
	&str[4],
	&str[5],
	&str[6],
	&str[7],
	&str[8],
	&str[9],
	&str[0],
	&str[11],
	&str[12],
	&str[13],
	&str[14],
	&str[15],
	&str[16],
	&str[17],
	&str[18],
	&str[19]
};
static int gint;

static void printstuff(int argpairs, ...)
{
	va_list ap;
	va_start(ap, argpairs);
	// CHERIoT: upstream checks the va_list's length and that it lacks STORE and
	// STORE_CAPABILITY here; not applicable (see 1. above).
	for (int i=0 ; i<argpairs ; i++)
	{
		int x = va_arg(ap, int);
		char *p = (char *)va_arg(ap, void*); // CHERIoT: cast for C++ (see 2. above)
		assert_eq(x, i);
		assert(__builtin_cheri_tag_get(p));
		assert_eq_cap(p, __DEVOLATILE(const void*, ptrs[i]));
		assert_eq(*p, str[i]);
	}
	va_end(ap);
}

typedef void (*inc_t)(void);

static void inc(void)
{
	gint++;
}

static void check_fp(int intarg, ...)
{
	inc_t incfp;
	va_list ap;
	va_start(ap, intarg);
	// Check that we've been passed a single function pointer sized argument
#ifdef INCLUDE_XFAIL
	assert_eq(__builtin_cheri_length_get(ap), sizeof(void *));
	incfp = va_arg(ap, inc_t);
	for (int i = 0; i < intarg; i++)
		incfp();
	assert_eq(gint, intarg);
#else
	(void)incfp;
#endif
	va_end(ap);
}

BEGIN_TEST(clang_purecap_va_args)
	printstuff(8, 0,ptrs[0],1,ptrs[1],2,ptrs[2],3,ptrs[3],4,ptrs[4],5,ptrs[5],6,ptrs[6],7,ptrs[7]);
	check_fp(3, &inc);
	assert_eq(faults, 0);
END_TEST
