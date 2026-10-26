// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_ownership_power_loss and _cheriot (EG2 verification spec REQ_SEC_14, "Security in
 * RISC-V mode"): a reset at any point of committing an owner slot leaves either the old owner or
 * the new one valid, never neither and never both. Behind the full secure-boot chain.
 *
 * Oracle: ownership transfer "OpenTitan Host Mode" ("fault tolerant manner"), "Ownership
 * Transfer" (unlock, then activate: the new owner's configuration in owner slot 1 becomes slot 0)
 * and the boot log's ownership state and transfer count (boot_log.h).
 *
 * Steps (one per boot, the state in the retention SRAM's owner area):
 *   0  record the ownership state and transfer count; request an ownership unlock (mode Any)
 *      with the signature the bench placed in `kEg2OwnerUnlockSig`, over this boot's nonce;
 *   1  the unlock accepted; request activation of the new owner (`kEg2OwnerActivateSig`). The
 *      bench resets the chip at the `reset_point`-th RRAM controller write into the owner slot
 *      pages while ROM_EXT commits it;
 *   2  after the interrupted commit (and every later boot): exactly one of {old owner, new owner}
 *      is in force: the transfer count is the old one or one more, and the ownership state is
 *      the locked state; never an unowned or unlocked device after an activation began.
 *
 * Blocked (rtl_tickets.md F28): the unlock and activate requests must be signed by the owner's
 * keys over the device's nonce, which ROM_EXT draws at run time. Nothing in this tree signs a
 * boot-services request in simulation: the FPGA suites sign on the host with opentitantool after
 * reading the nonce. The bench needs an ECDSA P-256 signer (DPI) with the fake owner keys, or
 * ROM_EXT a test-only fixed nonce. Until then the bench writes no signature and the run fails at
 * step 1 with `EG2CHK FAIL unlock accepted`; it is in no regression.
 *
 * Built with EG2_OWNERSHIP_POWER_LOSS_INJECT_FAIL the unlock is expected refused: the run must fail
 * with `EG2CHK FAIL unlock accepted`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/lib/base/hardened.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_ownership_activate.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_ownership_unlock.h"
#include "sw/device/silicon_creator/lib/error.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_boot/eg2_boot_svc.h"

EG2_TEST_CONFIG();

enum {
  kOwnState = 0,
  kOwnTransfers = 1,
};

/** Written by the bench before each request (in RAM, as the signed image cannot be patched). */
volatile owner_signature_t kEg2OwnerUnlockSig;
volatile owner_signature_t kEg2OwnerActivateSig;
volatile owner_keydata_t kEg2NextOwnerKey;
/** For the bench: the step this boot is at (it signs after it reads the nonce). */
volatile uint32_t kEg2OwnerStep;

static void copy_sig(owner_signature_t *dst, const volatile owner_signature_t *src) {
  volatile const uint32_t *s = (volatile const uint32_t *)src;
  uint32_t *d = (uint32_t *)dst;
  for (uint32_t i = 0; i < sizeof(owner_signature_t) / 4; ++i) {
    d[i] = s[i];
  }
}

bool eg2_test_main(void) {
  volatile eg2_boot_state_t *st = eg2_boot_state();
  volatile boot_log_t *log = &eg2_boot_retram()->creator.boot_log;
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA);
  boot_svc_msg_t msg = {0};
  nonce_t nonce = {{log->rom_ext_nonce.value[0], log->rom_ext_nonce.value[1]}};
  kEg2OwnerStep = st->step;
  // Give the bench time to read the nonce and write the signature.
  for (volatile uint32_t i = 0; i < 20000; ++i) {
  }
  owner_signature_t sig;
  owner_keydata_t key = {0};
  if (st->step == 0) {
    st->data[kOwnState] = log->ownership_state;
    st->data[kOwnTransfers] = log->ownership_transfers;
    copy_sig(&sig, &kEg2OwnerUnlockSig);
    boot_svc_ownership_unlock_req_init(kBootSvcUnlockAny, kOwnershipKeyAlgEcdsaP256, nonce, &key,
                                       &sig, &msg.ownership_unlock_req);
    eg2_boot_svc_put(&msg);
    st->step = 1;
    eg2_boot_sw_reset();
  }
  if (st->step == 1) {
    ok = eg2_boot_svc_check_response(kBootSvcOwnershipUnlockResType, &msg) && ok;
    bool accepted = msg.ownership_unlock_res.status == kErrorOk;
#ifdef EG2_OWNERSHIP_POWER_LOSS_INJECT_FAIL
    accepted = !accepted;
#endif
    ok = eg2_boot_expect("unlock accepted", accepted, msg.ownership_unlock_res.status) && ok;
    if (!ok) {
      return eg2_boot_summary() && ok;
    }
    copy_sig(&sig, &kEg2OwnerActivateSig);
    boot_svc_ownership_activate_req_init(/*primary_bl0_slot=*/kEg2BootSlotA,
                                         /*erase_previous=*/kHardenedBoolTrue, nonce, &sig,
                                         &msg.ownership_activate_req);
    eg2_boot_svc_put(&msg);
    st->step = 2;
    eg2_boot_sw_reset();
  }
  uint32_t transfers = log->ownership_transfers;
  bool one = transfers == st->data[kOwnTransfers] || transfers == st->data[kOwnTransfers] + 1;
  ok = eg2_boot_expect("old or new owner: transfer count", one, transfers) && ok;
  ok = eg2_boot_check("old or new owner: ownership state locked", st->data[kOwnState],
                      log->ownership_state) &&
       ok;
  st->magic = 0;
  return eg2_boot_summary() && ok;
}
