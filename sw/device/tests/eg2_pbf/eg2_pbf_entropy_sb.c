// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_entropy_sb (EG2 verification spec REQ_PBF_12, "Post-boot features in CHERIoT
 * mode"): csrng's software instance after the boot stages hand over, behind secure boot or the
 * test ROM, in RISC-V mode and (the `_cheriot` build) in CHERIoT mode.
 *
 * The reference is a CTR_DRBG in software (eg2_pbf_crypto.c: AES-256 from FIPS 197, the update,
 * instantiate and generate of SP 800-90A 10.2.1 without derivation function, as csrng's theory of
 * operation specifies), which first must reproduce a NIST CTR_DRBG vector (REQ_PBF_04).
 *
 * The run: the entropy complex in auto mode (the crypto library's setting); then a random
 * command sequence on the software instance with software seeds (flag0 set: csrng's known-answer
 * mode, so every output is predictable): instantiate, `num_cmds` commands drawn from generate
 * (1 to `max_blocks` blocks), reseed, update, and uninstantiate + instantiate, each with
 * additional data of the class `adata_class`; every generated word is compared with the
 * reference. Then an instantiate from entropy_src (flag0 clear) must complete and its two
 * generates must differ (their values against the model fed with the same entropy are the chip
 * bench's: gap); last, a generate after uninstantiate must be refused (csrng.hjson: CMD_STS
 * INVALID_CMD_SEQ, RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT and csrng's recoverable alert,
 * declared to the status module).
 *
 * Knobs: num_cmds (1-16), max_blocks (1-16), adata_class (0 none, 1 1-11 words, 2 12 words,
 * 3 drawn per command), model_gaps.
 *
 * | Skipped with model_gaps = 1 | Why |
 * |---|---|
 * | RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT and the recoverable alert of the refused
 *   generate | eg2sim's csrng answers INVALID_CMD_SEQ but sets neither (model finding) |
 *
 * With EG2_PBF_INJECT_FAIL the first generate's first word is expected with bit 0 flipped: the
 * run must fail with `PBF FAIL`.
 */

#include "hw/top/csrng_regs.h"  // Generated.
#include "hw/top/dt/api.h"      // Generated.
#include "hw/top/dt/csrng.h"    // Generated.
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/dif/dif_csrng_shared.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"

EG2_TEST_CONFIG();

#define ENTROPY_SB_PARAMS(X) \
  X(num_cmds, 8, 1, 16)      \
  X(max_blocks, 16, 1, 16)   \
  X(adata_class, 3, 0, 3)    \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(ENTROPY_SB_PARAMS)

enum {
  kPolls = 200000,
  kMaxBlocks = 16,
  kCmdStsInvalidCmdSeq = 3,
};

static dif_csrng_t csrng;

static bool ready(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    dif_csrng_cmd_status_t s;
    if (dif_csrng_get_cmd_interface_status(&csrng, &s) != kDifOk) {
      return false;
    }
    if (s.kind == kDifCsrngCmdStatusReady) {
      return true;
    }
    if (s.kind == kDifCsrngCmdStatusError) {
      eg2_pbf_note("csrng command status error, SW_CMD_STS",
                   eg2_dev_read32(kEg2DevCsrng, CSRNG_SW_CMD_STS_REG_OFFSET));
      return false;
    }
  }
  return false;
}

static bool sw_cmd(csrng_app_cmd_id_t id, const dif_csrng_seed_material_t *seed) {
  return ready() && csrng_send_app_cmd(csrng.base_addr, kCsrngAppCmdTypeCsrng,
                                       (csrng_app_cmd_t){
                                           .id = id,
                                           .entropy_src_enable = kDifCsrngEntropySrcToggleDisable,
                                           .seed_material = seed,
                                       }) == kDifOk;
}

/** Random seed material of `nwords` words (0-12). */
static void draw(dif_csrng_seed_material_t *m, uint32_t nwords) {
  memset(m, 0, sizeof(*m));
  for (uint32_t i = 0; i < nwords; ++i) {
    m->seed_material[i] = eg2_rand();
  }
  m->seed_material_len = nwords;
}

static uint32_t adata_words(void) {
  uint32_t c = EG2_PARAM(adata_class);
  if (c == 3) {
    c = eg2_rand_range(0, 2);
  }
  return c == 0 ? 0 : c == 1 ? eg2_rand_range(1, 11) : 12;
}

static bool generate(eg2_pbf_drbg_t *ref, uint32_t nblocks, uint32_t *count) {
  dif_csrng_seed_material_t a;
  uint32_t n = adata_words();
  draw(&a, n);
  uint8_t ab[48];
  eg2_pbf_drbg_words_to_bytes(a.seed_material, n, ab);
  uint32_t exp[4 * kMaxBlocks], got[4 * kMaxBlocks];
  eg2_pbf_drbg_generate(ref, n ? ab : NULL, nblocks, exp);
  eg2_pbf_note("csrng generate blocks", nblocks);
  eg2_pbf_note("csrng generate additional data words", n);
  bool ok = eg2_pbf_step("csrng generate runs",
                         ready() &&
                             dif_csrng_generate_start(&csrng, n ? &a : NULL, 4 * nblocks) ==
                                 kDifOk &&
                             dif_csrng_generate_read(&csrng, got, 4 * nblocks) == kDifOk);
  ok = eg2_pbf_check_words("csrng genbits = the CTR_DRBG reference", exp, got, 4 * nblocks) && ok;
  *count += nblocks;
  return ok;
}

static bool instantiate(eg2_pbf_drbg_t *ref) {
  dif_csrng_seed_material_t s;
  uint32_t n = adata_words();
  draw(&s, n);
  uint8_t sb[48];
  eg2_pbf_drbg_words_to_bytes(s.seed_material, n, sb);
  eg2_pbf_drbg_instantiate(ref, sb);
  eg2_pbf_note("csrng instantiate (flag0) seed words", n);
  return eg2_pbf_step("csrng instantiate", sw_cmd(kCsrngAppCmdInstantiate, &s));
}

static bool sequence(void) {
  eg2_pbf_drbg_t ref;
  uint32_t blocks = 0;
  // The software instance is uninstantiated: entropy_testutils restarted csrng.
  bool ok = instantiate(&ref);
  for (uint32_t c = 0; c < EG2_PARAM(num_cmds) && ok; ++c) {
    uint32_t pick = eg2_rand_range(0, 9);
    if (pick < 5) {
      ok = generate(&ref, eg2_rand_range(1, EG2_PARAM(max_blocks)), &blocks) && ok;
    } else if (pick < 7) {
      dif_csrng_seed_material_t s;
      uint32_t n = adata_words();
      draw(&s, n);
      uint8_t sb[48];
      eg2_pbf_drbg_words_to_bytes(s.seed_material, n, sb);
      eg2_pbf_drbg_update(&ref, sb);  // reseed without entropy: update(seed)
      eg2_pbf_note("csrng reseed (flag0) seed words", n);
      ok = eg2_pbf_step("csrng reseed", sw_cmd(kCsrngAppCmdReseed, &s)) && ok;
    } else if (pick < 9) {
      dif_csrng_seed_material_t s;
      uint32_t n = adata_words();
      draw(&s, n);
      uint8_t sb[48];
      eg2_pbf_drbg_words_to_bytes(s.seed_material, n, sb);
      eg2_pbf_drbg_update(&ref, sb);
      eg2_pbf_note("csrng update words", n);
      ok = eg2_pbf_step("csrng update", ready() && dif_csrng_update(&csrng, &s) == kDifOk) && ok;
    } else {
      ok = eg2_pbf_step("csrng uninstantiate",
                        ready() && dif_csrng_uninstantiate(&csrng) == kDifOk) &&
           ok;
      ok = instantiate(&ref) && ok;
    }
  }
  // A final generate, so every sequence is checked at least once.
  ok = generate(&ref, eg2_rand_range(1, EG2_PARAM(max_blocks)), &blocks) && ok;
  eg2_pbf_activity("csrng blocks compared", blocks);
  return ok;
}

static bool refused_after_uninstantiate(void) {
  bool ok = eg2_pbf_step("csrng uninstantiate",
                         ready() && dif_csrng_uninstantiate(&csrng) == kDifOk);
  ok = ready() && ok;
  int alert = eg2_status_alert_find("csrng_recov_alert");
  if (alert >= 0 && !EG2_PARAM(model_gaps)) {
    eg2_status_expect_alert((size_t)alert);
  }
  // A generate with nothing instantiated: acknowledged with INVALID_CMD_SEQ.
  eg2_dev_write32(kEg2DevCsrng, CSRNG_CMD_REQ_REG_OFFSET,
                  csrng_cmd_header_build(kCsrngAppCmdGenerate, kDifCsrngEntropySrcToggleEnable,
                                         0, 1));
  uint32_t sts = 0;
  for (uint32_t i = 0; i < kPolls; ++i) {
    sts = eg2_dev_read32(kEg2DevCsrng, CSRNG_SW_CMD_STS_REG_OFFSET);
    if (sts & (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) {
      break;
    }
  }
  ok = eg2_pbf_check("csrng generate after uninstantiate: CMD_STS INVALID_CMD_SEQ",
                     kCmdStsInvalidCmdSeq,
                     (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK) &&
       ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT and csrng_recov_alert");
    return ok;
  }
  // csrng.hjson RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT: set, and the recoverable alert.
  uint32_t rec = eg2_dev_read32(kEg2DevCsrng, CSRNG_RECOV_ALERT_STS_REG_OFFSET);
  ok = eg2_pbf_check("csrng RECOV_ALERT_STS.CMD_STAGE_INVALID_CMD_SEQ_ALERT", 1,
                     (rec >> CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT) & 1) &&
       ok;
  ok = eg2_pbf_step("csrng_recov_alert reached the alert handler",
                    alert >= 0 && eg2_status_wait_alert((size_t)alert, 10000)) &&
       ok;
  // 2026-10-26: csrng.hjson's cs_hw_inst_exc is "asserted when a hardware-attached CSRNG instance
  // encounters a command exception" (HW_EXC_STS: "to monitor the status of requests made to the SW
  // instance, check the SW_CMD_STS register"), so this software-instance error must not set it.
  // Checked on INTR_STATE so a set bit is named (F49), not only seen by the status sweep. Not on
  // HW_EXC_STS: the RTL rewrites it every cycle, so it shows an exception only in that cycle and
  // reads 0 afterwards (F55).
  uint32_t intr = eg2_dev_read32(kEg2DevCsrng, CSRNG_INTR_STATE_REG_OFFSET);
  ok = eg2_pbf_check("csrng INTR_STATE.CS_HW_INST_EXC after a software-instance error", 0,
                     (intr >> CSRNG_INTR_STATE_CS_HW_INST_EXC_BIT) & 1) &&
       ok;
  eg2_dev_write32(kEg2DevCsrng, CSRNG_RECOV_ALERT_STS_REG_OFFSET,
                  ~(1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT));
  // The interrupts this test's own software commands raise (csrng.hjson: cs_cmd_req_done when a
  // command completes, cs_entropy_req when csrng asks entropy_src for entropy), and
  // cs_hw_inst_exc once checked above: cleared, so the final status sweep reports only what the
  // test did not cause.
  eg2_dev_write32(kEg2DevCsrng, CSRNG_INTR_STATE_REG_OFFSET,
                  (1u << CSRNG_INTR_STATE_CS_CMD_REQ_DONE_BIT) |
                      (1u << CSRNG_INTR_STATE_CS_ENTROPY_REQ_BIT) |
                      (1u << CSRNG_INTR_STATE_CS_HW_INST_EXC_BIT));
  return ok;
}

static bool from_entropy_src(void) {
  // The sequence may have left the instance instantiated: an instantiate would be refused.
  bool ok = eg2_pbf_step("csrng uninstantiate", ready() && dif_csrng_uninstantiate(&csrng) == kDifOk);
  dif_csrng_seed_material_t none;
  memset(&none, 0, sizeof(none));
  bool inst = ready() &&
              dif_csrng_instantiate(&csrng, kDifCsrngEntropySrcToggleEnable, &none) == kDifOk &&
              ready();
  ok = eg2_pbf_step("csrng instantiate from entropy_src", inst) && ok;
  uint32_t a[8], b[8];
  // A generate on an instance that is not instantiated would wait for genbits forever.
  ok = eg2_pbf_step("csrng generate from entropy",
                    inst && dif_csrng_generate_start(&csrng, NULL, 8) == kDifOk &&
                        dif_csrng_generate_read(&csrng, a, 8) == kDifOk && ready() &&
                        dif_csrng_generate_start(&csrng, NULL, 8) == kDifOk &&
                        dif_csrng_generate_read(&csrng, b, 8) == kDifOk) &&
       ok;
  ok = eg2_pbf_step("csrng entropy-seeded generates differ", memcmp(a, b, sizeof(a)) != 0) && ok;
  eg2_pbf_gap("entropy-seeded csrng output against the CTR_DRBG model",
              "software does not know entropy_src's seed; the chip bench's model fed the same "
              "noise does (scoreboard)");
  ok = eg2_pbf_step("csrng uninstantiate", ready() && dif_csrng_uninstantiate(&csrng) == kDifOk) &&
       ok;
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("entropy", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  (void)eg2_pbf_boot_index();
  if (!eg2_pbf_drbg_selftest()) {
    return false;
  }
  ok = eg2_pbf_entropy_up() && ok;
  ok = eg2_pbf_step("csrng init", dif_csrng_init_from_dt(kDtCsrng, &csrng) == kDifOk) && ok;
  ok = sequence() && ok;
  ok = from_entropy_src() && ok;
  ok = refused_after_uninstantiate() && ok;
  return ok;
}
