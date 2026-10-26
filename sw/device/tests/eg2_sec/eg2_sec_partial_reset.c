// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_partial_reset (EG2 verification spec REQ_SEC_22, "Security in RISC-V mode"):
 * blocks that work in step stay consistent when one side is reset: a divergence raises an alert,
 * never a silent difference. Both modes.
 *
 * None of the pairs REQ_SEC_22 names has a software reset of its own in Earl Grey 2 (rstmgr's
 * software resets are SPI device and hosts, USB, I2C and I3C only, top_earlgrey.hjson), so the pair
 * is reset the way the chip allows: the core and its lockstep shadow. The bench
 * (chip_sw_eg2_sec_partial_reset_vseq) holds the shadow core's reset (ibex_lockstep rst_shadow_n)
 * for `hold` cycles while this software runs a busy loop (`busy_loops`). Oracle: rv_core_ibex's
 * "Lockstep" and local escalation: a divergence raises fatal_hw_err with ERR_STATUS.FATAL_CORE_ERR
 * and disables fetch (the bench then ends the run on the alert); a shadow that restarts in step
 * is "consistent", and this software then checks that no error status was left without the alert.
 *
 * Not done (reported): the TBRE and the bitmap's store (re-initialising the meta SRAM in CHERIoT
 * mode untags every capability in memory, the program's own included, and in RISC-V mode the TBRE
 * refuses to start: cheriot_mem_sys.hjson TBRE_STATUS.start_err), cheriot_mem_sys and its shadow
 * logic, rram_ctrl's two PRINCE paths: no reset of one side exists that software or the bench could
 * take without changing the design.
 *
 * Built with EG2_SEC_PARTIAL_RESET_INJECT_FAIL the pairing is expected broken: the run must fail
 * with `EG2CHK FAIL lockstep: alert and FATAL_CORE_ERR together`.
 */

#include "hw/top/rv_core_ibex_regs.h"     // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define PARTIAL_RESET_PARAMS(X) \
  X(busy_loops, 20000, 100, 1000000)
EG2_RM_PARAMS(PARTIAL_RESET_PARAMS)

/** For the bench: 1 while the busy loop of pair 0 runs. */
volatile uint32_t kEg2PartialResetBusy;

static bool alert_cause(const char *name, bool *raised) {
  int a = eg2_status_alert_find(name);
  *raised = false;
  if (a < 0) {
    return false;
  }
  *raised = eg2_status_wait_alert((size_t)a, 2000);
  if (*raised) {
    // Raised on purpose: the status sweep then requires it.
    eg2_status_expect_alert((size_t)a);
  }
  return true;
}

static bool pair_lockstep(void) {
  // The bench holds the shadow's reset while this loop runs.
  kEg2PartialResetBusy = 1;
  volatile uint32_t acc = 0;
  for (uint32_t i = 0; i < EG2_PARAM(busy_loops); ++i) {
    acc = acc * 33 + i;
  }
  kEg2PartialResetBusy = 0;
  bool raised = false;
  bool ok = eg2_boot_expect("alert fatal_hw_err known", alert_cause("rv_core_ibex_fatal_hw_err",
                                                                    &raised),
                            0);
  uint32_t err = eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  eg2_boot_note("rv_core_ibex ERR_STATUS", err);
  eg2_boot_note("fatal_hw_err raised", raised);
  // Consistent (no alert) or detected (the alert with FATAL_CORE_ERR): never an error status
  // without the alert.
  bool core_err = (err & (1u << RV_CORE_IBEX_ERR_STATUS_FATAL_CORE_ERR_BIT)) != 0;
  bool together = raised == core_err;
#ifdef EG2_SEC_PARTIAL_RESET_INJECT_FAIL
  together = !together;
#endif
  ok = eg2_boot_expect("lockstep: alert and FATAL_CORE_ERR together", together, err) && ok;
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  ok = pair_lockstep() && ok;
  return eg2_boot_summary() && ok;
}
