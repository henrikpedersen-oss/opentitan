// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_sym_crypto_fault (EG2 verification spec REQ_PBF_10, "Post-boot faults"): an AES
 * or KMAC operation faulted while in use, by an escalation or a reset, in both modes.
 *
 * Boot 0 (power-on): the operation runs to `fault_point` and the fault comes:
 *   AES   ECB over four NIST blocks (ECBVarTxt128, key 0): fault_point 0 after the first block
 *         went in, 1 after two, 2 after three; each block read before the fault is kept in
 *         retention SRAM;
 *   KMAC  SHA3-256 of the NIST block-sized message: fault_point 0 after the start, 1 after half
 *         the message, 2 after all of it was absorbed, before the digest is read;
 *   fault 0: an escalation (eg2_pbf_escalate_now: class A, the engine's recoverable alert raised
 *            through ALERT_TEST, life-cycle escalation to every crypto block, then the
 *            escalation reset); 1: a software reset (rstmgr RESET_REQ).
 * Boot 1: the reset reason and, after an escalation, the alert crash dump (the engine's alert,
 * class A in Phase0); the results read before the fault are the NIST values (no corrupted
 * partial result); nothing of the operation is visible: AES STATUS idle without OUTPUT_VALID and
 * every DATA_OUT word 0 (its reset value), KMAC idle with the STATE window reading 0; then the
 * whole operation again, which must give the NIST values (the engine came back).
 * The wipe while the escalation is in progress (AES and KMAC in their terminal error state, the
 * fatal alert, no output) is seen by the chip bench before the reset: the vseq checks it
 * (chip_sw_eg2_pbf_sym_crypto_fault_vseq).
 *
 * Knobs: engine (0 AES, 1 KMAC, 2 drawn), fault (0 escalation, 1 reset, 2 drawn), fault_point
 * (0-2, 3 drawn). With EG2_PBF_INJECT_FAIL the first value check after the fault expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 */

#include "hw/top/aes_regs.h"   // Generated.
#include "hw/top/kmac_regs.h"  // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

EG2_TEST_CONFIG();

#define SYM_FAULT_PARAMS(X) \
  X(engine, 2, 0, 2)        \
  X(fault, 2, 0, 2)         \
  X(fault_point, 3, 0, 3)
EG2_RM_PARAMS(SYM_FAULT_PARAMS)

enum {
  // Retention words: the choices of boot 0, the alert escalated, the blocks read before the
  // fault (4 words each).
  kRetEngine = 0,
  kRetFault = 1,
  kRetPoint = 2,
  kRetAlert = 3,
  kRetBlocks = 4,
  kRetOut = 5,
  kAesBlocks = 4,
};

static uint32_t pick(uint32_t knob, uint32_t drawn, uint32_t hi) {
  return knob == drawn ? eg2_rand_range(0, hi) : knob;
}

/** X_1..X_4 and Y_1..Y_4 of the 128-bit pairs, contiguous. */
static void aes_vectors(uint8_t *in, uint8_t *exp) {
  for (uint32_t i = 0; i < kAesBlocks; ++i) {
    memcpy(in + 16 * i, eg2_pbf_kat(kEg2PbfKatAes[0].x[i]), 16);
    memcpy(exp + 16 * i, eg2_pbf_kat(kEg2PbfKatAes[0].y[i]), 16);
  }
}

static const eg2_pbf_kat_hash_t *sha3_vector(void) {
  for (uint32_t i = 0; i < sizeof(kEg2PbfKatHash) / sizeof(kEg2PbfKatHash[0]); ++i) {
    if (kEg2PbfKatHash[i].alg == 3 && kEg2PbfKatHash[i].cls == 3) {
      return &kEg2PbfKatHash[i];
    }
  }
  return &kEg2PbfKatHash[0];
}

static void fault_now(uint32_t engine, uint32_t fault) {
  if (fault == 0) {
    eg2_pbf_escalate_now(engine == 0 ? "aes_recov_ctrl_update_err" : "kmac_recov_operation_err",
                         kRetAlert);
  }
  eg2_pbf_sw_reset();
}

static bool boot_0(void) {
  uint32_t engine = pick(EG2_PARAM(engine), 2, 1);
  uint32_t fault = pick(EG2_PARAM(fault), 2, 1);
  uint32_t point = pick(EG2_PARAM(fault_point), 3, 2);
  *eg2_pbf_ret(kRetEngine) = engine;
  *eg2_pbf_ret(kRetFault) = fault;
  *eg2_pbf_ret(kRetPoint) = point;
  eg2_pbf_note(engine == 0 ? "operation AES ECB, fault_point" : "operation SHA3-256, fault_point",
               point);
  eg2_pbf_note(fault == 0 ? "fault: escalation, engine" : "fault: software reset, engine",
               engine);
  if (engine == 0) {
    uint8_t in[16 * kAesBlocks], exp[16 * kAesBlocks], out[16 * kAesBlocks];
    aes_vectors(in, exp);
    if (!eg2_pbf_step("aes init", eg2_pbf_aes_init())) {
      return false;
    }
    uint8_t zero_key[16] = {0};
    uint32_t n = point + 1;
    if (!eg2_pbf_step("aes blocks before the fault",
                      eg2_pbf_aes_partial(128, zero_key, in, n, out))) {
      return false;
    }
    *eg2_pbf_ret(kRetBlocks) = n;
    for (uint32_t i = 0; i < 4 * n; ++i) {
      uint32_t w;
      memcpy(&w, out + 4 * i, 4);
      *eg2_pbf_ret(kRetOut + i) = w;
    }
    // The next block goes in, so the unit is busy when the fault comes.
    if (n < kAesBlocks) {
      uint32_t d[4];
      memcpy(d, in + 16 * n, 16);
      eg2_dev_write32(kEg2DevAes, AES_DATA_IN_0_REG_OFFSET, d[0]);
      eg2_dev_write32(kEg2DevAes, AES_DATA_IN_1_REG_OFFSET, d[1]);
      eg2_dev_write32(kEg2DevAes, AES_DATA_IN_2_REG_OFFSET, d[2]);
      eg2_dev_write32(kEg2DevAes, AES_DATA_IN_3_REG_OFFSET, d[3]);
    }
  } else {
    const eg2_pbf_kat_hash_t *v = sha3_vector();
    if (!eg2_pbf_step("kmac init", eg2_pbf_kmac_init(false))) {
      return false;
    }
    dif_kmac_operation_state_t st;
    if (!eg2_pbf_step("sha3 start",
                      dif_kmac_mode_sha3_start(eg2_pbf_kmac_handle(), &st,
                                               kDifKmacModeSha3Len256) == kDifOk)) {
      return false;
    }
    uint32_t n = point == 0 ? 0 : point == 1 ? v->msg_len / 2 : v->msg_len;
    if (n > 0 && !eg2_pbf_step("sha3 absorbs before the fault",
                               dif_kmac_absorb(eg2_pbf_kmac_handle(), &st, eg2_pbf_kat(v->msg),
                                               n, NULL) == kDifOk)) {
      return false;
    }
    *eg2_pbf_ret(kRetBlocks) = 0;
  }
  fault_now(engine, fault);
  return false;
}

static bool boot_1(void) {
  uint32_t engine = *eg2_pbf_ret(kRetEngine);
  uint32_t fault = *eg2_pbf_ret(kRetFault);
  bool ok = true;
  if (fault == 0) {
    ok = eg2_pbf_escalation_check(kRetAlert);
  } else {
    ok = eg2_pbf_check("reset reason: software reset", 1u << 2, eg2_pbf_reset_reason());
  }
  if (engine == 0) {
    uint8_t in[16 * kAesBlocks], exp[16 * kAesBlocks], out[16 * kAesBlocks];
    aes_vectors(in, exp);
    uint32_t n = *eg2_pbf_ret(kRetBlocks);
    for (uint32_t i = 0; i < 4 * n && n <= kAesBlocks; ++i) {
      uint32_t w = *eg2_pbf_ret(kRetOut + i);
      memcpy(out + 4 * i, &w, 4);
    }
    ok = eg2_pbf_check_bytes("aes blocks read before the fault = NIST", exp, out, 16 * n) && ok;
    uint32_t st = eg2_pbf_aes_status();
    ok = eg2_pbf_check("aes STATUS idle, no output valid, no fatal alert after the reset",
                       1u << AES_STATUS_IDLE_BIT,
                       st & ((1u << AES_STATUS_IDLE_BIT) | (1u << AES_STATUS_OUTPUT_VALID_BIT) |
                             (1u << AES_STATUS_ALERT_FATAL_FAULT_BIT))) &&
         ok;
    uint32_t any = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      any |= eg2_dev_read32(kEg2DevAes, AES_DATA_OUT_0_REG_OFFSET + 4 * i);
    }
    ok = eg2_pbf_check("aes DATA_OUT at its reset value (no partial result)", 0, any) && ok;
    ok = eg2_pbf_step("aes init after the fault", eg2_pbf_aes_init()) && ok;
    uint8_t zero_key[16] = {0};
    ok = eg2_pbf_step("aes whole operation after the fault",
                      eg2_pbf_aes_run(kEg2PbfAesEcb, 128, zero_key, false, false, NULL, in,
                                      kAesBlocks, out)) &&
         ok;
    ok = eg2_pbf_check_bytes("aes after the fault = NIST", exp, out, 16 * kAesBlocks) && ok;
  } else {
    uint32_t st = eg2_dev_read32(kEg2DevKmac, KMAC_STATUS_REG_OFFSET);
    ok = eg2_pbf_check("kmac STATUS sha3_idle, no fatal alert after the reset",
                       1u << KMAC_STATUS_SHA3_IDLE_BIT,
                       st & ((1u << KMAC_STATUS_SHA3_IDLE_BIT) |
                             (1u << KMAC_STATUS_SHA3_ABSORB_BIT) |
                             (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT) |
                             (1u << KMAC_STATUS_ALERT_FATAL_FAULT_BIT))) &&
         ok;
    uint32_t any = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      any |= eg2_dev_read32(kEg2DevKmac, KMAC_STATE_REG_OFFSET + 4 * i);
    }
    ok = eg2_pbf_check("kmac STATE window reads 0 (no partial state)", 0, any) && ok;
    const eg2_pbf_kat_hash_t *v = sha3_vector();
    uint8_t md[32];
    ok = eg2_pbf_step("kmac init after the fault", eg2_pbf_kmac_init(false)) && ok;
    ok = eg2_pbf_step("sha3 whole operation after the fault",
                      eg2_pbf_sha3(256, eg2_pbf_kat(v->msg), v->msg_len, md)) &&
         ok;
    ok = eg2_pbf_check_bytes("sha3 after the fault = NIST", eg2_pbf_kat(v->md), md, 32) && ok;
  }
  eg2_pbf_activity("faults injected", 1);
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("sym_crypto_fault", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  uint32_t boot = eg2_pbf_boot_index();
  if (boot == 0) {
    return boot_0() && ok;
  }
  return boot_1() && ok;
}
