// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_trvk_fault (EG2 spec "Revocation", "Corner cases": the glitch part of
 * chip_sw_cheriot_revbm_errors; REQ_CIB_03). CHERIoT mode only.
 *
 * A single fault in the core's load filter (ibex_trvk, in rv_core_ibex) must be detected wherever
 * the filter sits (REQ_CIB_03): inside the lockstep pair by the comparison, outside it by a
 * countermeasure of its own. The bench (chip_sw_eg2_trvk_fault_vseq) forces one of the filter's
 * signals for one cycle, at a moment where the fault lets a revoked capability reach the core
 * tagged, and watches rv_core_ibex's fatal core error (alert_major_internal, the fatal_hw_err
 * alert). The sites are those of the core bench's core_ibex_trvk_fault_test (`site`, 0-6: verdict,
 * tag_out, bit_select, req_required, out_of_range, sealing, align).
 *
 * The software gives the fault something to act on: `n_slots` tagged capabilities, each based in
 * its own granule of sec SRAM's arena A, stored in arena B; the granules marked in the bitmap
 * (only the bases for the verdict, tag_out and bit_select sites, so that a bitmap word has clear
 * bits for bit_select; every granule of arena A for the sites that skip the lookup, `mark_all`,
 * so that any skipped lookup would have revoked). Then `n_rounds` passes of CLC + CGetTag over
 * the slots (eg2_revoke.S eg2_rv_clc_tag), between the bench steps `trvk_fault_arm` (the bench
 * starts looking for its opportunity among the lookups of capabilities based in arena A) and
 * `trvk_fault_end` (the bench's answer: effective injections, and whether the fault was
 * detected). Every one of those CLCs must return tag 0, except the one the fault let through.
 *
 * Checks (bench answer):
 *  - at least one effective injection (`no effective injection` otherwise: the run shows nothing);
 *  - the CLCs that returned tag 1 are exactly the effective injections (the fault's effect, seen by
 *    the program);
 *  - the fault was detected. If it is, the CPU's fetch is turned off (rv_core_ibex fatal_core_err)
 *    and the software never gets here: the bench ends the run with its own verdict.
 * The bench makes the same detection check (`EG2CHK FAIL TRVK fault ... undetected`). With the
 * TRVK outside the lockstep pair and no check of its own logic (ibex_cheriot_verification
 * status/rtl_tickets.md, "ibex_trvk sits outside the lockstep pair"), every site fails today.
 *
 * Only the stub runs while the marks are set: no capability the test holds is based in arena A.
 *
 * `model_gaps` (eg2sim): no load filter and no bench: the steps are skipped and nothing is checked.
 *
 * Built with EG2_TRVK_FAULT_INJECT_FAIL, one more tagged CLC is expected than the bench's effective
 * injections: the run must fail with `MISMATCH revoked CLCs loaded tagged #0` (with
 * +eg2_inject_fail=1 the bench also fails, with `EG2CHK FAIL TRVK fault took effect`).
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TRVK_FAULT_PARAMS(X) \
  X(site, 0, 0, 6)           \
  X(mark_all, 0, 0, 1)       \
  X(n_slots, 64, 8, 256)     \
  X(n_rounds, 4, 1, 16)      \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TRVK_FAULT_PARAMS);

// eg2_revoke.S.
extern uint32_t eg2_rv_clc_tag(void *slot);

enum {
  kSlot = kEg2ArenaB,
  kGranules = kEg2ArenaSize / 8,
  // The bases, one word per slot, in sec SRAM's work area (not in .data: CHERIoT images keep
  // .data under 0x1000 bytes).
  kBases = kEg2WorkBase,
};

// A switch, not a table of pointers (CHERIoT images: no pointers in const tables).
static const char *site_name(uint32_t site) {
  switch (site) {
    case 0:
      return "verdict";
    case 1:
      return "tag_out";
    case 2:
      return "bit_select";
    case 3:
      return "req_required";
    case 4:
      return "out_of_range";
    case 5:
      return "sealing";
    default:
      return "align";
  }
}

static eg2_tally_t tally;

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool bench = !gaps;
  uint32_t site = EG2_PARAM(site) <= 6 ? EG2_PARAM(site) : 0;
  bool mark_all = EG2_PARAM(mark_all) != 0;
  uint32_t n_slots = EG2_PARAM(n_slots);
  uint32_t n_rounds = EG2_PARAM(n_rounds);
  eg2_puts("EG2 trvk_fault: site ");
  eg2_puts(site_name(site));
  eg2_put_kv(" mark_all", mark_all);
  eg2_put_kv(" n_slots", n_slots);
  eg2_put_kv(" n_rounds", n_rounds);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();

  // The slots: slot s holds a tagged capability based at a granule of arena A. Stored and checked
  // before any mark is set.
  for (uint32_t s = 0; s < n_slots; ++s) {
    uint32_t base = kEg2ArenaA + 8 * eg2_rand_range(0, kGranules - 1);
    eg2_ms_write32(kBases + 4 * s, base);
    eg2_ms_csc(kSlot + 8 * s, eg2_ms_object_cap(base, 8, false));
    if (!eg2_check(&tally, "slot tagged", s, eg2_ms_tag(kSlot + 8 * s), 1)) {
      return false;
    }
  }

  // The marks.
  if (mark_all) {
    uint32_t w0, w1, b;
    if (!eg2_ms_revbm_bit(kEg2ArenaA, &w0, &b) ||
        !eg2_ms_revbm_bit(kEg2ArenaA + kEg2ArenaSize - 8, &w1, &b)) {
      eg2_check(&tally, "arena A in the bitmap", 0, 0, 1);
      return false;
    }
    for (uint32_t w = w0; w <= w1; ++w) {
      eg2_ms_revbm_write(w, 0xffffffffu);
    }
  } else {
    for (uint32_t s = 0; s < n_slots; ++s) {
      eg2_ms_revbm_mark(eg2_ms_read32(kBases + 4 * s), true);
    }
  }

  // The loads, with the bench looking for its opportunity.
  ++tally.planned;
  (void)eg2_ms_bench("trvk_fault_arm", kEg2ArenaA, kEg2ArenaA + kEg2ArenaSize, bench);
  uint32_t n_tagged = 0;
  for (uint32_t r = 0; r < n_rounds; ++r) {
    for (uint32_t s = 0; s < n_slots; ++s) {
      n_tagged += eg2_rv_clc_tag(eg2_mem(kSlot + 8 * s, 8));
    }
  }
  bool answered = eg2_ms_bench("trvk_fault_end", 0, 0, bench);
  uint32_t res = bench && answered ? eg2_ms_bench_result() : 0;
  eg2_ms_revbm_clear();

  // With the bitmap clear the filter passes the stored tag again (REQ_CMS_11), on the chip and in
  // the model. Without a bench (eg2sim) this is the check the inject-fail build breaks, so that
  // build fails there too.
  ++tally.planned;
  uint32_t exp_again = 1;
#ifdef EG2_TRVK_FAULT_INJECT_FAIL
  if (!bench) {
    exp_again = 0;
  }
#endif
  eg2_check(&tally, "slot 0 tagged again with the bitmap clear", 0,
            eg2_rv_clc_tag(eg2_mem(kSlot, 8)), exp_again);
  ++tally.done;

  // {detected[31], effective injections[15:0]} (chip_sw_eg2_trvk_fault_vseq).
  uint32_t effective = res & 0xffffu;
  uint32_t detected = res >> 31;
  eg2_put_kv("EG2 trvk_fault: CLCs loaded tagged", n_tagged);
  eg2_put_kv(" effective injections", effective);
  eg2_put_kv(" detected", detected);
  eg2_puts("\r\n");
  if (bench) {
    eg2_check(&tally, "bench answered", 0, answered ? 1u : 0u, 1u);
    if (effective == 0) {
      eg2_puts("EG2 trvk_fault: no effective injection\r\n");
    }
    eg2_check(&tally, "effective injections > 0", 0, effective != 0 ? 1u : 0u, 1u);
    uint32_t exp_tagged = effective;
#ifdef EG2_TRVK_FAULT_INJECT_FAIL
    exp_tagged += 1;
#endif
    eg2_check(&tally, "revoked CLCs loaded tagged", 0, n_tagged, exp_tagged);
    eg2_check(&tally, "TRVK fault detected (fatal_core_err)", 0, detected, 1);
  } else {
    eg2_puts("EG2 trvk_fault: no bench (model_gaps): nothing checked\r\n");
  }
  ++tally.done;
  eg2_tally_print("trvk_fault", &tally);
  return eg2_tally_ok(&tally);
}
