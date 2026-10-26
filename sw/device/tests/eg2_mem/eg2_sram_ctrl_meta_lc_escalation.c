// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_ctrl_meta_lc_escalation (EG2 verification spec REQ_CMS_14, REQ_SEC_18; the
 * `chip_sw_sram_ctrl_meta` row's `chip_sw_sram_lc_escalation` procedure): OpenTitan's
 * chip_sw_sram_lc_escalation testpoint (chip_sram_ctrl_testplan.hjson; its test
 * sram_ctrl_lc_escalation_test.c with the host harness sram_ctrl_lc_escalation.rs) run on
 * `sram_ctrl_meta`, the SRAM that holds the CHERIoT tags and the revocation bitmap, with the life
 * cycle's escalation coming the documented way: an alert, the alert handler's escalation signal to
 * lc_ctrl, lc_ctrl's `lc_escalate_en` broadcast. (`chip_sw_sram_ctrl_meta` forces `lc_escalate_en`
 * at the controller instead; this test leaves the path alone.)
 *
 * The testpoint: "Configure the LC_CTRL to trigger an escalation request to the SRAMs. Verify that
 * the SRAMs stop accepting and responding to new memory requests. Reset the system to exit the
 * terminal escalation state. Re-initialize the SRAMs and verify that they can now respond
 * correctly to any further memory requests."
 *
 * Boot 1 (boot_phase 0):
 *   1. The meta SRAM works: RISC-V mode renews its key and initialises it (the escalation's key
 *      reset is then visible, sram_ctrl interfaces.md "Global and Local Escalation"); CHERIoT
 *      mode (the start-up code renewed and initialised it) writes and reads back `n_words` words
 *      of the bitmap through the revocation-bitmap window and stores and reloads a capability,
 *      whose tag lives in the meta SRAM. STATUS has no error bit and ESCALATED clear.
 *   2. `EG2 MEM metaesc ready <w> <fails>`: w is a bitmap word the test keeps none of its own
 *      capabilities under (the part of the bitmap over sec SRAM); the bench takes the RAM word and
 *      checks the controller holds a key from OTP, not its netlist constant.
 *   3. As sram_ctrl_lc_escalation_test.c: class A takes lc_ctrl's fatal_bus_integ_error, escalates
 *      on the first alert (threshold 0) and cannot be stopped; escalation signal `esc_signal` (1 or
 *      2: lc_ctrl's esc_scrap_state0 and 1, top_earlgrey.hjson) in phase `esc_phase`, the phases
 *      before it short and empty, its own phase endless. `EG2 MEM metaesc escalate <fails>`, then
 *      the alert through lc_ctrl's ALERT_TEST.
 *   4. The escalation stops the core (lc_ctrl's escalate state turns lc_cpu_en off), so the
 *      software cannot check the SRAM itself, as the OpenTitan test says; on the FPGA its host
 *      tries JTAG, which escalation closes for its own reasons. The bench checks instead
 *      (chip_sw_eg2_sram_ctrl_meta_lc_escalation_vseq): lc_escalate_en reaches sram_ctrl_meta from
 *      lc_ctrl; STATUS.ESCALATED; the key and nonce are the netlist constants; SCR_KEY_ROTATED is
 *      MuBi4False (registers.md); a read and a write it puts on the meta SRAM's bus port are
 *      answered with a bus error, never reach the memory, and leave the word unchanged: no tag is
 *      set (REQ_CMS_14 "capability accesses then error and set no tag"). It then resets the chip
 *      (power-on) with boot_phase = 1.
 * Boot 2 (boot_phase 1): ESCALATED clear and no error bit; the meta SRAM works again: RISC-V mode
 *   renews the key (the bench: a key from OTP again) and initialises it (the bench: RAM written);
 *   CHERIoT mode repeats step 1's access and capability round trip.
 *
 * The software's checks before the escalation are lost with the core, so `<fails>` carries them
 * to the bench, which fails the run on a non-zero count.
 *
 * Knobs: esc_signal (1-2), esc_phase (0-3), n_words (16-256), boot_phase (the bench's),
 * model_gaps.
 *
 * | Skipped with model_gaps = 1           | Why                                                  |
 * |---------------------------------------|------------------------------------------------------|
 * | the escalation (step 3) and the       | eg2sim has no life-cycle escalation and no bench;    |
 * |   bench's checks; boot 2 runs in the  |   the model runs steps 1-2 and boot 2's checks       |
 * |   same boot                           |                                                      |
 *
 * Built with EG2_SRAM_CTRL_META_LC_ESCALATION_INJECT_FAIL the first value check expects bit 0
 * flipped: on the chip boot 1 counts a failure and the bench fails the run with
 * `EG2 MEM metaesc: boot 1 failed 1 check(s)`; in the model the run fails with `MEM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/sram_ctrl_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define METAESC_PARAMS(X) \
  X(esc_signal, 1, 1, 2)  \
  X(esc_phase, 0, 0, 3)   \
  X(n_words, 64, 16, 256) \
  X(boot_phase, 0, 0, 1)  \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(METAESC_PARAMS)

enum {
  kMubiTrue = 0x6,
  kMubiFalse = 0x9,
  kPolls = 200000,
  kWin = TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR,
  kWinBytes = TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_SIZE_BYTES,
  // The bitmap bytes that cover sec SRAM: one bit per 8-byte granule from main SRAM's base.
  kOwnFirst = (TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR - TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR) /
              64,
  kOwnWords = (kWinBytes - kOwnFirst) / 4,
  // Class A's phases before the escalating one (alert handler cycles).
  kShortPhaseCycles = 100,
  // How long the core spins after raising the alert before it reports that it was not stopped.
  kStopPolls = 4000000,
};

static uint32_t meta_rd(uint32_t off) { return eg2_dev_read32(kEg2DevSramCtrlMetaRegs, off); }

static const uint32_t kErrBits = (1u << SRAM_CTRL_STATUS_BUS_INTEG_ERROR_BIT) |
                                 (1u << SRAM_CTRL_STATUS_INIT_ERROR_BIT) |
                                 (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT) |
                                 (1u << SRAM_CTRL_STATUS_SRAM_ALERT_BIT);

static bool status_clean(const char *when) {
  uint32_t st = meta_rd(SRAM_CTRL_STATUS_REG_OFFSET);
  eg2_chk_step(when, st);
  bool ok = eg2_chk_eq("STATUS error bits", 0, st & kErrBits);
  ok = eg2_chk_eq("STATUS.ESCALATED", 0, st & (1u << SRAM_CTRL_STATUS_ESCALATED_BIT)) && ok;
  return ok;
}

#ifndef __CHERIOT__
static void meta_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevSramCtrlMetaRegs, off, v); }

static bool wait_status(uint32_t bit, uint32_t *status) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    *status = meta_rd(SRAM_CTRL_STATUS_REG_OFFSET);
    if (*status & (1u << bit)) {
      return true;
    }
  }
  return false;
}

/** RISC-V mode: a key from OTP and INIT (the bench compares the key and the RAM). */
static bool renew_and_init(void) {
  uint32_t st = 0;
  LOG_INFO("EG2 MEM metaesc renew begin");
  meta_wr(SRAM_CTRL_CTRL_REG_OFFSET, 1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  bool ok = eg2_chk_true("SCR_KEY_VALID", wait_status(SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT, &st));
  ok = eg2_chk_eq("SCR_KEY_ROTATED after the renewal", kMubiTrue,
                  meta_rd(SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) & 0xf) &&
       ok;
  LOG_INFO("EG2 MEM metaesc renew done");
  meta_wr(SRAM_CTRL_CTRL_REG_OFFSET, 1u << SRAM_CTRL_CTRL_INIT_BIT);
  ok = eg2_chk_true("INIT_DONE", wait_status(SRAM_CTRL_STATUS_INIT_DONE_BIT, &st)) && ok;
  ok = eg2_chk_eq("status error bits after INIT", 0, st & kErrBits) && ok;
  LOG_INFO("EG2 MEM metaesc init done");
  return ok;
}

#else  // CHERIoT mode
// A capability-sized slot in main SRAM, whose tag is a bit of the meta SRAM.
static void *volatile cap_slot;
static uint32_t cap_target;

/** The pattern of the bitmap access (eg2_sram_ctrl_meta.c's). */
static uint32_t pattern(uint32_t seed, uint32_t i) {
  uint32_t x = seed ^ (i * 0x9e3779b9u);
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  return x;
}

/** CHERIoT mode: the bitmap window and a tag round trip, both served by the meta SRAM. */
static bool meta_works(void) {
  bool ok = true;
  uint32_t n = EG2_PARAM(n_words);
  if (n > kOwnWords) {
    n = kOwnWords;
  }
  uint32_t first = kOwnFirst / 4 + eg2_rand_range(0, kOwnWords - n);
  uint32_t seed = eg2_rand();
  volatile uint32_t *w = eg2_chk_ptr(kWin);
  for (uint32_t i = 0; i < n; ++i) {
    w[first + i] = pattern(seed, i);
  }
  uint32_t bad = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (w[first + i] != pattern(seed, i)) {
      ++bad;
    }
  }
  ok = eg2_chk_eq("bitmap words read back wrong", 0, bad) && ok;
  for (uint32_t i = 0; i < n; ++i) {
    w[first + i] = 0;
  }
  cap_slot = (void *)&cap_target;
  void *got = cap_slot;
  ok = eg2_chk_eq("tag of a stored and reloaded capability", 1,
                  (uint32_t)__builtin_cheri_tag_get(got)) &&
       ok;
  return ok;
}
#endif

/** Class A as sram_ctrl_lc_escalation_test.c configures it, with the drawn signal and phase. */
static void configure_class_a(size_t alert, uint32_t signal, uint32_t phase) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler,
                           ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * (uint32_t)alert,
                           ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
  eg2_dev_write32_shadowed(kEg2DevAlertHandler,
                           ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * (uint32_t)alert, 1);
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
                           0);
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
                           0);
  for (uint32_t p = 0; p < 4; ++p) {
    eg2_dev_write32_shadowed(kEg2DevAlertHandler,
                             ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET + 4 * p,
                             p == phase ? 0xffffffffu : kShortPhaseCycles);
  }
  // The signal in its phase only: each enabled signal is mapped to a phase, so the other three
  // stay disabled.
  uint32_t ctrl = (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT) |
                  (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_LOCK_BIT);
  ctrl |= signal == 1 ? (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E1_BIT) |
                            (phase << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E1_OFFSET)
                      : (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E2_BIT) |
                            (phase << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E2_OFFSET);
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET, ctrl);
}

/** Boot 2's checks (in the model: the same boot). */
static bool after_reset(bool cheriot) {
  bool ok = status_clean("STATUS after the reset");
#ifdef __CHERIOT__
  (void)cheriot;
  ok = meta_works() && ok;
#else
  (void)cheriot;
  ok = renew_and_init() && ok;
#endif
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
#ifdef __CHERIOT__
  const bool cheriot = true;
#else
  const bool cheriot = false;
#endif
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  if (EG2_PARAM(boot_phase) == 1) {
    eg2_chk_step("boot after the escalation", 1);
    return after_reset(cheriot) && eg2_chk_summary(3);
  }

  // 1. The meta SRAM works.
  bool ok = status_clean("STATUS before the escalation");
#ifdef __CHERIOT__
  ok = meta_works() && ok;
#else
  ok = renew_and_init() && ok;
#endif
  int alert = eg2_status_alert_find("lc_ctrl_fatal_bus_integ_error");
  ok = eg2_chk_true("lc_ctrl_fatal_bus_integ_error found", alert >= 0) && ok;

  // 2. The word the bench probes.
  uint32_t word = kOwnFirst / 4 + eg2_rand_range(0, kOwnWords - 1);
  LOG_INFO("EG2 MEM metaesc ready %u %u", word, ok ? 0u : 1u);
  eg2_chk_delay(2000);

  if (gaps) {
    eg2_chk_skip("the life-cycle escalation and the bench's checks; boot 2 in this boot");
    ok = after_reset(cheriot) && ok;
    return eg2_chk_summary(4) && ok;
  }

  // 3. The escalation.
  uint32_t signal = EG2_PARAM(esc_signal);
  uint32_t phase = EG2_PARAM(esc_phase);
  eg2_chk_step("escalation signal", signal);
  eg2_chk_step("escalation phase", phase);
  configure_class_a((size_t)alert, signal, phase);
  bool sum = eg2_chk_summary(3) && ok;
  LOG_INFO("EG2 MEM metaesc escalate %u", sum ? 0u : 1u);
  eg2_chk_delay(2000);
  eg2_status_raise_alert((size_t)alert);

  // 4. The core stops here; the bench resets the chip into boot 2.
  eg2_chk_delay(kStopPolls);
  return eg2_chk_true("the life-cycle escalation stopped the core", false) && false;
}
