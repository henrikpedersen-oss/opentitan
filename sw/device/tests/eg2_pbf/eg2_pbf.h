// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_H_

/**
 * @file
 * @brief What the post-boot feature tests share (eg2_top_verification spec REQ_PBF, the
 * `chip_sw_eg2_pbf_<area>_sb` and `_fault` tests).
 *
 *  - Checks with one log format, `PBF ok <what>` / `PBF FAIL <what> expected .. got ..`, and the
 *    must-fail build: with `EG2_PBF_INJECT_FAIL` the first `eg2_pbf_check*` compares against its
 *    expected value with bit 0 flipped, so the run must fail with `PBF FAIL` (REQ_STE_03).
 *  - Activity lines, `PBF activity <name> <count>`: what the feature did (REQ_PBF_05). They are
 *    for the log; the bench confirms the activity itself (the test's vseq, from the design).
 *  - Gaps, `PBF gap <what>: <why>`: a check the spec asks for that this build cannot make; a
 *    gap never passes silently. `PBF model_gap <what>` is a check skipped with `model_gaps` = 1,
 *    the knob that stays 0 on the chip.
 *  - The boot: which boot stage handed over (`eg2_pbf_boot_stage`: ROM_EXT, i.e. behind secure
 *    boot, or the test ROM), and the boot index across resets, kept in retention SRAM with a
 *    test's own words (`eg2_pbf_ret`), so a test can fault the chip with a reset and check the
 *    outcome on the next boot.
 *  - Escalation: one alert-handler class configured to escalate (`eg2_pbf_escalate`), every other
 *    alert left in the status module's class D.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True in a build with -DEG2_PBF_INJECT_FAIL (the define reaches only the test's source). */
#ifdef EG2_PBF_INJECT_FAIL
#define EG2_PBF_INJECT true
#else
#define EG2_PBF_INJECT false
#endif

/** The test's area name, printed with every line ("sym_crypto"); set by `eg2_pbf_begin`. */
void eg2_pbf_begin(const char *area, bool inject_fail);

/** `PBF ok <what>` or `PBF FAIL <what>`; returns ok. No corruption: a step, not a value. */
bool eg2_pbf_step(const char *what, bool ok);

/**
 * Compares a value with its expected value (the first compare of an inject-fail build expects
 * bit 0 flipped). Prints both on a mismatch. Returns true on a match.
 */
bool eg2_pbf_check(const char *what, uint32_t expected, uint32_t got);

/** The same for byte strings; the corruption flips bit 0 of the first byte. */
bool eg2_pbf_check_bytes(const char *what, const uint8_t *expected, const uint8_t *got,
                         size_t len);

/** The same for 32-bit word strings; the corruption flips bit 0 of the first word. */
bool eg2_pbf_check_words(const char *what, const uint32_t *expected, const uint32_t *got,
                         size_t n);

/** `PBF note <what> 0x<value>`: a value for the log, not a check. */
void eg2_pbf_note(const char *what, uint32_t value);

/** `PBF activity <name> <count>`: what the feature did, for the log (REQ_PBF_05). */
void eg2_pbf_activity(const char *name, uint32_t count);

/** `PBF gap <what>: <why>`: a check this build cannot make, named so it never passes silently. */
void eg2_pbf_gap(const char *what, const char *why);

/** `PBF model_gap <what>`: skipped with `model_gaps` = 1 (the SystemC model lacks it). */
void eg2_pbf_model_gap(const char *what);

/** Where the owner code was handed over from. */
typedef enum eg2_pbf_boot_stage {
  kEg2PbfBootTestRom = 0,  // the test ROM (no secure boot)
  kEg2PbfBootRomExt = 1,   // ROM, then ROM_EXT: behind secure boot
  kEg2PbfBootUnknown = 2,
} eg2_pbf_boot_stage_t;

/**
 * The boot stage that handed over, from retention SRAM: the test ROM's identifier in the last
 * reserved word of the creator area, or ROM_EXT's boot log (`BLOG`). Prints `PBF boot <stage>`
 * and, behind ROM_EXT, the boot log's ROM_EXT and BL0 slots. A `_sb` test reports
 * "not run behind secure boot" when it is not (REQ_PBF_01).
 */
eg2_pbf_boot_stage_t eg2_pbf_boot_stage(void);

/** ROM_EXT's boot log field (word index into boot_log_t), 0 if there is no boot log. */
uint32_t eg2_pbf_boot_log_word(uint32_t index);

/**
 * The boot index across resets: 0 on a power-on boot (the test's retention words cleared), else
 * the number of boots since. Kept with a magic word in the last 64 words of retention SRAM's
 * owner area, clear of ret_sram_testutils' area at its start. Call once, first.
 */
uint32_t eg2_pbf_boot_index(void);

/** The reset reason of this boot (rstmgr_testutils_reason_get: the ROM's copy). */
uint32_t eg2_pbf_reset_reason(void);

enum {
  /** Words a test may keep across resets (`eg2_pbf_ret`). */
  kEg2PbfRetWords = 56,
};

/** Word `i` (< kEg2PbfRetWords) of the test's state in retention SRAM. */
volatile uint32_t *eg2_pbf_ret(uint32_t i);

/** Requests a software reset (rstmgr RESET_REQ) and waits for it; never returns. */
void eg2_pbf_sw_reset(void) __attribute__((noreturn));

/** Alert-handler classes. */
typedef enum eg2_pbf_class {
  kEg2PbfClassA = 0,
  kEg2PbfClassB = 1,
  kEg2PbfClassC = 2,
} eg2_pbf_class_t;

/** One class's escalation (alert-handler theory of operation, "Escalation"). */
typedef struct eg2_pbf_esc {
  eg2_pbf_class_t cls;
  uint32_t accum_thresh;    // escalate on alert number accum_thresh + 1
  uint32_t phase_cycles[4]; // each phase's length (io_div4 cycles)
  uint32_t signal_en;       // bit i: escalation signal i enabled (CTRL.EN_Ei)
  uint32_t map[4];          // map[i]: the phase (0..3) in which signal i fires (CTRL.MAP_Ei);
                            // signals: 0 NMI, 1 and 2 life-cycle escalation, 3 reset
  uint32_t crashdump_phase; // the phase whose entry latches the crash dump
  bool lock;                // CTRL.LOCK: software cannot stop the escalation
} eg2_pbf_esc_t;

/** Configures one class and moves the given alerts (and local alerts) to it. */
void eg2_pbf_escalate(const eg2_pbf_esc_t *esc, const uint32_t *alerts, size_t num_alerts);

/** The class's state register (CLASSx_STATE): 0 Idle, 1 Timeout, 2 FsmError, 3 Terminal, 4-7 Phase0-3. */
uint32_t eg2_pbf_class_state(eg2_pbf_class_t cls);

/** The class's accumulator (CLASSx_ACCUM_CNT). */
uint32_t eg2_pbf_class_accum(eg2_pbf_class_t cls);

/** Writes the class's clear (CLASSx_CLR_SHADOWED); has no effect once the class is locked. */
void eg2_pbf_class_clear(eg2_pbf_class_t cls);

/** Busy-waits about `n` loop iterations (no timer needed). */
void eg2_pbf_spin(uint32_t n);

/** True in the CHERIoT build. */
static inline bool eg2_pbf_cheriot(void) {
#ifdef __CHERIOT__
  return true;
#else
  return false;
#endif
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_H_
