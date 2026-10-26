// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_boot_svc and chip_sw_eg2_boot_svc_cheriot (EG2 verification spec REQ_BOOT_12,
 * REQ_SEC_16, "Secure boot in RISC-V mode", "Twins of the RISC-V tests"): boot services work end
 * to end, and a request made from CHERIoT mode, through a capability to the retention SRAM, is
 * served the same way; the request and the reset reason survive the reset that carries it.
 *
 * Behind the full secure-boot chain (sim_dv_rom_ext). The owner image places each request in the
 * retention SRAM's boot services area and resets (rstmgr RESET_REQ); after ROM_EXT has served it,
 * the next boot checks the response (silicon creator "Boot Services"; boot_svc_*.h):
 *   step 0  empty request                       -> empty response;
 *   step 1  next BL0 slot (primary A, next A)   -> status OK, primary A, slot A booted;
 *   step 2  minimum BL0 security version = the current minimum (equal: allowed, no change)
 *                                               -> status OK, the minimum unchanged;
 *   step 3  ownership history                   -> a valid history response;
 *   step 4  ownership unlock (mode Any) with a zero key and a zero signature over the nonce in
 *           the boot log                        -> refused, ownership state and transfer count
 *                                                  unchanged (REQ_SEC_13: unlock only with the
 *                                                  owner's UNLOCK key);
 * and after every reset: the reset reasons ROM recorded (retention SRAM) include the software
 * reset (REQ_SEC_16). In the `_cheriot` build every boot switches to CHERIoT mode first: every
 * request is made in CHERIoT mode.
 *
 * The bench writes nothing into the signed image, so the steps run in this fixed order (the
 * spec's `boot service` knob is covered by running every service; the "reset during the request"
 * point is the bench's, see chip_sw_eg2_pbf_rom_ext_fault).
 *
 * Built with EG2_BOOT_SVC_TEST_INJECT_FAIL the bad unlock is expected to be accepted: the run must
 * fail with `EG2CHK FAIL unlock with a bad signature: status`.
 */

#include "hw/top/rstmgr_regs.h"  // Generated.
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_empty.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_min_bl0_sec_ver.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_next_boot_bl0_slot.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_ownership_history.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_ownership_unlock.h"
#include "sw/device/silicon_creator/lib/error.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_boot/eg2_boot_svc.h"

EG2_TEST_CONFIG();

enum {
  // State: data[0] the ownership state at step 0, data[1] the transfer count, data[2] the minimum.
  kOwnState = 0,
  kOwnTransfers = 1,
  kMinSecVer = 2,
  kSteps = 5,
};

static void request(volatile eg2_boot_state_t *st, const boot_svc_msg_t *msg) {
  eg2_boot_svc_put(msg);
  st->step = st->step + 1;
  eg2_boot_sw_reset();
}

bool eg2_test_main(void) {
  volatile eg2_boot_state_t *st = eg2_boot_state();
  volatile boot_log_t *log = &eg2_boot_retram()->creator.boot_log;
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA);
  boot_svc_msg_t msg = {0};
  uint32_t step = st->step;

  if (step > 0) {
    uint32_t reasons = eg2_boot_retram()->creator.reset_reasons;
    ok = eg2_boot_check("reset reason kept: software reset", 1u << RSTMGR_RESET_INFO_SW_RESET_BIT,
                        reasons & (1u << RSTMGR_RESET_INFO_SW_RESET_BIT)) &&
         ok;
  }
  switch (step) {
    case 0:
      st->data[kOwnState] = log->ownership_state;
      st->data[kOwnTransfers] = log->ownership_transfers;
      st->data[kMinSecVer] = log->bl0_min_sec_ver;
      st->fails = 0;
      boot_svc_empty_req_init(&msg.empty);
      break;
    case 1:
      ok = eg2_boot_svc_check_response(kBootSvcEmptyResType, &msg) && ok;
      boot_svc_next_boot_bl0_slot_req_init(kEg2BootSlotA, kEg2BootSlotA,
                                           &msg.next_boot_bl0_slot_req);
      break;
    case 2:
      ok = eg2_boot_svc_check_response(kBootSvcNextBl0SlotResType, &msg) && ok;
      ok = eg2_boot_check("next BL0 slot: status", kErrorOk, msg.next_boot_bl0_slot_res.status) &&
           ok;
      ok = eg2_boot_check("next BL0 slot: primary", kEg2BootSlotA,
                          msg.next_boot_bl0_slot_res.primary_bl0_slot) &&
           ok;
      boot_svc_min_bl0_sec_ver_req_init(st->data[kMinSecVer], &msg.min_bl0_sec_ver_req);
      break;
    case 3:
      ok = eg2_boot_svc_check_response(kBootSvcMinBl0SecVerResType, &msg) && ok;
      ok = eg2_boot_check("minimum security version: status", kErrorOk,
                          msg.min_bl0_sec_ver_res.status) &&
           ok;
      ok = eg2_boot_check("minimum security version unchanged", st->data[kMinSecVer],
                          msg.min_bl0_sec_ver_res.min_bl0_sec_ver) &&
           ok;
      ok = eg2_boot_check("boot log: minimum security version", st->data[kMinSecVer],
                          log->bl0_min_sec_ver) &&
           ok;
      boot_svc_ownership_history_req_init(&msg.ownership_history_req);
      break;
    case 4: {
      ok = eg2_boot_svc_check_response(kBootSvcOwnershipHistoryResType, &msg) && ok;
      ok = eg2_boot_check("ownership history: status", kErrorOk,
                          msg.ownership_history_res.status) &&
           ok;
      nonce_t nonce = {{log->rom_ext_nonce.value[0], log->rom_ext_nonce.value[1]}};
      owner_keydata_t key = {0};
      owner_signature_t sig = {0};
      boot_svc_ownership_unlock_req_init(kBootSvcUnlockAny, kOwnershipKeyAlgEcdsaP256, nonce,
                                         &key, &sig, &msg.ownership_unlock_req);
      break;
    }
    default: {
      ok = eg2_boot_svc_check_response(kBootSvcOwnershipUnlockResType, &msg) && ok;
      bool refused = msg.ownership_unlock_res.status != kErrorOk;
#ifdef EG2_BOOT_SVC_TEST_INJECT_FAIL
      refused = !refused;
#endif
      ok = eg2_boot_expect("unlock with a bad signature: status", refused,
                           msg.ownership_unlock_res.status) &&
           ok;
      ok = eg2_boot_check("ownership state unchanged", st->data[kOwnState],
                          log->ownership_state) &&
           ok;
      ok = eg2_boot_check("ownership transfers unchanged", st->data[kOwnTransfers],
                          log->ownership_transfers) &&
           ok;
      ok = eg2_boot_check("previous boots' checks", 0, st->fails) && ok;
      st->magic = 0;
      return eg2_boot_summary() && ok;
    }
  }
  if (!ok) {
    st->fails = st->fails + 1;
  }
  request(st, &msg);
  return false;
}
