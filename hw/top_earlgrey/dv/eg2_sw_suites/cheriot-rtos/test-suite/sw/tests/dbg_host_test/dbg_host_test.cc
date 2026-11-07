// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Directed test for the debug host: the debug module's system bus access (SBA), a bus master that
// is not the core. Testplan non_core_bus_masters (ibex/dv/cheriot-rtos-test-suite/data/cheriot-rtos_testplan.hjson):
//
//   bus_master_dbg_host_tag_clear  With debug access enabled, the debug host writes over a word
//                                  that holds a tagged capability. A core reload must find the
//                                  written address with the tag unchanged: SBA bypasses the tag
//                                  filter and does not clear tags (rv_dm theory of operation,
//                                  "CHERIoT Mode"; earlgrey-internal-tracker #971 and #1040,
//                                  closed as documented). The debugger is trusted by design.
//   bus_master_dbg_host_locked     With debug access disabled, the same write must not reach the
//                                  SRAM: the capability reloads unchanged, tag included, and the
//                                  debug module reports a bus error (sbcs.sberror = 2) for it.
//
// The debug host is driven by the testbench (tb/cheriot_rtos_dbg_host.sv), on request: this
// firmware prints
//   DBGHOST REQ <op> <slot address> <value> <flag address>
// on UART0 and polls `doneFlag`. The testbench writes <value> into the 32-bit word at <slot>
// over SBA (op 1: debug access enabled, op 2: disabled), reads it back, and writes the flag word
// over SBA with debug access enabled:
//   DoneMagic | op << 12 | sberror << 8 | readback << 4 | 1
// The checks are this firmware's, made with core loads: only a capability load shows the tag.
//
// The slot holds a capability to the global array `target`, and <value> is that capability's
// address plus 16: inside its bounds, so the capability's base, and with it the revocation bit the
// load barrier looks up on the reload, are the same as before the write. Without that, a
// capability whose base moved onto a revoked granule would come back untagged because of the
// load barrier, and the tag check would fail for the wrong reason.
//
// If no testbench answers (another SoC, or +dbg_host_drv=0), both tests report FAIL.

#include <compartment.h>
#include <debug.hh>
#include "../common/sim_exit.hh"
#include <thread.h>

using Debug = ConditionalDebug<true, "Debug host">;

namespace
{
	// Shared with tb/cheriot_rtos_dbg_host.sv.
	constexpr uint32_t OpTagClear    = 1;
	constexpr uint32_t OpLocked      = 2;
	constexpr uint32_t DoneMagic     = 0xdb60'0000;
	constexpr uint32_t DoneMagicMask = 0xffff'0000;
	/// sbcs.sberror for a bus error: what the SoC's debug-access gate answers.
	constexpr uint32_t SbErrorBus = 2;

	/// The RTOS test SoC's SRAM (board data_memory and heap lie in it): the testpoint is about
	/// the debug host writing SRAM.
	constexpr uint32_t SramBase = 0x0010'0000;
	constexpr uint32_t SramTop  = 0x0012'0000;

	/// Polls of doneFlag, one scheduler tick (400k cycles) apart: the testbench answers within
	/// a few thousand cycles, so this only bounds a run nobody answers.
	constexpr int MaxPolls = 20;

	/// The capability slots the debug host writes over, and the flag it writes when done.
	/// volatile so every access is a real load or store.
	void *volatile    tagClearSlot;
	void *volatile    lockedSlot;
	volatile uint32_t doneFlag;

	/// What the stored capabilities point to.
	uint64_t target[8];

	uint32_t address_of(const volatile void *p)
	{
		return static_cast<uint32_t>(__builtin_cheri_address_get(p));
	}

	/// Ask the testbench to serve `op` on `slot`, and wait for its flag. Returns the flag, or 0
	/// if nobody answered.
	uint32_t request(uint32_t op, void *volatile *slot, uint32_t value)
	{
		doneFlag = 0;
		Debug::log("DBGHOST REQ {} {} {} {}",
		           op,
		           address_of(slot),
		           value,
		           address_of(&doneFlag));
		for (int i = 0; i < MaxPolls; i++)
		{
			Timeout t{1};
			// NoEarlyWake: otherwise, with no other thread runnable, the sleep returns at once
			// and the polls are over in a few thousand cycles (revocation_test.cc).
			thread_sleep(&t, ThreadSleepNoEarlyWake);
			uint32_t flag = doneFlag;
			if ((flag & DoneMagicMask) == DoneMagic && ((flag >> 12) & 0xf) == op)
			{
				return flag;
			}
		}
		return 0;
	}

	/// Store a tagged capability to `target[index]` in `slot` and check it reloads tagged.
	bool prepare(const char *test, void *volatile *slot, size_t index, void *&stored)
	{
		uint32_t slotAddress = address_of(slot);
		if (slotAddress < SramBase || slotAddress + 8 > SramTop ||
		    address_of(&doneFlag) < SramBase || address_of(&doneFlag) + 4 > SramTop)
		{
			Debug::log("FAIL {}: slot {} or flag {} is not in the SRAM",
			           test,
			           slotAddress,
			           address_of(&doneFlag));
			return false;
		}
		stored  = &target[index];
		*slot   = stored;
		void *r = *slot;
		if (!__builtin_cheri_tag_get(stored) || !__builtin_cheri_equal_exact(r, stored))
		{
			Debug::log("FAIL {}: precondition: the stored capability {} reloads as {}",
			           test,
			           stored,
			           r);
			return false;
		}
		return true;
	}
} // namespace

/// bus_master_dbg_host_tag_clear
bool test_tag_clear()
{
	void *stored;
	if (!prepare("tag clear", &tagClearSlot, 0, stored))
	{
		return false;
	}
	uint32_t value = address_of(stored) + 16;
	uint32_t flag  = request(OpTagClear, &tagClearSlot, value);
	void    *after = tagClearSlot;
	if (flag == 0)
	{
		Debug::log("FAIL tag clear: no answer from the debug host (no debug module "
		           "driver in this testbench?)");
		return false;
	}
	// What the slot must hold: the stored capability with the address the debug host wrote and
	// every other bit unchanged, tag included.
	void *expected = __builtin_cheri_address_set(stored, value);
	uint32_t sberror = (flag >> 8) & 0x7;
	if (sberror != 0 || address_of(after) != value)
	{
		Debug::log("FAIL tag clear: the debug host's write did not reach the slot "
		           "(sberror {}, reloaded {}, expected address {})",
		           sberror,
		           after,
		           value);
		return false;
	}
	if (!__builtin_cheri_tag_get(after))
	{
		Debug::log("FAIL tag clear: the debug host's write cleared the tag: {} is "
		           "untagged, but SBA must leave tags unchanged (rv_dm \"CHERIoT Mode\")",
		           after);
		return false;
	}
	if (!__builtin_cheri_equal_exact(after, expected))
	{
		Debug::log("FAIL tag clear: the reloaded word {} differs from {} beyond the "
		           "address the debug host wrote",
		           after,
		           expected);
		return false;
	}
	Debug::log("PASS tag clear: the debug host's write over a tagged capability "
	           "changed its address and left the tag unchanged ({})",
	           after);
	return true;
}

/// bus_master_dbg_host_locked
bool test_locked()
{
	void *stored;
	if (!prepare("locked", &lockedSlot, 2, stored))
	{
		return false;
	}
	uint32_t value = address_of(stored) + 16;
	uint32_t flag  = request(OpLocked, &lockedSlot, value);
	void    *after = lockedSlot;
	if (flag == 0)
	{
		Debug::log("FAIL locked: no answer from the debug host (no debug module driver "
		           "in this testbench?)");
		return false;
	}
	uint32_t sberror = (flag >> 8) & 0x7;
	if (!__builtin_cheri_equal_exact(after, stored))
	{
		Debug::log("FAIL locked: the debug host wrote the SRAM while debug access was "
		           "disabled: the slot holds {}, was {} (sberror {})",
		           after,
		           stored,
		           sberror);
		return false;
	}
	if (sberror != SbErrorBus)
	{
		Debug::log("FAIL locked: the refused write was answered with sberror {}, "
		           "not {} (bus error from the debug-access gate)",
		           sberror,
		           SbErrorBus);
		return false;
	}
	Debug::log("PASS locked: with debug access disabled the debug host's write was "
	           "refused (sberror {}) and the capability reloads unchanged, tag included",
	           sberror);
	return true;
}

/// Thread entry point. Prints the verdict lines cheriot_rtos_results.py reads, then ends the run.
[[noreturn]] void __cheri_compartment("dbg_host_test") run_dbg_host_tests()
{
	Debug::log("Starting debug host (system bus access) test");

	bool tagClearOk = test_tag_clear();
	bool lockedOk   = test_locked();
	if (tagClearOk && lockedOk)
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
