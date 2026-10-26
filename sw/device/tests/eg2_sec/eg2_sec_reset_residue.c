// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_reset_residue (EG2 verification spec REQ_SEC_25, "Security in RISC-V mode"): no
 * secret survives a reset in a flop or memory the next software can read. Both modes.
 *
 * Oracle: hardware guidelines Recommendation 15; the registers' reset values (keymgr_dpe.hjson
 * SW_SHARE0/1_OUTPUT, hmac.hjson DIGEST_*: reset value 0, and every reset type of rstmgr reaches
 * the blocks of the `sys` domain).
 *
 * First boot: known secrets are left where software can read them after a reset:
 *   `locs` bit 0  keymgr_dpe's software output: a versioned key generated twice from the same
 *                 inputs, the first read (its value kept) and the second left in
 *                 SW_SHARE0/1_OUTPUT;
 *   `locs` bit 1  hmac's digest: SHA-256 of a random message, left in DIGEST_0..7.
 * The values are kept in the retention SRAM's owner area, then the chip resets by `reset`:
 *   0  a software reset (rstmgr RESET_REQ);
 *   1  the watchdog's bite (aon_timer, pwrmgr RESET_EN for the aon_timer request);
 *   2  an escalation reset (an alert of class A whose phase 0 maps to escalation signal 3, pwrmgr's
 *      escalation reset).
 * Second boot: every location reads differently from the secret (and its shares do not contain
 * it), and the reset reason is the one requested.
 *
 * Not checked by software: the register files (main and shadow core) and the meta SRAM, which no
 * software can read before the boot code overwrites them (test_rom_start.S and the ROM clear every
 * register first); the bench's part, reported as not done.
 *
 * Built with EG2_SEC_RESET_RESIDUE_INJECT_FAIL the digest is expected to survive: the run must
 * fail with `EG2CHK FAIL hmac digest cleared by the reset`.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/aon_timer_regs.h"      // Generated.
#include "hw/top/hmac_regs.h"           // Generated.
#include "hw/top/keymgr_dpe_regs.h"     // Generated.
#include "hw/top/pwrmgr_regs.h"         // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define RESIDUE_PARAMS(X) \
  X(locs, 3, 1, 3)        \
  X(reset, 0, 0, 2)
EG2_RM_PARAMS(RESIDUE_PARAMS)

enum {
  // Retention state: data[0..7] the key, data[8..15] the digest.
  kKey = 0,
  kDigest = 8,
  kWdogBite = 64,  // aon clock cycles
};

static bool gen_key(uint32_t key[8]) {
  uint32_t binding[8], salt[8], k1[8], k2[8];
  for (uint32_t i = 0; i < 8; ++i) {
    binding[i] = eg2_rand();
    salt[i] = eg2_rand();
  }
  bool ok = eg2_kmgr_chain(0, binding, salt, k1, k2);
  for (uint32_t i = 0; i < 8; ++i) {
    key[i] = k2[i];
  }
  // The same generation again, left unread in SW_SHARE0/1_OUTPUT.
  ok = eg2_boot_check("second generation", kEg2KmgrDone,
                      eg2_kmgr_op(kEg2KmgrOpGenSw, 0, 0, 0, false, NULL)) &&
       ok;
  return ok;
}

static void hmac_digest(uint32_t digest[8]) {
  eg2_dev_write32(kEg2DevHmac, HMAC_CFG_REG_OFFSET,
                  (1u << HMAC_CFG_SHA_EN_BIT) | (1u << HMAC_CFG_DIGEST_SWAP_BIT) |
                      (1u << HMAC_CFG_DIGEST_SIZE_OFFSET));  // SHA2-256
  eg2_dev_write32(kEg2DevHmac, HMAC_CMD_REG_OFFSET, 1u << HMAC_CMD_HASH_START_BIT);
  for (uint32_t i = 0; i < 16; ++i) {
    eg2_dev_write32(kEg2DevHmac, HMAC_MSG_FIFO_REG_OFFSET, eg2_rand());
  }
  eg2_dev_write32(kEg2DevHmac, HMAC_CMD_REG_OFFSET, 1u << HMAC_CMD_HASH_PROCESS_BIT);
  for (uint32_t i = 0; i < 100000; ++i) {
    if (eg2_dev_read32(kEg2DevHmac, HMAC_STATUS_REG_OFFSET) & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) {
      break;
    }
  }
  for (uint32_t i = 0; i < 8; ++i) {
    digest[i] = eg2_dev_read32(kEg2DevHmac, HMAC_DIGEST_0_REG_OFFSET + 4 * i);
  }
  // hmac_done is an event, not an error interrupt.
  eg2_dev_write32(kEg2DevHmac, 0 /* INTR_STATE */, 0xffffffffu);
}

static void ah_shadowed(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

static void reset_by_escalation(void) {
  int alert = eg2_status_alert_find("uart1_fatal_fault");
  // Class A: phase 0 after 16 cycles, mapped to escalation signal 3 (pwrmgr's escalation reset).
  ah_shadowed(ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0);
  ah_shadowed(ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET, 16);
  ah_shadowed(ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
              (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT) |
                  (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E0_BIT) |
                  (3u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E0_OFFSET));
  ah_shadowed(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * (uint32_t)alert, 0);
  eg2_puts("EG2 escalation reset\r\n");
  eg2_status_raise_alert((size_t)alert);
}

static void reset_by_watchdog(void) {
  // pwrmgr: let the aon_timer's reset request through (its request index is 1).
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_RESET_EN_REG_OFFSET, 1u << PWRMGR_RESET_EN_EN_1_BIT);
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  while (eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET) & 1) {
  }
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 0xffffffffu);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, kWdogBite);
  eg2_puts("EG2 watchdog reset\r\n");
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET,
                  1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  volatile eg2_boot_state_t *st = eg2_boot_state();
  uint32_t locs = EG2_PARAM(locs);

  if (st->step == 0) {
    uint32_t v[8];
    if (locs & 1) {
      ok = gen_key(v) && ok;
      eg2_boot_note_key("secret key", v);
      for (uint32_t i = 0; i < 8; ++i) {
        st->data[kKey + i] = v[i];
      }
    }
    if (locs & 2) {
      hmac_digest(v);
      eg2_boot_note_key("secret digest", v);
      for (uint32_t i = 0; i < 8; ++i) {
        st->data[kDigest + i] = v[i];
      }
    }
    st->fails = ok ? 0 : 1;
    st->step = 1;
    switch (EG2_PARAM(reset)) {
      case 1:
        reset_by_watchdog();
        break;
      case 2:
        reset_by_escalation();
        break;
      default:
        eg2_boot_sw_reset();
    }
    for (uint32_t i = 0; i < 1000000; ++i) {
      __asm__ volatile("nop");
    }
    eg2_puts("EG2CHK FAIL the requested reset did not happen\r\n");
    return false;
  }

  ok = eg2_boot_check("first boot's checks", 0, st->fails) && ok;
  uint32_t reasons = eg2_boot_retram()->creator.reset_reasons;
  eg2_boot_note("reset reasons", reasons);
  if (EG2_PARAM(reset) == 0) {
    ok = eg2_boot_check("reset reason: software", 1u << RSTMGR_RESET_INFO_SW_RESET_BIT,
                        reasons & (1u << RSTMGR_RESET_INFO_SW_RESET_BIT)) &&
         ok;
  } else {
    ok = eg2_boot_expect("reset reason: a hardware request",
                         ((reasons >> RSTMGR_RESET_INFO_HW_REQ_OFFSET) &
                          RSTMGR_RESET_INFO_HW_REQ_MASK) != 0,
                         reasons) &&
         ok;
  }
  if (locs & 1) {
    uint32_t s0[8], s1[8], k[8], secret[8];
    eg2_kmgr_sw_output(s0, s1);
    bool share_has = false;
    for (uint32_t i = 0; i < 8; ++i) {
      k[i] = s0[i] ^ s1[i];
      secret[i] = st->data[kKey + i];
    }
    share_has = eg2_boot_key_eq(s0, secret) || eg2_boot_key_eq(s1, secret);
    ok = eg2_boot_expect("keymgr SW output cleared by the reset", !eg2_boot_key_eq(k, secret),
                         k[0]) &&
         ok;
    ok = eg2_boot_expect("keymgr SW output shares hold no copy", !share_has, s0[0]) && ok;
  }
  if (locs & 2) {
    uint32_t d[8], secret[8];
    for (uint32_t i = 0; i < 8; ++i) {
      d[i] = eg2_dev_read32(kEg2DevHmac, HMAC_DIGEST_0_REG_OFFSET + 4 * i);
      secret[i] = st->data[kDigest + i];
    }
    bool cleared = !eg2_boot_key_eq(d, secret);
#ifdef EG2_SEC_RESET_RESIDUE_INJECT_FAIL
    cleared = !cleared;
#endif
    ok = eg2_boot_expect("hmac digest cleared by the reset", cleared, d[0]) && ok;
  }
  st->magic = 0;
  return eg2_boot_summary() && ok;
}
