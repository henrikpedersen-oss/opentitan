// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_otbn_sb (EG2 verification spec REQ_PBF_11, "Post-boot features in CHERIoT
 * mode"): ECDSA P-256 sign and verify on OTBN after the boot stages hand over, behind secure boot
 * or the test ROM, in RISC-V mode and (the `_cheriot` build) in CHERIoT mode, through the crypto
 * library's driver and OTBN program, as the OTBN reference checker drives them on OTBN's ISS
 * (eg2_model/nist/nist_otbn_check.py, `make nist-otbn-ref`).
 *
 * Every expected value is a NIST CAVP value (eg2_pbf_crypto_kat.h; the message digests are
 * computed on the host when the table is generated, as the reference checker prepares them):
 *   sign    SigGen.txt [P-256,SHA-256], the first vector, with its d and k (MODE_SIGN_CONFIG_K):
 *           r || s equal the file's R || S;
 *   verify  SigVer.rsp [P-256,SHA-256] vector `vec` (0-1 valid, 2-3 invalid in the file): the
 *           verdict is the file's Result; with probability `bad_pct` % a valid signature has one
 *           bit of S flipped first and must then not verify.
 * Knobs: op (0 sign, 1 verify, 2 both), vec (0-3, 4 drawn), bad_pct, model_gaps.
 *
 * | Skipped with model_gaps = 1 | Why |
 * |---|---|
 * | every OTBN operation | OTBN is not in the SystemC model (dv_todo decision 6: its reference is
 *   the Python ISS); eg2sim stops the run at CMD EXECUTE. The chip runs and their `_inject_fail`
 *   builds are this test's evidence |
 *
 * Before any operation OTBN must have finished the secure wipe that follows reset (STATUS IDLE,
 * otbn theory of operation): the first value check. With EG2_PBF_INJECT_FAIL it expects bit 0
 * flipped and the run must fail with `PBF FAIL`.
 */

#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_crypto.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_otbn.h"

EG2_TEST_CONFIG();

#define OTBN_SB_PARAMS(X) \
  X(op, 2, 0, 2)          \
  X(vec, 4, 0, 4)         \
  X(bad_pct, 20, 0, 100)  \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(OTBN_SB_PARAMS)

enum {
  kPolls = 2000000,
};

static bool sign(void) {
  const eg2_pbf_kat_ecdsa_t *v = &kEg2PbfKatEcdsa[0];
  uint8_t rs[64], exp[64];
  for (uint32_t i = 0; i < 32; ++i) {
    exp[i] = eg2_pbf_kat(v->r)[i];
    exp[32 + i] = eg2_pbf_kat(v->s)[i];
  }
  bool ok = eg2_pbf_step("otbn ECDSA P-256 sign (MODE_SIGN_CONFIG_K)", eg2_pbf_otbn_sign(v, rs));
  ok = eg2_pbf_check_bytes("otbn signature R || S = NIST SigGen.txt", exp, rs, 64) && ok;
  eg2_pbf_activity("otbn signatures", 1);
  return ok;
}

static bool verify(uint32_t idx) {
  const eg2_pbf_kat_ecdsa_t *v = &kEg2PbfKatEcdsa[1 + idx];
  bool corrupt = v->valid && eg2_rand_range(0, 99) < EG2_PARAM(bad_pct);
  bool expect = v->valid && !corrupt;
  eg2_pbf_note("otbn verify SigVer.rsp vector (0-1 valid, 2-3 invalid)", idx);
  if (corrupt) {
    eg2_pbf_note("otbn verify: one bit of S flipped, must not verify", 1);
  }
  bool valid = false;
  bool ok = eg2_pbf_step("otbn ECDSA P-256 verify runs",
                         eg2_pbf_otbn_verify_start(v, corrupt) && eg2_pbf_otbn_verify_end(&valid));
  ok = eg2_pbf_check("otbn verdict = NIST SigVer.rsp Result (1 valid)", expect, valid) && ok;
  eg2_pbf_activity("otbn verifications", 1);
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("otbn", EG2_PBF_INJECT);
  (void)eg2_pbf_boot_stage();
  (void)eg2_pbf_boot_index();
  ok = eg2_pbf_entropy_up() && ok;
  // STATUS BUSY_SEC_WIPE_INT (4) while the post-reset wipe runs.
  ok = eg2_pbf_step("otbn leaves its post-reset secure wipe", eg2_pbf_otbn_wait_not(0x04, kPolls)) &&
       ok;
  ok = eg2_pbf_check("otbn STATUS IDLE before use", kEg2PbfOtbnIdle, eg2_pbf_otbn_status()) && ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("ECDSA sign and verify on OTBN");
    return ok;
  }
  uint32_t op = EG2_PARAM(op);
  uint32_t vec = EG2_PARAM(vec) == 4 ? eg2_rand_range(0, 3) : EG2_PARAM(vec);
  if (op != 1) {
    ok = sign() && ok;
  }
  if (op != 0) {
    ok = verify(vec) && ok;
  }
  return ok;
}
