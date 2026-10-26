// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_OTBN_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_OTBN_H_

/**
 * @file
 * @brief ECDSA P-256 on OTBN for the post-boot OTBN tests, through the crypto library's driver
 * (sw/device/lib/crypto/impl/ecc/p256.c) and its OTBN program (run_p256), in the formats the
 * OTBN reference checker uses (eg2_model/nist/nist_otbn_check.py): little-endian 32-bit words,
 * the scalars d and k in arithmetic shares mod n, a signature valid only if the driver's `ok`
 * and x_r == r hold (p256_ecdsa_verify_finalize).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

/** OTBN STATUS values (otbn.hjson). */
enum {
  kEg2PbfOtbnIdle = 0x00,
  kEg2PbfOtbnBusyExecute = 0x01,
  kEg2PbfOtbnLocked = 0xff,
};

/** OTBN's STATUS, ERR_BITS, FATAL_ALERT_CAUSE and one DMEM word. */
uint32_t eg2_pbf_otbn_status(void);
uint32_t eg2_pbf_otbn_err_bits(void);
uint32_t eg2_pbf_otbn_fatal_cause(void);
uint32_t eg2_pbf_otbn_dmem_word(uint32_t index);

/** Waits until STATUS is not `busy` (`busy` itself, e.g. BUSY_EXECUTE); false on a timeout. */
bool eg2_pbf_otbn_wait_not(uint32_t busy, uint32_t polls);

/** Starts verifying vector `v` of kEg2PbfKatEcdsa; `corrupt` flips bit 0 of S first. */
bool eg2_pbf_otbn_verify_start(const eg2_pbf_kat_ecdsa_t *v, bool corrupt);

/** Ends the verification; *valid is the driver's verdict. False on a driver error. */
bool eg2_pbf_otbn_verify_end(bool *valid);

/** Signs with the vector's d and k (MODE_SIGN_CONFIG_K); r || s big-endian, 64 bytes. */
bool eg2_pbf_otbn_sign(const eg2_pbf_kat_ecdsa_t *v, uint8_t rs[64]);

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_OTBN_H_
