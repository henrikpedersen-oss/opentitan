// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0

// Performance test for Earl Grey 2's only stated performance requirement: from power-on reset to
// the end of ROM, including the ML-DSA-87 verification of a 128 KiB ROM_EXT stored in RRAM, in at
// most 100 ms (15M cycles at 150 MHz). The core's share of that is feeding the ROM_EXT image into
// the hash engine's message FIFO; ML-DSA itself runs on OTBN.
//
// The image is a 128 KiB constant, so it links with the code into the code RAM, which the test
// SoC delays by +code_mem_latency=<n> cycles to stand in for RRAM. It is fed to the perf device's
// FIFO (rtl/cheriot_rtos_perf_dev.sv) twice:
//   core: the core loads and stores every word, as Earl Grey 2's Ibex does
//   dma:  the core programs the DMA and sleeps on its completion interrupt, as in a design where
//         an external DMA moves the data
// Each prints "PERF <mode> cycles <n> core_cycles <n> instret <n> words <n>" (cycles: the feed's
// duration; core_cycles: the core's awake time, from mcycle) and checks the FIFO's word count and
// checksum against the image.

#include "../common/sim_exit.hh"
#include <compartment.h>
#include <debug.hh>
#include <futex.h>
#include <interrupt.h>
#include <riscvreg.h>
#include <stdint.h>
#include <thread.h>

using Debug = ConditionalDebug<true, "Perf test">;

DECLARE_AND_DEFINE_INTERRUPT_CAPABILITY(dmaInterrupt, PerfDmaInterrupt, true, true);

namespace
{
	constexpr size_t ImageBytes = 128 * 1024;
	constexpr size_t ImageWords = ImageBytes / sizeof(uint32_t);

	constexpr uint32_t image_word(uint32_t i)
	{
		uint32_t x = i * 0x9e3779b9u + 0x7f4a7c15u;
		x ^= x >> 15;
		x *= 0x2c1b3c6du;
		x ^= x >> 12;
		return x;
	}

	struct Image
	{
		uint32_t words[ImageWords];
	};

	constexpr Image make_image()
	{
		Image image{};
		for (uint32_t i = 0; i < ImageWords; i++)
		{
			image.words[i] = image_word(i);
		}
		return image;
	}

	alignas(16) constexpr Image RomExt = make_image();

	struct PerfDevice
	{
		uint32_t fifo;
		uint32_t count;
		uint32_t checksum;
		uint32_t reserved;
		uint32_t dmaSrc;
		uint32_t dmaDst;
		uint32_t dmaLen;
		uint32_t dmaCtrl;
		uint32_t dmaStatus;
		uint32_t dmaWords;
		uint32_t dmaCycles;
	};

	constexpr uint32_t DmaCtrlStart    = 1u << 0;
	constexpr uint32_t DmaCtrlDstFixed = 1u << 1;
	constexpr uint32_t DmaStatusBusy   = 1u << 0;
	constexpr uint32_t DmaStatusDone   = 1u << 1;
	constexpr uint32_t DmaStatusError  = 1u << 2;

	volatile PerfDevice *perf_device()
	{
		return MMIO_CAPABILITY(PerfDevice, perf);
	}

	uint32_t rdinstret()
	{
		uint32_t value;
		__asm__ volatile("csrr %0, minstret" : "=r"(value));
		return value;
	}

	// The image through a volatile pointer, so the compiler cannot fold the loads of a constant it
	// knows.
	const uint32_t *image()
	{
		const uint32_t *volatile words = RomExt.words;
		return words;
	}

	uint32_t expected_checksum()
	{
		const uint32_t *src = image();
		uint32_t        sum = 0;
		for (size_t i = 0; i < ImageWords; i++)
		{
			sum = ((sum << 1) | (sum >> 31)) ^ src[i];
		}
		return sum;
	}

	// What Earl Grey 2's ROM does with the hash engine's FIFO: four loads, four stores.
	__attribute__((noinline)) void
	feed_core(const uint32_t *src, size_t words, volatile uint32_t *fifo)
	{
		for (size_t i = 0; i < words; i += 4)
		{
			uint32_t a = src[i];
			uint32_t b = src[i + 1];
			uint32_t c = src[i + 2];
			uint32_t d = src[i + 3];
			*fifo      = a;
			*fifo      = b;
			*fifo      = c;
			*fifo      = d;
		}
	}

	struct Sample
	{
		uint64_t cycles;
		uint32_t instret;
	};

	Sample sample()
	{
		return {rdcycle64(), rdinstret()};
	}

	bool check_fifo(const char *mode, uint32_t expectedSum)
	{
		volatile PerfDevice *dev   = perf_device();
		uint32_t             count = dev->count;
		uint32_t             sum   = dev->checksum;
		if (count != ImageWords || sum != expectedSum)
		{
			Debug::log("FAIL {}: FIFO got {} words checksum {}, expected {} words checksum {}",
			           mode,
			           static_cast<int32_t>(count),
			           sum,
			           static_cast<int32_t>(ImageWords),
			           expectedSum);
			return false;
		}
		return true;
	}

	// cycles: how long the feed took. core_cycles: mcycle, which stops while the core sleeps (Ibex
	// gates its clock in wfi), so for the DMA it is the core's awake time only.
	void report(const char *mode,
	            uint64_t    cycles,
	            Sample      start,
	            Sample      end,
	            uint32_t    words)
	{
		Debug::log("PERF {} cycles {} core_cycles {} instret {} words {}",
		           mode,
		           static_cast<int64_t>(cycles),
		           static_cast<int64_t>(end.cycles - start.cycles),
		           static_cast<int32_t>(end.instret - start.instret),
		           static_cast<int32_t>(words));
	}

	bool test_core(uint32_t expectedSum)
	{
		volatile PerfDevice *dev = perf_device();
		dev->count               = 0;
		const uint32_t *src      = image();

		Sample start = sample();
		feed_core(src, ImageWords, &dev->fifo);
		Sample end = sample();

		report("hash_feed_core", end.cycles - start.cycles, start, end, ImageWords);
		return check_fifo("hash_feed_core", expectedSum);
	}

	bool test_dma(uint32_t expectedSum)
	{
		volatile PerfDevice *dev = perf_device();
		dev->count               = 0;
		const uint32_t *irq = interrupt_futex_get(STATIC_SEALED_VALUE(dmaInterrupt));
		uint32_t        seen = *irq;

		Sample start = sample();
		dev->dmaSrc  = __builtin_cheri_address_get(image());
		dev->dmaDst  = __builtin_cheri_address_get(&dev->fifo);
		dev->dmaLen  = ImageBytes;
		dev->dmaCtrl = DmaCtrlStart | DmaCtrlDstFixed;
		while (*irq == seen)
		{
			futex_wait(irq, seen);
		}
		Sample end = sample();

		uint32_t status = dev->dmaStatus;
		uint32_t words  = dev->dmaWords;
		uint32_t cycles = dev->dmaCycles;
		dev->dmaStatus  = DmaStatusDone;
		interrupt_complete(STATIC_SEALED_VALUE(dmaInterrupt));

		report("hash_feed_dma", cycles, start, end, words);
		if ((status & (DmaStatusBusy | DmaStatusError)) != 0 || (status & DmaStatusDone) == 0)
		{
			Debug::log("FAIL hash_feed_dma: DMA status {}", status);
			return false;
		}
		return check_fifo("hash_feed_dma", expectedSum);
	}
} // namespace

[[noreturn]] void __cheri_compartment("perf_test") run_perf_tests()
{
	Debug::log("Starting performance tests: ROM_EXT hash feed, {} bytes",
	           static_cast<int32_t>(ImageBytes));

	uint32_t expectedSum = expected_checksum();
	bool     coreOk      = test_core(expectedSum);
	bool     dmaOk       = test_dma(expectedSum);
	if (coreOk && dmaOk)
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
