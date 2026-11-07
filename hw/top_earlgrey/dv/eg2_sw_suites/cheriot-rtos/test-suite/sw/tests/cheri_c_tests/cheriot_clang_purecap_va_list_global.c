/*-
 * Copyright (c) 2018 Alex Richardson
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
 * CHERIoT port (lowRISC) of cheri-c-tests/clang-purecap/clang_purecap_va_list_global.c.
 *
 * Upstream stores a va_list in a global (as libxo does), uses it from another function, and
 * expects it to work for both arguments and then fault once, on a length violation, when a third
 * argument is read past its bounds. Neither half holds on CHERIoT, and the port tests what the
 * same program must do there instead:
 *
 * 1. A CHERIoT va_list is derived from the stack pointer (the callee's incoming csp moved by
 *    cincoffset; llvm-cheriot 17, see printstuff in cheri_c_tests.dump), so it is a local
 *    capability. Stored into global_info -- through a capability derived from cgp, which lacks
 *    StoreLocal -- it loses its tag (cheriot-sail cheri_insts.sail CSC: clearTagIf(cs2,
 *    not(auth.permit_store_local_cap) & not(cs2.global))). The expected outcome is therefore:
 *    the va_list read back from the global is untagged, and the first va_arg through it raises
 *    a CHERI tag violation. print_impl() asserts the tag is clear, and the wrapper's error handler
 *    checks that the fault is a tag violation.
 * 2. The CHERIoT va_list is not bounded to the variadic arguments (no csetbounds after va_start);
 *    it keeps the stack capability's bounds. The length and offset assertions on it are removed,
 *    as is the one after print_impl() returns (never reached: the fault unwinds the compartment).
 * 3. print_impl() is marked noinline, so that it reads the va_list back from global_info rather
 *    than the compiler forwarding the tagged value va_start left in a register -- the round trip
 *    through memory is what the test is about.
 *
 * The rest of the loop is kept as upstream wrote it, although on CHERIoT it is not reached.
 */

// Should have one trap (dereferencing one arg too many)
#define TEST_EXPECTED_FAULTS 1
#include "cheri_c_test.h"
#include <stdarg.h>
#include <stddef.h>

/* Check that the crazy stuff libxo does (storing va_list and using later works) */
struct print_info {
    va_list vap;
    int num_args;
};

const char* arg1 = "arg1";

const char* arg2 = "arg2";

struct print_info global_info;

__noinline void print_impl(struct print_info* info) // CHERIoT: noinline (see 3. above)
{
	DEBUG_MSG("Called print_impl!");
	assert_eq(info->num_args, 3);
	// CHERIoT: upstream checks the va_list's length and offset here; not applicable (see 2.).
	// A local capability stored through cgp loses its tag (see 1.).
	assert_eq(__builtin_cheri_tag_get((void*)info->vap), 0);
	for (int i = 0; i < info->num_args; i++) {
		// CHERIoT: the first va_arg faults, on the tag (see 1.); the offset checks around it are
		// not applicable (see 2.).
		char* cp = va_arg(info->vap, char *);
		assert(cp != NULL);
		if (i == 2) {
			// At this point we will have attempted to load
			// from from past the end of the va_list.
			// XXX: even with optnone, the load doesn't
			// actually occur until after the assert()...
			assert_eq(faults, 1);
			break;
		}
		assert(*cp != '\0');
		assert_eq(cp[0], 'a');
		assert_eq(cp[1], 'r');
		assert_eq(cp[2], 'g');
		assert_eq(cp[3], '1' + i);
		DEBUG_MSG(cp);
	}
	assert_eq(faults, 1);
}

static void printstuff(int num_args, ...)
{
	DEBUG_MSG("Called printstuff!");
	// Store in a global: see xo_emit
	struct print_info* pi = &global_info;
	pi->num_args = num_args;
	va_start(pi->vap, num_args);
	print_impl(pi);
	va_end(pi->vap);
	// CHERIoT: upstream checks the va_list's offset here; not applicable (see 2.).
	__builtin_memset(&pi->vap, 0, sizeof(pi->vap));
}

BEGIN_TEST(clang_purecap_va_list_global)
	// There are only two args so it should die after the second one
	printstuff(3, "arg1", "arg2");
END_TEST
