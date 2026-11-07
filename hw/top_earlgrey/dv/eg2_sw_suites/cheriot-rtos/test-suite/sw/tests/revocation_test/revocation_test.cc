// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Directed test for the CHERIoT hardware load barrier (ibex_trvk).
//
// Why this exists, when cwe415/cwe416 already cover use-after-free:
//
// Those tests only check one direction — that a stale capability loses its tag.
// A load barrier that cleared *every* tag would pass them both, and so would a
// barrier wired to a constant "always revoked". Worse, the reverse stub is what
// we actually shipped for a while: sonata_system.sv fed trvk_revbm_rdata_i a
// constant 32'h0 ("nothing is ever revoked"), and the RTOS suite's own
// test_revoke still reported "Checked that all allocations have been
// deallocated (0..7 of 8)". A test that passes whether or not the hardware is
// connected tells us nothing.
//
// So this test drives the shadow bitmap *directly* over MMIO rather than going
// through heap_free, and asserts both directions on the same object:
//
//   Phase 1  bit clear  -> reloaded capability keeps its tag   (no false positive)
//   Phase 2  bit set    -> reloaded capability loses its tag   (barrier fires)
//   Phase 3  bit clear  -> reloaded capability keeps its tag   (recovers, not latched)
//
// Phase 1 is the one that makes the result meaningful: it fails if the barrier
// is stuck clearing everything, which is the failure mode the CWE tests cannot
// see. Phase 3 catches a barrier that latches on first fire.
//
// Then a sweep check (test_revocation_sweep), and a randomised check of the
// barrier and the sweep over 32 capabilities per iteration
// (test_revocation_random, after lowRISC/opentitan#31725). Then three checks of
// revocation against the rest of the system: a preempted thread's saved
// registers (test_revocation_preempt), loads and stores racing a running sweep
// (test_revocation_sweep_race), and the core's progress while a sweep runs
// (test_revocation_sweep_progress).
//
// Bitmap geometry (sonata board.json and tl_main_pkg.sv agree):
//   shadow region        0x30000000, 0x800 bytes = 512 words = 16384 bits
//   revokable_memory     starts 0x00100000  (== ibex_top's trvk_heap_base_addr_i)
//   one bit per 8 bytes of revokable memory
// ibex_trvk computes its index the same way:
//   revbm_cap_addr = cap_base - heap_base_addr_i   (ibex_trvk.sv:328)
// so the bit is selected by the *base of the loaded capability*, not by the
// address the capability was loaded from.

#include <compartment.h>
#include <debug.hh>
#include "../common/sim_exit.hh"
#include <futex.h>
#include <riscvreg.h>
#include <stdlib.h>
#include <thread.h>

using Debug = ConditionalDebug<true, "Revocation barrier">;

namespace
{
	/// Start of revokable memory; must match board.json revokable_memory_start
	/// and the trvk_heap_base_addr_i tie-off in sonata_system.sv.
	constexpr size_t RevokableMemoryStart = 0x0010'0000;

	/// One shadow bit per this many bytes of revokable memory.
	constexpr size_t BytesPerShadowBit = 8;

	/// Shadow bitmap as words. The device is named "shadow" in board.json.
	/// MMIO_CAPABILITY already yields a volatile pointer, so the type argument
	/// must not repeat it.
	volatile uint32_t *shadow_bitmap()
	{
		return MMIO_CAPABILITY(uint32_t, shadow);
	}

	/// Index of the shadow bit covering `addr`, as (word, bit-in-word).
	void shadow_index(size_t addr, size_t &word, size_t &bit)
	{
		size_t bitIndex = (addr - RevokableMemoryStart) / BytesPerShadowBit;
		word            = bitIndex / 32;
		bit             = bitIndex % 32;
	}

	void set_shadow_bit(size_t addr, bool value)
	{
		size_t word, bit;
		shadow_index(addr, word, bit);
		volatile uint32_t *bitmap = shadow_bitmap();
		uint32_t           v      = bitmap[word];
		bitmap[word] = value ? (v | (uint32_t(1) << bit)) : (v & ~(uint32_t(1) << bit));
	}

	/// The hardware revoker's registers (Sonata rev_ctl; same layout as
	/// platform-hardware_revoker.hh). With the CHERIoT memory subsystem these
	/// drive its revocation engine (TBRE) through cheriot_rev_ctl_tbre.sv.
	struct RevokerInterface
	{
		uint32_t base;
		uint32_t top;
		uint32_t control;
		uint32_t epoch; ///< low bit set while a sweep runs; +2 per sweep
		uint32_t interruptStatus;
		uint32_t interruptRequested;
	};

	volatile RevokerInterface *revoker()
	{
		return MMIO_CAPABILITY(RevokerInterface, revoker);
	}

	/// Capabilities stored in this compartment's globals, which lie inside the
	/// revoker's sweep range (compartment globals to end of heap, set by the
	/// allocator at boot). volatile so every access is a real load/store.
	void *volatile sweptSlot;
	void *volatile keptSlot;
	/// Untagged copies of the swept and the kept capability, also in the sweep
	/// range: a revocation engine that only invalidates leaves them untagged
	/// and unchanged (test_revocation_sweep).
	void *volatile sweptUntaggedSlot;
	void *volatile keptUntaggedSlot;

	/// The barrier test's object, stored once with its shadow bit clear. The
	/// barrier strips tags on the way into a register and never touches the
	/// copy in memory, so every phase reloads this same copy.
	///
	/// volatile so every read is a real load for the barrier to act on --
	/// without that every phase would trivially "pass".
	void *volatile barrierSlot;

	/// Reload the barrier test's object and report whether it kept its tag.
	bool tag_survives_reload()
	{
		void *reload = barrierSlot;
		return __builtin_cheri_tag_get(reload);
	}
} // namespace

/// Returns true if every phase behaved as expected.
bool test_revocation_barrier()
{
	Timeout t{5};
	void   *p = heap_allocate(&t, MALLOC_CAPABILITY, 64);
	if (!__builtin_cheri_tag_get(p))
	{
		Debug::log("FAIL: heap_allocate returned an untagged capability");
		return false;
	}

	size_t base = __builtin_cheri_base_get(p);
	size_t word, bit;
	shadow_index(base, word, bit);
	// The testbench reads the base from this line ("base 0x... -> shadow word"): with
	// +cheriot_rtos_mutate=barrier_all it makes the barrier revoke this object regardless of its bit
	// (tb/cheriot_rtos_tb.sv). Keep the wording in step with it.
	Debug::log("object {} base {} -> shadow word {} bit {}", p, base, word, bit);

	if (base < RevokableMemoryStart)
	{
		Debug::log("FAIL: allocation at {} is below revokable memory start {}",
		           base,
		           RevokableMemoryStart);
		heap_free(MALLOC_CAPABILITY, p);
		return false;
	}

	// While the bit is set, *any* load of a capability to this object loses its
	// tag -- including a callee restoring the saved register that holds `p`.
	// debug_log_message_write saves and restores cs1, which is where `p` lives,
	// so a Debug::log between setting and clearing the bit strips `p` itself:
	// that is what made phase 3 fail on 2026-09-29 with a correct barrier. So
	// the three phases run back to back with no calls but the leaf
	// set_shadow_bit, every phase reloads the copy stored here with the bit
	// clear, and the results are logged afterwards.
	set_shadow_bit(base, false);
	barrierSlot = p;

	// Let the object line above reach the testbench before phase 1. The UART sends a character
	// every ~430 cycles and the firmware runs ahead of it by up to its FIFO, while the phases
	// take a few hundred cycles: without the wait, barrier_all would arm after they ran.
	// Two ticks so the sleep is at least one full tick (400k cycles at 100 Hz); NoEarlyWake, or
	// the only runnable thread is woken at once. Here, after the store, so that the copy the
	// phases reload was stored with its tag whatever the sleep's register restore does to `p`
	// once the barrier revokes it.
	{
		Timeout s{2};
		thread_sleep(&s, ThreadSleepNoEarlyWake);
	}

	// Phase 1: bit clear -> tag must survive. Guards against a barrier that
	// clears every tag, and against one wired to a constant "always revoked".
	bool phase1 = tag_survives_reload();

	// Phase 2: bit set -> tag must be cleared. The actual revocation check.
	// Fails if the barrier is stubbed to "nothing is ever revoked", which is
	// how sonata_system.sv used to wire it.
	set_shadow_bit(base, true);
	bool phase2 = !tag_survives_reload();

	// Phase 3: bit clear again -> tag must survive. Catches a barrier that
	// latches after its first fire.
	set_shadow_bit(base, false);
	bool phase3 = tag_survives_reload();

	if (phase1)
	{
		Debug::log("PASS phase 1: tag survived reload with shadow bit clear");
	}
	else
	{
		Debug::log("FAIL phase 1: tag cleared with shadow bit CLEAR "
		           "(barrier is revoking unconditionally)");
	}
	if (phase2)
	{
		Debug::log("PASS phase 2: tag cleared on reload with shadow bit set");
	}
	else
	{
		Debug::log("FAIL phase 2: tag survived with shadow bit SET "
		           "(barrier is not reading the bitmap)");
	}
	if (phase3)
	{
		Debug::log("PASS phase 3: tag survived again after clearing shadow bit");
	}
	else
	{
		Debug::log("FAIL phase 3: tag still cleared after shadow bit cleared "
		           "(barrier latched)");
	}

	// The bit is clear again, so this reload keeps its tag even if `p`'s
	// register copy was stripped.
	void *freeMe = barrierSlot;
	barrierSlot  = nullptr;
	heap_free(MALLOC_CAPABILITY, freeMe);
	return phase1 && phase2 && phase3;
}

/// Sweep check: the revocation engine clears the tag of a revoked capability
/// *in memory*, and of nothing else.
///
/// Phases 1-3 only exercise the load barrier, which strips the tag on the way
/// into a register. Here the barrier is taken out of the picture: the bitmap
/// bit is cleared again before the stored copies are read back, so only a
/// sweep can have removed a tag.
///
///   sweptSlot = p, keptSlot = q; set p's bit; run one sweep; clear p's bit
///   -> sweptSlot must have lost its tag (the sweep ran and revoked it)
///   -> keptSlot must still have its tag (the sweep did not clear everything)
///
/// Then the TBRE invalidate-only check (testplan bus_master_tbre_invalidate_only):
/// the TBRE writes memory without the core's capability checks, so it must
/// write nothing but tag clears. After the sweep, compared bit for bit with
/// CSEQX (__builtin_cheri_equal_exact, which includes the tag):
///   -> sweptSlot == p with its tag cleared (no other bit written)
///   -> keptSlot == q (not written at all)
///   -> untagged copies of p and of q, stored beside them, are unchanged (a
///      tag is never set, and data the sweep looks at is never written)
///
/// Without a sweep engine (the old Sonata flow's default build) the epoch never moves; that
/// is reported as SKIP, not failure. The CHERIoT memory subsystem build
/// (make cheriot-rtos-revocation-xlm) requires "PASS sweep".
enum class SweepResult
{
	Pass,
	Fail,
	Skip
};

SweepResult test_revocation_sweep()
{
	Timeout t{5};
	void   *p = heap_allocate(&t, MALLOC_CAPABILITY, 64);
	void   *q = heap_allocate(&t, MALLOC_CAPABILITY, 64);
	if (!__builtin_cheri_tag_get(p) || !__builtin_cheri_tag_get(q))
	{
		Debug::log("FAIL sweep: heap_allocate returned an untagged capability");
		return SweepResult::Fail;
	}
	size_t pBase = __builtin_cheri_base_get(p);
	size_t qBase = __builtin_cheri_base_get(q);
	size_t pWord, pBit, qWord, qBit;
	shadow_index(pBase, pWord, pBit);
	shadow_index(qBase, qWord, qBit);
	if (pWord == qWord && pBit == qBit)
	{
		Debug::log("FAIL sweep: the two objects share a shadow bit");
		return SweepResult::Fail;
	}

	// Expected values, taken before p's shadow bit is set: while it is set a
	// callee restoring the register that holds p strips p's tag, and these
	// untagged values cannot be stripped.
	void *pUntagged   = __builtin_cheri_tag_clear(p);
	void *qUntagged   = __builtin_cheri_tag_clear(q);
	sweptSlot         = p;
	keptSlot          = q;
	sweptUntaggedSlot = pUntagged;
	keptUntaggedSlot  = qUntagged;
	set_shadow_bit(pBase, true);

	// Start one sweep, as the allocator's driver does (system_bg_revoker_kick):
	// only from an even epoch, by writing control 0 then 1.
	volatile RevokerInterface *dev = revoker();
	uint32_t                   start;
	int                        waited = 0;
	// ThreadSleepNoEarlyWake on every sleep here: without it a sleeping thread
	// is woken as soon as no other thread is runnable -- always, in this test --
	// so the 50 polls below took a few thousand cycles in all and gave up on
	// 2026-09-29 while a sweep that finished ~85k cycles after starting was
	// still running (trbe_monitor in the old flow's sonata-xlm/top_sonata_xlm.sv).
	while (((start = dev->epoch) & 1) && waited++ < 50)
	{
		Timeout s{1};
		thread_sleep(&s, ThreadSleepNoEarlyWake);
	}
	dev->control = 0;
	dev->control = 1;

	// Wait for the epoch to advance by a full sweep (+2 from an even value).
	// One tick (400k cycles) is ample: a full sweep of the ~16k capabilities
	// takes ~85k cycles; 50 ticks is the hang bound.
	bool started = false;
	bool done    = false;
	for (int i = 0; i < 50 && !done; i++)
	{
		uint32_t now = dev->epoch;
		started |= (now != start);
		done = (now - start) >= 2 && !(now & 1);
		if (!done)
		{
			Timeout s{1};
			thread_sleep(&s, ThreadSleepNoEarlyWake);
		}
	}

	set_shadow_bit(pBase, false);
	void *swept    = sweptSlot;
	void *kept     = keptSlot;
	bool  sweptTag = __builtin_cheri_tag_get(swept);
	bool  keptTag  = __builtin_cheri_tag_get(kept);
	bool  sweptOnlyTag =
	  __builtin_cheri_equal_exact(swept, pUntagged);
	bool keptExact = __builtin_cheri_equal_exact(kept, q);
	bool untaggedKept =
	  __builtin_cheri_equal_exact(sweptUntaggedSlot, pUntagged) &&
	  __builtin_cheri_equal_exact(keptUntaggedSlot, qUntagged);
	sweptSlot         = nullptr;
	keptSlot          = nullptr;
	sweptUntaggedSlot = nullptr;
	keptUntaggedSlot  = nullptr;
	heap_free(MALLOC_CAPABILITY, p);
	heap_free(MALLOC_CAPABILITY, q);

	if (!started)
	{
		Debug::log("SKIP sweep: the revoker's epoch never moved "
		           "(no revocation engine in this build)");
		return SweepResult::Skip;
	}
	if (!done)
	{
		Debug::log("FAIL sweep: a sweep started but did not finish (epoch {} -> {})",
		           start,
		           dev->epoch);
		return SweepResult::Fail;
	}
	bool passed = true;
	if (sweptTag)
	{
		Debug::log("FAIL sweep: revoked capability kept its tag in memory "
		           "(the sweep did not clear it)");
		passed = false;
	}
	if (!keptTag)
	{
		Debug::log("FAIL sweep: unrevoked capability lost its tag in memory "
		           "(the sweep cleared too much)");
		passed = false;
	}
	if (passed)
	{
		Debug::log("PASS sweep: revoked capability cleared in memory, "
		           "unrevoked one kept (epoch {} -> {})",
		           start,
		           dev->epoch);
	}
	if (sweptOnlyTag && keptExact && untaggedKept)
	{
		Debug::log("PASS TBRE invalidate-only: the sweep changed only the "
		           "revoked capability's tag");
	}
	else
	{
		Debug::log("FAIL TBRE invalidate-only: revoked word {} (expected {}), "
		           "kept word {}, untagged copies {}",
		           sweptOnlyTag ? "tag cleared only" : "changed otherwise",
		           pUntagged,
		           keptExact ? "unchanged" : "CHANGED",
		           untaggedKept ? "unchanged" : "CHANGED");
		passed = false;
	}
	return passed ? SweepResult::Pass : SweepResult::Fail;
}

#ifndef REVOCATION_RANDOM_ITERATIONS
#	define REVOCATION_RANDOM_ITERATIONS 32
#endif
#ifndef REVOCATION_RANDOM_SEED
#	define REVOCATION_RANDOM_SEED 1
#endif

namespace
{
	/// One shadow word covers 32 granules of 8 bytes: 256 bytes, 32 capabilities.
	constexpr size_t CapsPerWord  = 32;
	constexpr size_t RegionBytes  = CapsPerWord * BytesPerShadowBit;
	constexpr size_t RandomBlock  = 6 * RegionBytes;
	constexpr int    SweepPollMax = 2'000'000;

	/// Xorshift32 (needs a non-zero state), as lowRISC/opentitan#31725 uses.
	inline uint32_t xorshift32(uint32_t &x)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return x;
	}

	/// Bit i set if slots[i] holds a tagged capability. Inlined: no call may run while a
	/// shadow bit is set (see test_revocation_barrier).
	__attribute__((always_inline)) inline uint32_t tags_of(void *volatile *slots)
	{
		uint32_t tags = 0;
		for (size_t i = 0; i < CapsPerWord; i++)
		{
			if (__builtin_cheri_tag_get(slots[i]))
			{
				tags |= uint32_t(1) << i;
			}
		}
		return tags;
	}

	struct RandomFailure
	{
		int      iteration;
		int      step;
		uint32_t word;
		uint32_t expectedA;
		uint32_t gotA;
		uint32_t gotB;
	};
} // namespace

/// Randomised load barrier and sweep check, a port of lowRISC/opentitan#31725
/// (sw/device/tests/cheriot_tbre_revocation_test.S) to this SoC and the RTOS.
///
/// Phases 1-3 and the sweep check each use one object. Here, in every iteration, region A holds
/// 32 capabilities, each bounded to one 8-byte granule of region B (one shadow word) at a random
/// offset in it, and a random value is written to B's shadow word:
///
///   step 1  word 0             -> every capability in A is tagged, nothing in B is
///   step 2  word w             -> A reloads with tags ~w: the barrier strips exactly the
///                                 capabilities whose granule's bit is set
///   step 3  word 0 again       -> A is all tagged again: the barrier only hid the tags
///   step 4  word w, one sweep, word 0
///                              -> A reloads with tags ~w: the sweep cleared exactly those in
///                                 memory, and set none (B stays untagged)
///
/// Differences from the original: the sweep is started through rev_ctl, as the RTOS's revoker
/// does, and covers the whole revocable range, not only region A; A and B come from one heap
/// block rather than anywhere in SRAM; B is also checked untagged after every step. The block's
/// own capability has its base below B, so B's shadow word never revokes it, and A and B are
/// reached through it, never through a capability bounded to B. As in phases 1-3, no call runs
/// while the word is set; results are recorded and logged once it is clear.
bool test_revocation_random()
{
	Timeout t{5};
	char   *block = static_cast<char *>(heap_allocate(&t, MALLOC_CAPABILITY, RandomBlock));
	if (!__builtin_cheri_tag_get(block))
	{
		Debug::log("FAIL random: heap_allocate returned an untagged capability");
		return false;
	}
	size_t base = __builtin_cheri_base_get(block);
	// First region boundary strictly above the block's base, and how many regions fit.
	size_t first   = (base / RegionBytes + 1) * RegionBytes;
	size_t regions = (base + RandomBlock - first) / RegionBytes;

	volatile uint32_t         *bitmap = shadow_bitmap();
	volatile RevokerInterface *dev    = revoker();

	uint32_t      state = REVOCATION_RANDOM_SEED ? REVOCATION_RANDOM_SEED : 1;
	RandomFailure fail{-1, 0, 0, 0, 0, 0};
	int           done = 0;
	for (int it = 0; it < REVOCATION_RANDOM_ITERATIONS && fail.iteration < 0; it++)
	{
		size_t bIndex = xorshift32(state) % regions;
		size_t aIndex = (bIndex + 1 + xorshift32(state) % (regions - 1)) % regions;
		size_t offset = xorshift32(state) % BytesPerShadowBit;
		char  *b      = block + (first - base) + bIndex * RegionBytes;
		void *volatile *aSlots = reinterpret_cast<void *volatile *>(
		  block + (first - base) + aIndex * RegionBytes);
		void *volatile *bSlots = reinterpret_cast<void *volatile *>(b);
		size_t          word, bit;
		shadow_index(first + bIndex * RegionBytes, word, bit);

		// Step 1: A points into B's granules; B holds no capabilities.
		for (size_t i = 0; i < CapsPerWord; i++)
		{
			void *granule = __builtin_cheri_bounds_set(b + i * BytesPerShadowBit,
			                                           BytesPerShadowBit);
			aSlots[i]     = static_cast<char *>(granule) + offset;
			bSlots[i]     = nullptr;
		}
		uint32_t a = tags_of(aSlots), bt = tags_of(bSlots);
		if (a != ~uint32_t(0) || bt != 0)
		{
			fail = {it, 1, 0, ~uint32_t(0), a, bt};
			break;
		}

		uint32_t w = xorshift32(state);

		// Step 2: the barrier strips exactly the capabilities whose bit is set.
		bitmap[word] = w;
		a            = tags_of(aSlots);
		bitmap[word] = 0;
		if (a != ~w)
		{
			fail = {it, 2, w, ~w, a, 0};
			break;
		}

		// Step 3: with the word clear every tag is back.
		a  = tags_of(aSlots);
		bt = tags_of(bSlots);
		if (a != ~uint32_t(0) || bt != 0)
		{
			fail = {it, 3, w, ~uint32_t(0), a, bt};
			break;
		}

		// Step 4: one sweep with the word set, then reload with it clear.
		bitmap[word]   = w;
		uint32_t start = dev->epoch;
		int      polls = 0;
		while ((start & 1) && polls++ < SweepPollMax)
		{
			start = dev->epoch;
		}
		dev->control = 0;
		dev->control = 1;
		uint32_t now = start;
		while (!((now - start) >= 2 && !(now & 1)) && polls++ < SweepPollMax)
		{
			now = dev->epoch;
		}
		bitmap[word] = 0;
		if (!((now - start) >= 2 && !(now & 1)))
		{
			fail = {it, 4, w, start, now, 0xFFFFFFFF};
			break;
		}
		a  = tags_of(aSlots);
		bt = tags_of(bSlots);
		if (a != ~w || bt != 0)
		{
			fail = {it, 4, w, ~w, a, bt};
			break;
		}
		done++;
	}

	// Every word is clear again: logging cannot strip anything now.
	heap_free(MALLOC_CAPABILITY, block);
	if (fail.iteration >= 0)
	{
		if (fail.step == 4 && fail.gotB == 0xFFFFFFFF)
		{
			Debug::log("FAIL random: iteration {} step 4: the sweep did not finish (epoch {} -> {})",
			           fail.iteration,
			           fail.expectedA,
			           fail.gotA);
		}
		else
		{
			Debug::log("FAIL random: iteration {} step {}: shadow word {}, tags in A {} "
			           "(expected {}), tags in B {} (expected 0)",
			           fail.iteration,
			           fail.step,
			           fail.word,
			           fail.gotA,
			           fail.expectedA,
			           fail.gotB);
		}
		return false;
	}
	Debug::log("PASS random: {} iterations of 32 capabilities, seed {} "
	           "(barrier and sweep exact, tags restored, B untagged)",
	           done,
	           REVOCATION_RANDOM_SEED);
	return true;
}

namespace
{
	/// test_revocation_base_not_address's capabilities, stored while every bit is clear, and the
	/// allocation itself, reloaded for the free once the bits are clear again.
	void *volatile baseRevokedSlot;
	void *volatile addressRevokedSlot;
	void *volatile zeroLengthSlot;
	void *volatile baseTestObjectSlot;
} // namespace

/// The barrier revokes by a capability's base, not by its address (ibex_trvk.sv:328,
/// revbm_cap_addr = cap_base - heap_base). Every other check here keeps a capability's address
/// inside the granule of its base, so a barrier indexing by address would pass them. Three cases
/// cross a granule boundary, in a 128-byte allocation P (granules G0..G15):
///   base   bounds [P, P+8), address P+24 (G3):   G0's bit set -> loses its tag
///   addr   bounds [P+24, P+32), address P+40 (G5): G5's bit set -> keeps its tag
/// The address of the second lies past its top, not below its base: CHERIoT has no representable
/// space below a base (cheriot-sail getCapBoundsBits: a_mid < B is read as the region above), so a
/// capability with its address below its base cannot exist. Past the top it is representable up to
/// base + 2^(E+9), here 512 bytes.
///   zero   zero length at P+64, one past the end of the sub-object [P, P+64): G0..G7 set (that
///          sub-object freed) -> keeps its tag, as its base is G8; G8 set too -> loses it
/// The last is the end-of-object case: a zero-length or one-past-the-end capability belongs to
/// the granule after the object, so freeing the object must not revoke it.
/// Same discipline as test_revocation_barrier: everything is stored before a bit is set, nothing
/// but the leaf set_shadow_bit runs while one is set, and every check is a reload from memory.
bool test_revocation_base_not_address()
{
	constexpr size_t G = BytesPerShadowBit;
	Timeout          t{5};
	char *p = static_cast<char *>(heap_allocate(&t, MALLOC_CAPABILITY, 16 * G));
	if (!__builtin_cheri_tag_get(p))
	{
		Debug::log("FAIL base: heap_allocate returned an untagged capability");
		return false;
	}
	size_t base = __builtin_cheri_base_get(p);
	if (base < RevokableMemoryStart || base % G != 0)
	{
		Debug::log("FAIL base: allocation at {} is below revokable memory or not granule-aligned",
		           base);
		heap_free(MALLOC_CAPABILITY, p);
		return false;
	}
	void *a = __builtin_cheri_address_set(__builtin_cheri_bounds_set(p, G), base + 3 * G);
	void *b = __builtin_cheri_address_set(__builtin_cheri_bounds_set(p + 3 * G, G), base + 5 * G);
	void *c = __builtin_cheri_bounds_set(p + 8 * G, 0);
	if (!__builtin_cheri_tag_get(a) || !__builtin_cheri_tag_get(b) ||
	    !__builtin_cheri_tag_get(c) || __builtin_cheri_base_get(c) != base + 8 * G)
	{
		Debug::log("FAIL base: test capabilities not representable (setup, not the barrier)");
		heap_free(MALLOC_CAPABILITY, p);
		return false;
	}
	for (size_t i = 0; i < 16; i++)
	{
		set_shadow_bit(base + i * G, false);
	}
	baseTestObjectSlot = p;
	baseRevokedSlot    = a;
	addressRevokedSlot = b;
	zeroLengthSlot     = c;

	set_shadow_bit(base, true);
	void *ra       = baseRevokedSlot;
	bool  baseOk   = !__builtin_cheri_tag_get(ra);
	set_shadow_bit(base + 5 * G, true);
	void *rb       = addressRevokedSlot;
	bool  addrOk   = __builtin_cheri_tag_get(rb);
	for (size_t i = 1; i < 8; i++)
	{
		set_shadow_bit(base + i * G, true);
	}
	void *rc       = zeroLengthSlot;
	bool  zeroKept = __builtin_cheri_tag_get(rc);
	set_shadow_bit(base + 8 * G, true);
	void *rc2         = zeroLengthSlot;
	bool  zeroRevoked = !__builtin_cheri_tag_get(rc2);
	for (size_t i = 0; i <= 8; i++)
	{
		set_shadow_bit(base + i * G, false);
	}

	bool ok = baseOk && addrOk && zeroKept && zeroRevoked;
	if (ok)
	{
		Debug::log("PASS base: revoked by base, not by address (base {}, granules G0, G5 and G8)",
		           base);
	}
	else
	{
		Debug::log("FAIL base: base-revoked {} address-kept {} zero-length kept {} "
		           "zero-length revoked by its own granule {} (expected all true)",
		           baseOk,
		           addrOk,
		           zeroKept,
		           zeroRevoked);
	}
	void *freeMe       = baseTestObjectSlot;
	baseTestObjectSlot = nullptr;
	baseRevokedSlot    = nullptr;
	addressRevokedSlot = nullptr;
	zeroLengthSlot     = nullptr;
	heap_free(MALLOC_CAPABILITY, freeMe);
	return ok;
}

namespace
{
	/// Start one sweep as the allocator's driver does (system_bg_revoker_kick): only from an even
	/// epoch, by writing control 0 then 1. Returns the epoch it started from. Inlined: callers run
	/// it with a shadow bit set.
	__attribute__((always_inline)) inline uint32_t
	sweep_start(volatile RevokerInterface *dev)
	{
		uint32_t start = dev->epoch;
		for (int polls = 0; (start & 1) && polls < SweepPollMax; polls++)
		{
			start = dev->epoch;
		}
		dev->control = 0;
		dev->control = 1;
		return start;
	}

	/// A full sweep has run since `start`: the epoch advanced by 2 from an even value.
	__attribute__((always_inline)) inline bool sweep_done(uint32_t start, uint32_t now)
	{
		return (now - start) >= 2 && !(now & 1);
	}

	/// The capabilities the preempted thread loads, its handshake with run_revocation_tests and
	/// the tags it saw. preemptStart is the futex word it waits on.
	void *volatile    preemptRevokedSlot;
	void *volatile    preemptKeptSlot;
	/// volatile, not atomic: one core, and the core has no A extension.
	volatile uint32_t preemptStart;
	volatile uint32_t preemptReady;
	volatile uint32_t preemptGo;
	volatile uint32_t preemptDone;
	volatile uint32_t preemptRevokedTag;
	volatile uint32_t preemptKeptTag;
} // namespace

/// The second thread (xmake.lua: priority 1, below run_revocation_tests' 2). Each round it loads
/// the two capabilities into registers, reports that it holds them and spins, without touching
/// memory, until told to go on, then reports the tags its registers hold. All of it is one asm
/// statement, so the registers are never spilled: whatever happens to their tags in between is
/// the switcher's doing. It runs only while run_revocation_tests sleeps, so it is preempted in the
/// spin, and its registers are saved and restored (trustedReloadRegisters, clc) around every
/// change test_revocation_preempt makes.
[[noreturn]] void __cheri_compartment("revocation_test") revocation_preempt_thread()
{
	while (true)
	{
		while (preemptStart == 0)
		{
			futex_wait(const_cast<uint32_t *>(&preemptStart), 0);
		}
		preemptStart = 0;
		void    *revokedReg, *keptReg;
		uint32_t scratch, revokedTag, keptTag;
		__asm__ volatile("clc     %[rc], 0(%[rs])\n\t"
		                 "clc     %[kc], 0(%[ks])\n\t"
		                 "li      %[tmp], 1\n\t"
		                 "csw     %[tmp], 0(%[ready])\n\t"
		                 "1:\n\t"
		                 "clw     %[tmp], 0(%[go])\n\t"
		                 "beqz    %[tmp], 1b\n\t"
		                 "cgettag %[rt], %[rc]\n\t"
		                 "cgettag %[kt], %[kc]\n\t"
		                 : [rc] "=&C"(revokedReg),
		                   [kc] "=&C"(keptReg),
		                   [tmp] "=&r"(scratch),
		                   [rt] "=&r"(revokedTag),
		                   [kt] "=&r"(keptTag)
		                 : [rs] "C"(&preemptRevokedSlot),
		                   [ks] "C"(&preemptKeptSlot),
		                   [ready] "C"(&preemptReady),
		                   [go] "C"(&preemptGo)
		                 : "memory");
		(void)revokedReg;
		(void)keptReg;
		(void)scratch;
		preemptRevokedTag = revokedTag;
		preemptKeptTag    = keptTag;
		preemptDone       = 1;
	}
}

/// A capability already in a register when its object is revoked is not reloaded by the thread,
/// but it is reloaded by the switcher: a preempted thread's registers are saved to memory and
/// restored with clc, so the barrier strips the revoked one on the way back. In one 32-byte
/// allocation P (granules G0..G3), whose own capability (base G0) is never revoked:
///   revoked  bounds [P+8, P+16), base G1: G1's bit set while the thread is preempted
///            -> its register holds it untagged when it resumes
///   kept     bounds [P+16, P+24), base G2: bit clear -> still tagged after the same restore
/// No sweep runs: this is the barrier on the restore, before any sweep reaches the saved context.
/// Fails under barrier_none. The bit is set only while the other thread is preempted; this thread
/// holds no copy it needs while it is set (its own sleep restores registers too).
bool test_revocation_preempt()
{
	constexpr size_t G = BytesPerShadowBit;
	Timeout          t{5};
	char *block = static_cast<char *>(heap_allocate(&t, MALLOC_CAPABILITY, 4 * G));
	if (!__builtin_cheri_tag_get(block))
	{
		Debug::log("FAIL preempt: heap_allocate returned an untagged capability");
		return false;
	}
	size_t base = __builtin_cheri_base_get(block);
	if (base < RevokableMemoryStart || base % G != 0)
	{
		Debug::log("FAIL preempt: allocation at {} is below revokable memory or not "
		           "granule-aligned",
		           base);
		heap_free(MALLOC_CAPABILITY, block);
		return false;
	}
	for (size_t i = 0; i < 4; i++)
	{
		set_shadow_bit(base + i * G, false);
	}
	preemptRevokedSlot = __builtin_cheri_bounds_set(block + G, G);
	preemptKeptSlot    = __builtin_cheri_bounds_set(block + 2 * G, G);
	preemptReady       = 0;
	preemptGo          = 0;
	preemptDone        = 0;
	preemptStart = 1;
	futex_wake(const_cast<uint32_t *>(&preemptStart), 1);

	// The other thread runs while this one sleeps; this one's wake-up preempts it in its spin.
	for (int i = 0; i < 50 && !preemptReady; i++)
	{
		Timeout s{1};
		thread_sleep(&s, ThreadSleepNoEarlyWake);
	}
	bool ready = preemptReady;
	if (ready)
	{
		// It is preempted now, its registers saved with both capabilities tagged.
		set_shadow_bit(base + G, true);
		preemptGo = 1;
		for (int i = 0; i < 50 && !preemptDone; i++)
		{
			Timeout s{1};
			thread_sleep(&s, ThreadSleepNoEarlyWake);
		}
		set_shadow_bit(base + G, false);
	}
	// Release it in every case, so that it parks on the futex again.
	preemptGo        = 1;
	bool     done    = preemptDone;
	uint32_t revoked = preemptRevokedTag;
	uint32_t kept    = preemptKeptTag;
	preemptRevokedSlot = nullptr;
	preemptKeptSlot    = nullptr;
	heap_free(MALLOC_CAPABILITY, block);

	if (!ready || !done)
	{
		Debug::log("FAIL preempt: the second thread {} (ready {}, done {})",
		           ready ? "did not finish" : "never held the capabilities",
		           ready,
		           done);
		return false;
	}
	if (revoked != 0 || kept != 1)
	{
		Debug::log("FAIL preempt: after the restore the revoked capability's tag is {} "
		           "(expected 0), the unrevoked one's {} (expected 1)",
		           revoked,
		           kept);
		return false;
	}
	Debug::log("PASS preempt: a capability revoked while its thread was preempted came back "
	           "untagged, an unrevoked one tagged");
	return true;
}

namespace
{
	/// test_revocation_sweep_race: slots reloaded while a sweep runs.
	constexpr size_t RaceSlots = 32;
	/// Slots 0..RaceWritten-1 of the revoked array are overwritten with the kept capability during
	/// the sweep, spread over it; the others keep the revoked one, for the sweep to clear.
	constexpr size_t RaceWritten = 16;
	constexpr int    RacePassMax = 20'000;

	struct RaceResult
	{
		uint32_t stale;   ///< revoked slot reloaded with its tag
		uint32_t written; ///< overwritten slot not reloaded as the kept capability
		uint32_t kept;    ///< kept slot not reloaded as the kept capability
	};

	/// One pass: reload every slot. Inlined: it runs with a shadow bit set.
	__attribute__((always_inline)) inline RaceResult race_pass(void *volatile *revoked,
	                                                           void *volatile *kept,
	                                                           uint32_t        writtenMask,
	                                                           void           *keptCap)
	{
		RaceResult r{0, 0, 0};
		for (size_t i = 0; i < RaceSlots; i++)
		{
			uint32_t bit = uint32_t(1) << i;
			void    *v   = revoked[i];
			if (writtenMask & bit)
			{
				if (!__builtin_cheri_equal_exact(v, keptCap))
				{
					r.written |= bit;
				}
			}
			else if (__builtin_cheri_tag_get(v))
			{
				r.stale |= bit;
			}
			if (!__builtin_cheri_equal_exact(kept[i], keptCap))
			{
				r.kept |= bit;
			}
		}
		return r;
	}

	volatile uint32_t raceSink;
} // namespace

/// Loads and stores racing a running sweep (REQ_REV_04, and REQ_REV_07 at the SoC level). In one
/// heap block P: G0 holds nothing (P's own capability, base G0, is never revoked), G1 is the
/// revoked target, G2 the kept one; two arrays of 32 slots follow, one holding a capability to G1,
/// one to G2. Every pass reloads all 64 slots.
///   calibration  one sweep with every bit clear, the core reloading throughout: counts the passes
///                a sweep takes, and the sweep must change no slot
///   race         G1's bit set, one sweep, the core reloading throughout: no reload of a G1 slot is
///                tagged, whether the sweep has passed it or not (the barrier before, the sweep
///                after), and every G2 slot reloads exactly. 16 of the G1 slots are overwritten
///                with the G2 capability, one at a time, spread over the sweep by the calibration's
///                pass count: each reloads exactly as written from then on, and after the sweep
///   after        G1's bit clear: the 16 untouched G1 slots are untagged and otherwise unchanged
///                (the sweep cleared them, and only their tags), the 16 overwritten ones and all
///                G2 slots are exact
/// The race only counts if at least two passes ran with the epoch odd. Fails under barrier_none
/// (stale reloads during the sweep) and tbre_no_inval (G1 slots still tagged after it). No call
/// runs while G1's bit is set.
bool test_revocation_sweep_race()
{
	constexpr size_t G = BytesPerShadowBit;
	Timeout          t{5};
	char            *block = static_cast<char *>(
      heap_allocate(&t, MALLOC_CAPABILITY, 4 * G + 2 * RaceSlots * sizeof(void *)));
	if (!__builtin_cheri_tag_get(block))
	{
		Debug::log("FAIL race: heap_allocate returned an untagged capability");
		return false;
	}
	size_t base = __builtin_cheri_base_get(block);
	if (base < RevokableMemoryStart || base % G != 0)
	{
		Debug::log("FAIL race: allocation at {} is below revokable memory or not "
		           "granule-aligned",
		           base);
		heap_free(MALLOC_CAPABILITY, block);
		return false;
	}
	void *revokedCap     = __builtin_cheri_bounds_set(block + G, G);
	void *revokedCleared = __builtin_cheri_tag_clear(revokedCap);
	void *keptCap        = __builtin_cheri_bounds_set(block + 2 * G, G);
	auto *revoked        = reinterpret_cast<void *volatile *>(block + 4 * G);
	auto *kept           = revoked + RaceSlots;
	for (size_t i = 0; i < 4; i++)
	{
		set_shadow_bit(base + i * G, false);
	}
	for (size_t i = 0; i < RaceSlots; i++)
	{
		revoked[i] = revokedCap;
		kept[i]    = keptCap;
	}
	volatile RevokerInterface *dev = revoker();

	// Calibration: every bit clear, so every slot must stay as stored.
	uint32_t start     = sweep_start(dev);
	int      calPasses = 0;
	// The pass's verdicts mean nothing here (the bit is clear): it runs only so that the passes cost
	// what they do in the race.
	while (calPasses < RacePassMax && !sweep_done(start, dev->epoch))
	{
		RaceResult r = race_pass(revoked, kept, 0, keptCap);
		raceSink     = r.stale ^ r.kept;
		calPasses++;
	}
	bool calDone = sweep_done(start, dev->epoch);
	bool calKept = true;
	for (size_t i = 0; i < RaceSlots; i++)
	{
		calKept &= __builtin_cheri_equal_exact(revoked[i], revokedCap) &&
		           __builtin_cheri_equal_exact(kept[i], keptCap);
	}

	// Race: G1 revoked for the length of one sweep.
	RaceResult bad{0, 0, 0};
	int        badPass     = -1;
	int        passes      = 0;
	int        inSweep     = 0;
	uint32_t   writtenMask = 0;
	size_t     nextWrite   = 0;
	uint32_t   now         = 0;
	bool       done        = false;
	if (calDone && calKept)
	{
		set_shadow_bit(base + G, true);
		start = sweep_start(dev);
		while (passes < RacePassMax)
		{
			now = dev->epoch;
			if (sweep_done(start, now))
			{
				done = true;
				break;
			}
			inSweep += now & 1;
			RaceResult r = race_pass(revoked, kept, writtenMask, keptCap);
			if ((r.stale | r.written | r.kept) && badPass < 0)
			{
				bad     = r;
				badPass = passes;
			}
			if (nextWrite < RaceWritten &&
			    size_t(passes) >= nextWrite * size_t(calPasses) / RaceWritten)
			{
				revoked[nextWrite] = keptCap;
				writtenMask |= uint32_t(1) << nextWrite;
				nextWrite++;
			}
			passes++;
		}
		set_shadow_bit(base + G, false);
	}

	// After: with the bit clear the barrier passes every tag, so these are the slots as the sweep
	// left them.
	uint32_t afterRevoked = 0, afterWritten = 0, afterKept = 0;
	for (size_t i = 0; i < RaceSlots; i++)
	{
		uint32_t bit = uint32_t(1) << i;
		if (writtenMask & bit)
		{
			afterWritten |= __builtin_cheri_equal_exact(revoked[i], keptCap) ? 0 : bit;
		}
		else
		{
			afterRevoked |= __builtin_cheri_equal_exact(revoked[i], revokedCleared) ? 0 : bit;
		}
		afterKept |= __builtin_cheri_equal_exact(kept[i], keptCap) ? 0 : bit;
	}
	heap_free(MALLOC_CAPABILITY, block);

	if (!calDone || !calKept)
	{
		Debug::log("FAIL race: calibration sweep {} (epoch {} -> {})",
		           calDone ? "changed a slot with every bit clear" : "did not finish",
		           start,
		           dev->epoch);
		return false;
	}
	bool passed = true;
	if (!done || inSweep < 2)
	{
		Debug::log("FAIL race: sweep {} ({} passes, {} with the epoch odd; epoch {} -> {})",
		           done ? "finished before the core raced it" : "did not finish",
		           passes,
		           inSweep,
		           start,
		           now);
		passed = false;
	}
	if (badPass >= 0)
	{
		Debug::log("FAIL race: pass {} during the sweep: stale revoked {}, overwritten {}, "
		           "kept {} (slot masks)",
		           badPass,
		           bad.stale,
		           bad.written,
		           bad.kept);
		passed = false;
	}
	if (afterRevoked | afterWritten | afterKept)
	{
		Debug::log("FAIL race: after the sweep: revoked slots not cleared (or changed "
		           "otherwise) {}, overwritten {}, kept {} (slot masks)",
		           afterRevoked,
		           afterWritten,
		           afterKept);
		passed = false;
	}
	if (passed)
	{
		Debug::log("PASS race: {} passes over 64 slots during one sweep ({} with the epoch "
		           "odd), {} slots overwritten mid-sweep; no stale tag, sweep exact",
		           passes,
		           inSweep,
		           nextWrite);
	}
	return passed;
}

namespace
{
	/// test_revocation_sweep_progress: one run of the workload is a read-modify-write of every word.
	constexpr size_t   ProgressWords       = 256;
	constexpr int      ProgressRunsMax     = 20'000;
	constexpr int      ProgressIdleRuns    = 4;
	/// How much slower, on average, the workload may run while a sweep runs.
	constexpr uint32_t ProgressSlowdownMax = 4;

	__attribute__((always_inline)) inline uint32_t progress_run(volatile uint32_t *buf)
	{
		uint32_t c0 = uint32_t(rdcycle64());
		for (size_t i = 0; i < ProgressWords; i++)
		{
			buf[i] = buf[i] + uint32_t(i) + 1;
		}
		return uint32_t(rdcycle64()) - c0;
	}
} // namespace

/// The core and the revocation engine share the SRAM; neither may starve the other (REQ_REV_03).
/// A workload of 256 word read-modify-writes in the heap is timed with mcycle, first with no sweep
/// (the fastest of 4 runs), then back to back for the length of one sweep:
///   - the sweep finishes while the core runs the workload without a pause (the engine is not
///     starved by the core)
///   - at least two runs fall inside the sweep, and on average they take no more than 4 times the
///     idle run (the core is not starved by the engine). The bound catches starvation, not a
///     performance target; the cycle counts are reported.
///   - every word ends at its expected value (neither side's traffic corrupted the other's)
bool test_revocation_sweep_progress()
{
	Timeout t{5};
	auto   *buf = static_cast<volatile uint32_t *>(
      heap_allocate(&t, MALLOC_CAPABILITY, ProgressWords * sizeof(uint32_t)));
	if (!__builtin_cheri_tag_get(const_cast<uint32_t *>(buf)))
	{
		Debug::log("FAIL progress: heap_allocate returned an untagged capability");
		return false;
	}
	for (size_t i = 0; i < ProgressWords; i++)
	{
		buf[i] = 0;
	}
	uint32_t idle = ~uint32_t(0);
	for (int i = 0; i < ProgressIdleRuns; i++)
	{
		uint32_t c = progress_run(buf);
		idle       = c < idle ? c : idle;
	}
	int runs = ProgressIdleRuns;

	volatile RevokerInterface *dev    = revoker();
	uint32_t                   start  = sweep_start(dev);
	uint32_t                   now    = start;
	bool                       done   = false;
	int                        during = 0;
	uint64_t                   sum    = 0;
	uint32_t                   worst  = 0;
	while (runs < ProgressIdleRuns + ProgressRunsMax)
	{
		now = dev->epoch;
		if (sweep_done(start, now))
		{
			done = true;
			break;
		}
		uint32_t c = progress_run(buf);
		runs++;
		if (now & 1)
		{
			during++;
			sum += c;
			worst = c > worst ? c : worst;
		}
	}
	size_t badWord = ProgressWords;
	for (size_t i = 0; i < ProgressWords && badWord == ProgressWords; i++)
	{
		if (buf[i] != uint32_t(runs) * uint32_t(i + 1))
		{
			badWord = i;
		}
	}
	heap_free(MALLOC_CAPABILITY, const_cast<uint32_t *>(buf));

	uint32_t average = during ? uint32_t(sum / uint64_t(during)) : 0;
	bool     passed  = true;
	if (!done || during < 2)
	{
		Debug::log("FAIL progress: sweep {} while the core ran ({} runs, {} with the epoch odd; "
		           "epoch {} -> {})",
		           done ? "finished too soon to measure" : "did not finish",
		           runs - ProgressIdleRuns,
		           during,
		           start,
		           now);
		passed = false;
	}
	else if (average > ProgressSlowdownMax * idle)
	{
		Debug::log("FAIL progress: the workload took {} cycles on average during the sweep, "
		           "over {} times its {} idle",
		           average,
		           ProgressSlowdownMax,
		           idle);
		passed = false;
	}
	if (badWord != ProgressWords)
	{
		Debug::log("FAIL progress: word {} corrupted after {} runs", badWord, runs);
		passed = false;
	}
	if (passed)
	{
		Debug::log("PASS progress: workload {} cycles idle, {} average and {} worst over {} runs "
		           "during one sweep (bound {}x average); sweep finished, data intact",
		           idle,
		           average,
		           worst,
		           during,
		           ProgressSlowdownMax);
	}
	return passed;
}

/// Thread entry point. Prints the strings test_runner.py greps for.
///
/// CHERIoT RTOS threads must not return, so this loops forever after printing
/// the result -- same shape as juliet_orchestrator's run_juliet_tests. The
/// __cheri_compartment attribute (not extern "C") is what makes the exported
/// symbol name match what the firmware linker script expects.
[[noreturn]] void __cheri_compartment("revocation_test") run_revocation_tests()
{
	Debug::log("Starting hardware load-barrier test");

	bool barrierOk = test_revocation_barrier();
	bool sweepOk   = test_revocation_sweep() != SweepResult::Fail;
	bool randomOk  = test_revocation_random();
	bool baseOk    = test_revocation_base_not_address();
	bool preemptOk = test_revocation_preempt();
	bool raceOk    = test_revocation_sweep_race();
	bool progOk    = test_revocation_sweep_progress();
	if (barrierOk && sweepOk && randomOk && baseOk && preemptOk && raceOk && progOk)
	{
		Debug::log("All tests finished");
	}
	else
	{
		Debug::log("Test(s) Failed");
	}
	sim_exit();

	while (true)
	{
		Timeout t{1000};
		thread_sleep(&t);
	}
}
