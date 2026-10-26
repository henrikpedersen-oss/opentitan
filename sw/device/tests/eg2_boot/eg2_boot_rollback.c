// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_boot_rollback (EG2 verification spec REQ_BOOT_09, "Secure boot in RISC-V mode"): an
 * owner image whose security version is below the minimum recorded for BL0 is not booted, and the
 * minimum only increases.
 *
 * Oracle: secure boot "Manifest Requirements" (rollback protection) and the boot service "Set
 * Minimum Security Version" (boot_svc_min_bl0_sec_ver.h): ROM_EXT raises the minimum on request
 * up to the running image's security version, never lowers it and never raises it above that
 * version; an image below the minimum fails verification and ROM_EXT boots the other slot.
 *
 * Images (assembled by the BUILD): slot A is this image with security version 2; slot B is the
 * same source built with EG2_BOOT_ROLLBACK_OLD and security version 1, which fails at once if it
 * ever runs (`EG2CHK FAIL an older image booted`). Steps, one per boot (boot services through the
 * retention SRAM, software resets):
 *   0  request minimum 2 (equal to this image's version);
 *   1  the response: OK, minimum 2, the boot log agrees; request the next boot from slot B;
 *   2  slot A booted again (slot B's version 1 is below the minimum); request minimum 1 (lower);
 *   3  the response: refused, minimum still 2; request minimum 3 (above this image's version);
 *   4  the response: refused, minimum still 2.
 *
 * Built with EG2_BOOT_ROLLBACK_INJECT_FAIL the lowering is expected to be accepted: the run must
 * fail with `EG2CHK FAIL lower minimum refused`.
 */

#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_min_bl0_sec_ver.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_next_boot_bl0_slot.h"
#include "sw/device/silicon_creator/lib/error.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_boot/eg2_boot_svc.h"

EG2_TEST_CONFIG();

enum {
  kImageVersion = 2,
};

static bool min_response(const char *what, bool expect_ok, uint32_t expect_min) {
  boot_svc_msg_t msg;
  bool ok = eg2_boot_svc_check_response(kBootSvcMinBl0SecVerResType, &msg);
  bool status_ok = msg.min_bl0_sec_ver_res.status == kErrorOk;
  ok = eg2_boot_expect(what, status_ok == expect_ok, msg.min_bl0_sec_ver_res.status) && ok;
  ok = eg2_boot_check("minimum in the response", expect_min,
                      msg.min_bl0_sec_ver_res.min_bl0_sec_ver) &&
       ok;
  ok = eg2_boot_check("minimum in the boot log", expect_min,
                      eg2_boot_retram()->creator.boot_log.bl0_min_sec_ver) &&
       ok;
  return ok;
}

static void request_min(volatile eg2_boot_state_t *st, uint32_t min) {
  boot_svc_msg_t msg = {0};
  boot_svc_min_bl0_sec_ver_req_init(min, &msg.min_bl0_sec_ver_req);
  eg2_boot_svc_put(&msg);
  st->step = st->step + 1;
  eg2_boot_sw_reset();
}

bool eg2_test_main(void) {
#ifdef EG2_BOOT_ROLLBACK_OLD
  eg2_boot_expect("an older image booted", false, 0);
  return false;
#else
  volatile eg2_boot_state_t *st = eg2_boot_state();
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA);
  bool lower_refused = false;
  switch (st->step) {
    case 0:
      st->fails = 0;
      eg2_boot_note("minimum at the start", eg2_boot_retram()->creator.boot_log.bl0_min_sec_ver);
      request_min(st, kImageVersion);
      break;
    case 1: {
      ok = min_response("raise to the image's version accepted", true, kImageVersion) && ok;
      st->fails += ok ? 0 : 1;
      boot_svc_msg_t msg = {0};
      boot_svc_next_boot_bl0_slot_req_init(kEg2BootSlotA, kEg2BootSlotB,
                                           &msg.next_boot_bl0_slot_req);
      eg2_boot_svc_put(&msg);
      st->step = 2;
      eg2_boot_sw_reset();
      break;
    }
    case 2:
      // The boot log check above: slot A again, slot B was refused.
      st->fails += ok ? 0 : 1;
      request_min(st, kImageVersion - 1);
      break;
    case 3:
#ifdef EG2_BOOT_ROLLBACK_INJECT_FAIL
      lower_refused = !min_response("lower minimum refused", true, kImageVersion);
#else
      lower_refused = min_response("lower minimum refused", false, kImageVersion);
#endif
      st->fails += (ok && lower_refused) ? 0 : 1;
      request_min(st, kImageVersion + 1);
      break;
    default:
      ok = min_response("raise above the image's version refused", false, kImageVersion) && ok;
      ok = eg2_boot_check("previous boots' checks", 0, st->fails) && ok;
      st->magic = 0;
      return eg2_boot_summary() && ok;
  }
  return false;
#endif
}
