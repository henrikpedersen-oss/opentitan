// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_SHA2_ENGINE_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_SHA2_ENGINE_H_

/**
 * @file
 * @brief SHA-2 256/384/512 and HMAC on the `hmac` block, programmed through its registers as the
 * HMAC programmer's guide describes (the DIF covers SHA-2 256 with a 256-bit key only).
 *
 * Byte order, from hmac.hjson with every swap off: the message FIFO takes words little-endian
 * (the first message byte in bits 7:0); KEY_0 holds the first four key bytes big-endian; DIGEST_0
 * holds the first four digest bytes big-endian.
 *
 * A key shorter than a supported length (128, 256, 384, 512, 1024 bits, at most the block) is
 * zero-padded at its end; a longer one is hashed first, on the engine, as HMAC defines.
 */

#include <stdbool.h>
#include <stdint.h>

/** Initialises the driver's register handle. */
void eg2_nist_sha2_init(void);

/**
 * Hashes `msg` (SHA-2 with `bits` 256, 384 or 512), or computes HMAC with it if `key` is not
 * NULL; writes bits/8 bytes to `out`. The message goes to the FIFO in bursts of a random number
 * of words (1 to `max_burst`), waiting for room between them. Returns false (after printing why)
 * on an engine error or a timeout.
 */
bool eg2_nist_sha2_run(uint32_t bits, const uint8_t *key, uint32_t key_len, const uint8_t *msg,
                       uint32_t msg_len, uint32_t max_burst, uint8_t *out);

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_NIST_EG2_NIST_SHA2_ENGINE_H_
