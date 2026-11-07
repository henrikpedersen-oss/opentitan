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
 * CHERIoT port (lowRISC) of cheri-c-tests/clang-purecap/clang_purecap_capret.c. The test calls
 * a hand-written assembly function that returns by jumping to the capability return register;
 * returning at all shows that the compiler's call sequence hands the callee a return address
 * that is a valid, executable capability. Upstream has only a CHERI-MIPS body (`cjr $c17`) and
 * an #error for every other target. The port replaces it with the CHERIoT equivalent, `cjr cra`;
 * the test body is unchanged.
 *
 * On CHERIoT the call is `cjal cra, _capret_foo` (run_test_capret in cheri_c_tests.dump), which
 * seals the link as a backward sentry in cra (cheriot-sail cheri_insts.sail CJAL), and `cjr cra`
 * -- cjalr cnull, cra, which objdump shows as `cret` -- is the only jump a backward sentry is
 * allowed for (CJALR: isAllowedSentryType). If cra held anything else -- an untagged value,
 * an unsealed or forward-sealed capability, or one without Permit_Execute -- the cjr would raise a
 * tag, seal or permit-execute violation, and the test would be unwound and report CRASH:/FAIL.
 *
 * The function is declared with an asm label because the wrapper compiles this file as C++: the
 * plain declaration would refer to the mangled name _Z11_capret_foov, which the assembly does not
 * define.
 *
 * clang_purecap_capretaddr checks the same return capability's tag, permit-execute and offset
 * from C; this test checks that a return through it works.
 */

#include "cheri_c_test.h"

void _capret_foo(void) __asm__("_capret_foo");

#if defined(__riscv) && defined(__CHERI_PURE_CAPABILITY__)
__asm__(
	".text\n"
	".globl	_capret_foo\n"
	".p2align	1\n"
	".type	_capret_foo,@function\n"
"_capret_foo:\n"
	"cjr	cra\n"
"._capret_foo_end:\n"
	".size	_capret_foo, ._capret_foo_end-_capret_foo\n"
);
#else
#error This test checks that return addresses are capabilities.
#endif

BEGIN_TEST(clang_purecap_capret)
	_capret_foo();
END_TEST
