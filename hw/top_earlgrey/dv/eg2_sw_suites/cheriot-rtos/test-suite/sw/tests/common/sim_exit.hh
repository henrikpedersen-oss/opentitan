// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// End a simulation of the CHERIoT RTOS test SoC from a test compartment.
//
// The Sonata testbenches' uartdpi stops the simulator when it sees this magic string on UART0
// (EXIT_STRING in dv/tb/cheriot_rtos_tb.sv). The RTOS's own simulation_exit() sends it only in
// SIMULATION builds, which the sonata-1.1 board is not -- and switching the board changes the
// whole firmware -- so a test built for sonata-1.1 that ends in an idle loop never ends the run:
// it simulates until the testbench's MAX_CYCLES, days later. Call sim_exit() after printing the
// verdict. Same string and method as the scheduler's platform_simulation_exit
// (cheriot-rtos/sdk/include/platform/sunburst/platform-simulation_exit.hh). On hardware it is
// just a line of text.
//
// Header-only so each compartment that calls it imports the UART itself.

#pragma once

#include <compartment.h>
#include <platform-uart.hh>

static inline void sim_exit()
{
#if DEVICE_EXISTS(uart0)
	auto uart = MMIO_CAPABILITY(Uart, uart0);
#else
	auto uart = MMIO_CAPABILITY(Uart, uart);
#endif
	const char *magicString = "Safe to exit simulator.\xd8\xaf\xfb\xa0\xc7\xe1\xa9\xd7";
	while (char ch = *magicString++)
	{
		uart->blocking_write(ch);
	}
}
