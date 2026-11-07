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
 * CHERIoT port (lowRISC) of cheri-c-tests/libc/libc_setjmp.c. The one change: the jmp_buf is on
 * the stack of the test's body and passed down, not a static. CHERIoT's setjmp saves the stack
 * capability (sdk/include/setjmp.h: "CHERIoT cannot store a jmp_buf anywhere other than the stack
 * without clearing tags"): a stack capability is local, a store of it to a global loses its tag,
 * and longjmp from such a buffer faults. The sequence the test checks (setjmp returns 0, the
 * nested function's longjmp returns 2 to it, positions 0 to 4) is unchanged.
 */
#include "cheri_c_test.h"
#include <setjmp.h>

static int position;

static void dolongjump(jmp_buf buf)
{
	assert_eq(position, 3);
	position++;
	longjmp(buf, 2);
}

static void dosetjump(jmp_buf buf)
{
	assert_eq(position, 1);
	position++;
	int result = setjmp(buf);
	if (result == 0)
	{
		assert_eq(position, 2);
		position++;
		dolongjump(buf);
	}
	assert_eq(position, 4);
}

BEGIN_TEST(libc_setjmp)
	jmp_buf buf;
	assert_eq(position, 0);
	position++;
	dosetjump(buf);
END_TEST
