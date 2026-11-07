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
 * CHERIoT port (lowRISC, 2026) of cheri-c-tests/clang-purecap/clang_purecap_intcap.c. Two
 * upstream assumptions do not hold on CHERIoT; everything else is unchanged.
 *
 * 1. The +50 round trip went through a *global* (`tmp`). `foo` is derived from `str`, a stack
 *    array, so it is a local capability, and CHERIoT clears the tag of a local capability stored
 *    through an authorising capability without StoreLocal -- cgp has none (cheriot-sail
 *    cheri_insts.sail CSC: clearTagIf(cs2, not(auth.permit_store_local_cap) & not(cs2.global))).
 *    The reload was untagged and the dereference faulted: mcause 0x1c, mtval 0x182 (tag
 *    violation on ca2), 2026-09-29. Here the round trip goes through a volatile local instead,
 *    which is stored via csp (which has StoreLocal) and still stops the compiler folding +50-50.
 *    str+55 is inside the 512-byte representable window of an E=0 capability, so the tag must
 *    survive the csetaddr: that part still tests the core.
 * 2. The raw-bytes check assumed a 128/256-bit capability with the cursor in the second 64-bit
 *    word. A CHERIoT capability is 64 bits: address in the low 32, metadata in the high 32, and
 *    (__intcap_t)1 has null metadata, so its one 64-bit word is 1.
 */
#include "cheri_c_test.h"
extern volatile __intcap_t one;
extern volatile __intcap_t two;
volatile __intcap_t one = 1;
volatile __intcap_t two = 2;

BEGIN_TEST(clang_purecap_intcap)
	char str[] = "0123456789";
	volatile __intcap_t tmp; // CHERIoT: a local, not a global (see 1. above)
	__intcap_t foo = 42;
	assert_eq(__builtin_cheri_tag_get((void*)foo), 0);
	assert_eq(__builtin_cheri_offset_get((void*)foo), 42);
	assert_eq(__builtin_cheri_base_get((void*)foo), 0);
	foo = (__intcap_t)str;
	assert_eq(__builtin_cheri_tag_get((void*)foo), 1);
	assert_eq(__builtin_cheri_tag_get((void*)foo), 1);
	foo += 5;
	assert_eq((*(char*)foo), '5');
	assert_eq(__builtin_cheri_offset_get((void*)foo), 5);
	assert_eq(__builtin_cheri_base_get((void*)foo), __builtin_cheri_base_get(str));
	assert_eq(__builtin_cheri_length_get((void*)foo), __builtin_cheri_length_get(str));
	foo += 50;
	// Ensure that the +50 is not removed
	tmp = foo;
	foo = tmp;
	// CHERIoT: str+55 is representable, so the tag must still be set here.
	assert_eq(__builtin_cheri_tag_get((void*)foo), 1);
	foo -= 50;
	assert_eq((*(char*)foo), '5');
	assert_eq(__builtin_cheri_offset_get((void*)foo), 5);
	// Valid capabilities are not strictly ordered after invalid ones
	// We only compare the virtual address
	assert(0xffffffffU > (__uintcap_t)foo); // CHERIoT: 32-bit addresses
	assert((__uintcap_t)0 < (__uintcap_t)foo && "Tag bits should not be included in relational comparisons");
	assert_eq_cap((void*)one, (void*)(__intcap_t)1);
	// When casted to an int it should always be one
	assert_eq((__uint64_t)one, (__uint64_t)1);
	// Also check the raw bytes to debug emulator issues. CHERIoT: one 64-bit word, address 1 in
	// the low half, null metadata (zero) in the high half (see 2. above).
	volatile __uint64_t* one_bytes = (volatile __uint64_t*)&one;
	assert_eq(one_bytes[0], 1);

	// Check that storing a capability always yields the same value back
	__intcap_t tmp2 = two;
	assert_eq_cap((void*)tmp2, (void*)(__intcap_t)2);
	assert_eq((__uint64_t)tmp2, 2);
	two = 3; // change the value
	assert_eq_cap((void*)two, (void*)(__intcap_t)3);
	assert_eq((__uint64_t)two, 3);
	two = tmp2; // restore old value
	assert_eq_cap((void*)two, (void*)(__intcap_t)2);
	assert_eq((__uint64_t)two, 2);
END_TEST
