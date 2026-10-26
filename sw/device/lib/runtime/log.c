// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/runtime/log.h"

#include <assert.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/runtime/print.h"

/**
 * Ensure that log_fields_t is always 20 bytes.
 *
 * The assertion below helps prevent inadvertant changes to the struct.
 * Please see the description of log_fields_t in log.h for more details.
 */
#ifndef __CHERIOT__
// In a CHERIoT build the pointers are 8-byte capabilities; its DV bypass uses
// log_fields_dv_t (log.h).
static_assert(sizeof(log_fields_t) == 20,
              "log_fields_t must always be 20 bytes.");
#endif

/**
 * Converts a severity to a static string.
 */
static const char *stringify_severity(log_severity_t severity) {
  switch (severity) {
    case kLogSeverityInfo:
      return "I";
    case kLogSeverityWarn:
      return "W";
    case kLogSeverityError:
      return "E";
    case kLogSeverityFatal:
      return "F";
    default:
      return "?";
  }
}

/**
 * Logs `log` and the values that follow to stdout.
 *
 * @param log the log data to log.
 * @param ... format parameters matching the format string.
 */
void base_log_internal_core(const log_fields_t *log, ...) {
  size_t file_name_len =
      (size_t)(((const char *)memchr(log->file_name, '\0', PTRDIFF_MAX)) -
               log->file_name);
  const char *base_name = memrchr(log->file_name, '/', file_name_len);
  if (base_name == NULL) {
    base_name = log->file_name;
  } else {
    ++base_name;  // Remove the final '/'.
  }

  // A small global counter that increments with each log line. This can be
  // useful for seeing how many times this function has been called, even if
  // nothing was printed for some time.
  static uint16_t global_log_counter = 0;

  base_printf("%s%05d %s:%d] ", stringify_severity(log->severity),
              global_log_counter, base_name, log->line);
  ++global_log_counter;

  va_list args;
  va_start(args, log);
  base_vprintf(log->format, args);
  va_end(args);

  base_printf("\r\n");
}

/**
 * Logs `log` and the values that follow in an efficient, DV-testbench
 * specific way, which bypasses the UART.
 *
 * @param log a pointer to log data to log. Note that this pointer is likely to
 *        be invalid at runtime, since the pointed-to data will have been
 *        stripped from the binary.
 * @param nargs the number of arguments passed to the format string.
 * @param ... format parameters matching the format string.
 */
void base_log_internal_dv(const log_fields_t *log, uint32_t nargs, ...) {
  mmio_region_t log_device =
      mmio_region_from_addr(device_log_bypass_uart_address());
  mmio_region_write32(log_device, 0x0, (uintptr_t)log);

  va_list args;
  va_start(args, nargs);
  for (int i = 0; i < nargs; ++i) {
    mmio_region_write32(log_device, 0x0, va_arg(args, uint32_t));
  }
  va_end(args);
}

#ifdef __CHERIOT__
// The CHERIoT (purecap) `LOG` (log.h): no pointer is kept in a global.

static_assert(sizeof(log_fields_dv_t) == 20,
              "log_fields_dv_t must be 20 bytes, as log_fields_t.");

void base_log_internal_dv_cheriot(uint32_t log_fields_addr) {
  // `_dv_log_offset` is an absolute symbol of the linker script: its value as
  // an integer, so no capability is formed to it.
  uint32_t dv_log_offset;
  asm("lui %0, %%hi(_dv_log_offset)\n"
      "addi %0, %0, %%lo(_dv_log_offset)"
      : "=r"(dv_log_offset));
  mmio_region_t log_device =
      mmio_region_from_addr(device_log_bypass_uart_address());
  mmio_region_write32(log_device, 0x0, log_fields_addr + dv_log_offset);
}

void base_log_internal_dv_word_cheriot(uint32_t word) {
  mmio_region_t log_device =
      mmio_region_from_addr(device_log_bypass_uart_address());
  mmio_region_write32(log_device, 0x0, word);
}

void base_log_internal_cheriot(log_severity_t severity, const char *file_name,
                               uint32_t line, const char *format, ...) {
  // Byte by byte, not memchr(file_name, '\0', PTRDIFF_MAX): memchr reads whole words, and the last
  // one runs past the bounds of the string's capability (a bounds fault in every LOG through the
  // console, regr-vlt 2026-10-25: eg2_ottf_smoke_cheriot hung on its first LOG).
  size_t file_name_len = 0;
  while (file_name[file_name_len] != '\0') {
    ++file_name_len;
  }
  const char *base_name = memrchr(file_name, '/', file_name_len);
  if (base_name == NULL) {
    base_name = file_name;
  } else {
    ++base_name;  // Remove the final '/'.
  }

  static uint16_t global_log_counter = 0;

  base_printf("%s%05d %s:%d] ", stringify_severity(severity),
              global_log_counter, base_name, line);
  ++global_log_counter;

  va_list args;
  va_start(args, format);
  base_vprintf(format, args);
  va_end(args);

  base_printf("\r\n");
}
#endif  // __CHERIOT__
