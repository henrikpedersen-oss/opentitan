// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_RUNTIME_LOG_H_
#define OPENTITAN_SW_DEVICE_LIB_RUNTIME_LOG_H_

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/macros.h"

/**
 * @file
 * @brief Generic logging APIs
 *
 * The logging APIs below take a format string with a variable number of
 * arguments for the type specifiers. The APIs are designed to provide a way
 * for attaching the log severity, file name, and line number
 * information along with the message to provide an easier path to debug.
 * These parameters form a log prefix, which is prepended to the actual
 * message being printed. The following is a brief description of these:
 *
 *  log_type:     Severity of the message: info, warning, error or fatal
 *
 *  file name:    Name of the file using __FILE__
 *
 *  line number:  Line where the print originated using __LINE__
 *
 * Log macros support OpenTitan formatting specifiers; see print.h for
 * details the subset of C specifier syntax supported.
 *
 * The precise mechanism for logging is dependent on the target device. On core
 * devices, like Verilator, logs are printed using whatever `stdout` is set to
 * in print.h. DV testbenches may use an alternative, more efficient mechanism.
 *
 * In DV mode, some format specifiers may be unsupported, such as %s.
 */

/**
 * Log severities available.
 *
 * Additional log severities can be added as necessary.
 */
typedef enum log_severity {
  kLogSeverityInfo,
  kLogSeverityWarn,
  kLogSeverityError,
  kLogSeverityFatal,
} log_severity_t;

/**
 * Represents log metadata used to format a log line.
 *
 * Any modification to this struct must be made with caution due to external
 * assumptions. A post-processing script parses the ELF file and extracts the
 * log fields. The said script uses 20-byte size as the delimiter to collect the
 * log fields. Any changes to this struct must be accompanied with the updates
 * to the script, located here:
 * util/device_sw_utils/extract_sw_logs.py.
 */
typedef struct log_fields {
  /**
   * Indicates the severity of the LOG.
   */
  log_severity_t severity;
  /**
   * Name of the file at which a LOG line occurs, e.g. `__FILE__`. There
   * are no requirements for this string, other than that it be some kind of
   * UNIX-like pathname.
   */
  const char *file_name;
  /**
   * Indicates the line number at which the LOG line occurs, e.g., `__LINE__`.
   */
  uint32_t line;
  /**
   * Indicates the number of arguments passed to the format string.
   *
   * This value used only in DV mode, and is ignored by non-DV logging.
   */
  uint32_t nargs;
  /**
   * The actual format string.
   */
  const char *format;
} log_fields_t;

// Internal functions exposed only for access by macros. Their
// real doxygen can be found in log.c.
/**
 * Implementation detail.
 */
void base_log_internal_core(const log_fields_t *log, ...);
/**
 * Implementation detail.
 */
void base_log_internal_dv(const log_fields_t *log, uint32_t nargs, ...);

extern char _dv_log_offset[];

#ifdef __CHERIOT__
/**
 * `log_fields_t` for the DV log of a CHERIoT (purecap) build: the same 20-byte
 * layout, with the two strings as 32-bit addresses rather than pointers (which
 * are 8-byte capabilities in purecap). See `LOG` below.
 */
typedef struct log_fields_dv {
  uint32_t severity;
  uint32_t file_name;
  uint32_t line;
  uint32_t nargs;
  uint32_t format;
} log_fields_dv_t;

/**
 * Implementation detail of the CHERIoT `LOG` (DV path): writes the address of
 * a log line's `log_fields_dv_t`, offset by `_dv_log_offset`.
 */
void base_log_internal_dv_cheriot(uint32_t log_fields_addr);
/**
 * Implementation detail of the CHERIoT `LOG` (DV path): writes one argument.
 */
void base_log_internal_dv_word_cheriot(uint32_t word);
/**
 * Implementation detail of the CHERIoT `LOG` (console path).
 */
void base_log_internal_cheriot(log_severity_t severity, const char *file_name,
                               uint32_t line, const char *format, ...);
#endif  // __CHERIOT__

/**
 * A macro that wraps the `OT_FAIL_IF_64_BIT` macro, providing the name
 * of the LOG macro for better error messages.
 *
 * @param arg an arg/expression to check
 */
#define OT_FAIL_IF_64_BIT_LOG(arg) OT_FAIL_IF_64_BIT(arg, LOG)

/**
 * A macro that checks the variable arguments of the `LOG` function are valid
 * at compile time, by asserting that each of the argument is not a standard
 * C integer type with a width of 64 bits. Any such invalid argument will
 * cause a relevant error via a static assertion.
 *
 * @param ... the variable args list
 */
#define OT_CHECK_VALID_LOG_ARGS(...) \
  OT_VA_FOR_EACH(OT_FAIL_IF_64_BIT_LOG, ##__VA_ARGS__)

/**
 * Basic logging macro that all other logging macros delegate to.
 *
 * Prefer to use a LOG function with a specified severity, instead.
 *
 * @param severity a severity of type `log_severity_t`.
 * @param format a format string, as described in print.h. This must be a
 *               string literal.
 * @param ... format parameters matching the format string.
 *
 * In a CHERIoT (purecap) build (`__CHERIOT__`) the two paths are the same as
 * above, but no log line keeps a pointer in a global: a pointer there is an
 * 8-byte capability in the image's globals (CGP-relative, which must stay under
 * 4 KiB) with a capability relocation the start-up code has to apply.
 *  - DV (`device_log_bypass_uart_address() != 0`): the record in `.logs.fields`
 *    is a `log_fields_dv_t`, the 20-byte layout of `log_fields_t` with 32-bit
 *    addresses, so `extract_sw_logs.py`, the chip bench's `sw_logger_if` and the
 *    EG2 model decode it unchanged. Its address is a 32-bit constant in
 *    `.rodata`; each argument is written as one 32-bit word (a pointer as its
 *    address), as `base_log_internal_dv` does in RISC-V mode.
 *  - Otherwise: the file name and the format string are passed as arguments
 *    (capabilities derived from PCC at run time) to `base_log_internal_cheriot`.
 */
#ifdef __CHERIOT__
#define LOG(severity, format, ...)                                           \
  do {                                                                       \
    OT_CHECK_VALID_LOG_ARGS(__VA_ARGS__);                                    \
    if (device_log_bypass_uart_address() != 0) {                             \
      __attribute__((section(                                                \
          ".logs.fields"))) static const log_fields_dv_t kLogFields = {      \
          /* severity, file_name, line, nargs, format (positional: the */    \
          /* macro's parameters have the fields' names) */                   \
          (severity),                                                        \
          (uint32_t)(uintptr_t)("" __FILE__ ""),                             \
          __LINE__,                                                          \
          OT_VA_ARGS_COUNT(format, ##__VA_ARGS__),                           \
          (uint32_t)(uintptr_t)(format),                                     \
      };                                                                     \
      /* Read from .rodata: folded, the compiler would form the address */  \
      /* from CGP, and .logs.fields is not among the globals. */             \
      static const volatile uint32_t kLogFieldsAddr =                        \
          (uint32_t)(uintptr_t)&kLogFields;                                  \
      base_log_internal_dv_cheriot(kLogFieldsAddr);                          \
      OT_VA_FOR_EACH(LOG_DV_ARG_CHERIOT_, ##__VA_ARGS__);                    \
    } else {                                                                 \
      base_log_internal_cheriot((severity), "" __FILE__ "", __LINE__,        \
                                (format), ##__VA_ARGS__);                    \
    }                                                                        \
  } while (false)

/**
 * Implementation detail of the CHERIoT `LOG`: one argument as the 32-bit word
 * the DV log carries (a pointer's address, a small integer widened with its
 * sign, the first word of anything else, such as a `status_t`).
 */
#define LOG_DV_ARG_CHERIOT_(arg)                                          \
  do {                                                                    \
    __auto_type log_arg_ = (arg);                                         \
    uint32_t log_word_ = 0;                                               \
    if (__builtin_classify_type(log_arg_) == 5 /* pointer */) {           \
      log_word_ = (uint32_t)__builtin_cheri_address_get(                  \
          *(const void *const *)&log_arg_);                               \
    } else if (sizeof(log_arg_) >= sizeof(uint32_t)) {                    \
      __builtin_memcpy(&log_word_, &log_arg_, sizeof(uint32_t));          \
    } else if (sizeof(log_arg_) == sizeof(uint16_t)) {                    \
      uint16_t log_half_;                                                 \
      __builtin_memcpy(&log_half_, &log_arg_, sizeof(log_half_));         \
      log_word_ = _Generic(log_arg_, short: 1, default: 0)                \
                      ? (uint32_t)(int32_t)(int16_t)log_half_             \
                      : log_half_;                                        \
    } else {                                                              \
      uint8_t log_byte_;                                                  \
      __builtin_memcpy(&log_byte_, &log_arg_, sizeof(log_byte_));         \
      log_word_ = _Generic(log_arg_, signed char: 1,                      \
                           char: ((char)-1 < 0), default: 0)              \
                      ? (uint32_t)(int32_t)(int8_t)log_byte_              \
                      : log_byte_;                                        \
    }                                                                     \
    base_log_internal_dv_word_cheriot(log_word_);                         \
  } while (false)
#else
#define LOG(severity, format, ...)                                             \
  do {                                                                         \
    OT_CHECK_VALID_LOG_ARGS(__VA_ARGS__);                                      \
    if (device_log_bypass_uart_address() != 0) {                               \
      /* Put DV-only log constants in .logs.* sections, which                  \
       * the linker will dutifully discard. */                                 \
      __attribute__((                                                          \
          section(".logs.fields"))) static const log_fields_t kLogFields =     \
          LOG_MAKE_FIELDS_(severity, format, ##__VA_ARGS__);                   \
      base_log_internal_dv((const log_fields_t *)((char *)&kLogFields +        \
                                                  (uintptr_t)&_dv_log_offset), \
                           OT_VA_ARGS_COUNT(format, ##__VA_ARGS__),            \
                           ##__VA_ARGS__);                                     \
    } else {                                                                   \
      static const log_fields_t log_fields =                                   \
          LOG_MAKE_FIELDS_(severity, format, ##__VA_ARGS__);                   \
      base_log_internal_core(&log_fields, ##__VA_ARGS__);                      \
    }                                                                          \
  } while (false)
#endif  // __CHERIOT__

/**
 * Implementation detail of `LOG`.
 */
#define LOG_MAKE_FIELDS_(_severity, _format, ...)        \
  {                                                      \
      .severity = _severity,                             \
      .file_name = "" __FILE__ "",                       \
      .line = __LINE__,                                  \
      .nargs = OT_VA_ARGS_COUNT(_format, ##__VA_ARGS__), \
      .format = _format,                                 \
  }

/**
 * Log an informational message.
 *
 * @param severity a severity of type `log_severity_t`.
 * @param format a format string, as described in print.h. This must be a string
 * literal.
 * @param ... format parameters matching the format string.
 */
#define LOG_INFO(...) LOG(kLogSeverityInfo, __VA_ARGS__)

/**
 * Log a warning
 *
 * @param severity a severity of type `log_severity_t`.
 * @param format a format string, as described in print.h. This must be a string
 * literal.
 * @param ... format parameters matching the format string.
 */
#define LOG_WARNING(...) LOG(kLogSeverityWarn, __VA_ARGS__)

/**
 * Log a non-fatal error.
 *
 * @param severity a severity of type `log_severity_t`.
 * @param format a format string, as described in print.h. This must be a string
 * literal.
 * @param ... format parameters matching the format string.
 */
#define LOG_ERROR(...) LOG(kLogSeverityError, __VA_ARGS__)

/**
 * Log a fatal error.
 *
 * @param severity a severity of type `log_severity_t`.
 * @param format a format string, as described in print.h. This must be a string
 * literal.
 * @param ... format parameters matching the format string.
 *
 * It is the user's responsibility to follow this up with a call to `abort()` to
 * immediately stop the execution.
 */
#define LOG_FATAL(...) LOG(kLogSeverityFatal, __VA_ARGS__)

#endif  // OPENTITAN_SW_DEVICE_LIB_RUNTIME_LOG_H_
