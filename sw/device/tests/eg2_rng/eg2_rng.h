// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_RNG_EG2_RNG_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_RNG_EG2_RNG_H_

/**
 * @file
 * @brief What the random-number tests share (eg2_top_verification spec "Random Number Generation
 * Test Suite", REQ_PBF_24, REQ_PBF_12).
 *
 *  - Checks with one log format, `RNG ok <what>` / `RNG FAIL <what> expected .. got ..`; with
 *    `EG2_RNG_INJECT_FAIL` the first `eg2_rng_check` compares against its expected value with bit 0
 *    flipped, so the run must fail with `RNG FAIL` (REQ_STE_03). `RNG gap <what>: <why>` for a
 *    check the documents do not support, `RNG model_gap <what>` for one skipped with
 *    `model_gaps` = 1 (eg2sim only).
 *  - The first check of every test: entropy_src is enabled when the test starts, MODULE_ENABLE
 *    true, as the test ROM leaves it (test_rom_start.S enables entropy_src, csrng and EDN0 for
 *    memory scrambling); every test then stops the complex (eg2_rng_stop_all) before it
 *    configures entropy_src.
 *  - entropy_src through its registers (hw/ip/entropy_src/data/entropy_src.hjson and the
 *    programmer's guide), csrng and edn through their DIFs.
 *  - Bench steps: the software asks the chip bench to act by writing `kEg2RngBenchCode | step` to
 *    the DV status word (rv_core_ibex DV_SIM_WINDOW[0]); the test's vseq acts in the same
 *    simulation step (it selects the noise source's pattern, eg2_rng_select), then the software
 *    writes "in test" back. In eg2sim nobody answers.
 *  - The capture's DV log lines, `EG2_RNG <tap>_<mode> <8 words>`, the input of
 *    scripts/rng_extract.py and `make rng-assess`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EG2_RNG_INJECT_FAIL
#define EG2_RNG_INJECT true
#else
#define EG2_RNG_INJECT false
#endif

enum {
  /** The DV status code of a bench step (the low 16 bits are the bench's sw_test_status). */
  kEg2RngBenchCode = 0x5200u,
  /** Polls of a status bit before a wait gives up. */
  kEg2RngPolls = 2000000,
};

/** entropy_src's modes as the spec names them (and the capture lines' <mode>). */
typedef enum eg2_rng_esmode {
  kEg2RngBoot = 0,    // CONF.FIPS_ENABLE false: boot-time / bypass mode, 384-bit raw seeds
  kEg2RngFips = 1,    // CONF.FIPS_ENABLE true: SHA-3 conditioned seeds of FIPS_WINDOW symbols
  kEg2RngBypass = 2,  // FIPS_ENABLE true, ENTROPY_CONTROL.ES_TYPE true: conditioner bypassed
} eg2_rng_esmode_t;

/** The health tests in their threshold register order (REPCNT_THRESHOLD first, 4 bytes apart). */
typedef enum eg2_rng_ht {
  kEg2RngHtRepcnt = 0,
  kEg2RngHtRepcnts = 1,
  kEg2RngHtAdaptpHi = 2,
  kEg2RngHtAdaptpLo = 3,
  kEg2RngHtAdaptps = 4,
  kEg2RngHtBucket = 5,
  kEg2RngHtMarkovHi = 6,
  kEg2RngHtMarkovLo = 7,
  kEg2RngHtExthtHi = 8,
  kEg2RngHtExthtLo = 9,
  kEg2RngHtCount = 10,
} eg2_rng_ht_t;

/** A health test's name. */
const char *eg2_rng_ht_name(uint32_t ht);

/** Whether the test fails below its threshold (the *_LO tests) rather than above it. */
static inline bool eg2_rng_ht_is_lo(uint32_t ht) {
  return ht == kEg2RngHtAdaptpLo || ht == kEg2RngHtMarkovLo || ht == kEg2RngHtExthtLo;
}

/** entropy_src's configuration for a test. */
typedef struct eg2_rng_es_cfg {
  eg2_rng_esmode_t mode;
  bool route_to_fw;   // ES_ROUTE and ENTROPY_DATA_REG_ENABLE: seeds to ENTROPY_DATA
  bool fw_ov;         // FW_OV_CONTROL.FW_OV_MODE: the observe FIFO
  bool insert;        // FW_OV_CONTROL.FW_OV_ENTROPY_INSERT
  bool rng_fips;      // CONF.RNG_FIPS
  uint16_t fips_window;      // HEALTH_TEST_WINDOWS.FIPS_WINDOW (symbols)
  uint16_t alert_threshold;  // ALERT_THRESHOLD (0: no alert)
  uint16_t thresh[kEg2RngHtCount];
} eg2_rng_es_cfg_t;

/** Starts the test's log: its name, the mode, the inject-fail note. */
void eg2_rng_begin(const char *test, bool inject_fail);

/** `RNG ok|FAIL <what>`; returns ok. */
bool eg2_rng_step(const char *what, bool ok);

/** Compares a value (the first compare of an inject-fail build expects bit 0 flipped). */
bool eg2_rng_check(const char *what, uint32_t expected, uint32_t got);

/** `RNG note <what> 0x<value>`. */
void eg2_rng_note(const char *what, uint32_t value);

/** `RNG gap <what>: <why>`. */
void eg2_rng_gap(const char *what, const char *why);

/** `RNG model_gap <what>`. */
void eg2_rng_model_gap(const char *what);

/** The first check of every test (see the file comment). */
bool eg2_rng_check_reset_state(void);

/** Asks the bench for step `step` (< 0x100). */
void eg2_rng_bench_step(uint32_t step);

/** Stops EDN0, EDN1, csrng and entropy_src, in that order (entropy_testutils_stop_all). */
bool eg2_rng_stop_all(void);

/** Every health test out of reach: high thresholds 0xffff, low thresholds 0 (theory of op.). */
void eg2_rng_thresholds_off(eg2_rng_es_cfg_t *cfg);

/** Configures entropy_src (disabled, registers writable) and clears its failure records. */
void eg2_rng_es_configure(const eg2_rng_es_cfg_t *cfg);

/** MODULE_ENABLE true or false; disabling waits for DEBUG_STATUS.MAIN_SM_IDLE. */
bool eg2_rng_es_enable(bool enable);

/** One entropy_src register. */
uint32_t eg2_rng_es_read(uint32_t offset);
void eg2_rng_es_write(uint32_t offset, uint32_t value);

/** Reads one 384-bit seed from ENTROPY_DATA (12 words); false if none came within `polls`. */
bool eg2_rng_es_read_seed(uint32_t words[12], uint32_t polls);

/** The health test's failure count in ALERT_FAIL_COUNTS / EXTHT_FAIL_COUNTS. */
uint32_t eg2_rng_fail_count(uint32_t ht);

/** Whether MAIN_SM_STATE is AlertState or AlertHang (the DIF's dif_entropy_src_main_fsm_t). */
bool eg2_rng_main_sm_alert(uint32_t *state);

/** Enables csrng (software instance and internal-state read off), EDN0 auto mode on csrng
 * instance 0 with a reseed every `reseed_interval` generates of one block. */
bool eg2_rng_csrng_edn0_auto(uint32_t reseed_interval);

/** Reads rv_core_ibex RND_DATA (an EDN0 consumer) once valid; false after `polls`. */
bool eg2_rng_rnd_read(uint32_t *word, uint32_t polls);

/** The capture's taps, the bits of the `taps` knob. */
enum {
  kEg2RngTapRaw = 0,
  kEg2RngTapCond = 1,
  kEg2RngTapDrbg = 2,
  kEg2RngTapEdn = 3,
};

/** Writes one capture line: `EG2_RNG <tap>_<mode> w0..w7` through the DV log. */
void eg2_rng_log_line(uint32_t tap, eg2_rng_esmode_t mode, const uint32_t w[8]);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_RNG_EG2_RNG_H_
