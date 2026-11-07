// Copyright Microsoft and CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#ifdef SIMULATION
#	include <stdint.h>
#	include <platform-uart.hh>
#	include <string_view>

static void platform_simulation_exit(uint32_t code)
{
	auto uart =
#	if DEVICE_EXISTS(uart0)
	  MMIO_CAPABILITY(Uart, uart0);
#	elif DEVICE_EXISTS(uart)
	  MMIO_CAPABILITY(Uart, uart);
#	else
#		error No UART found in platform_simulation_exit
#	endif
	// Local change (ibex_cheriot_verification): print the exit code. The magic
	// string below is the same for every code, so without this a scheduler
	// panic or an error handler's exit(1) ends the run exactly like a clean
	// exit(0).
	const char *codeString = "Simulation exit code: ";
	while (char ch = *codeString++)
	{
		uart->blocking_write(ch);
	}
	char digits[10];
	int  digitCount = 0;
	do
	{
		digits[digitCount++] = static_cast<char>('0' + code % 10);
		code /= 10;
	} while (code != 0);
	while (digitCount > 0)
	{
		uart->blocking_write(digits[--digitCount]);
	}
	uart->blocking_write('\n');
	// Writing the following magic string to the UART will cause the sonata
	// simulator to exit.
	const char *magicString =
	  "Safe to exit simulator.\xd8\xaf\xfb\xa0\xc7\xe1\xa9\xd7";
	while (char ch = *magicString++)
	{
		uart->blocking_write(ch);
	}
}
#endif
