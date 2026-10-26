// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_fault (EG2 spec "Revocation", "Corner cases"; REQ_CMS_12, REQ_CMS_18; the
 * SoC-level test the core specification's VP_REV_11 requires).
 *
 * The bench injects one fault into a sweep (tbre_fault_arm, chip_sw_eg2_tbre_fault_vseq), then the
 * firmware reads the failure from the registers and starts a new sweep. Sixteen slots in sec SRAM's
 * arena B, each a tagged capability based on an object in arena A, one object per bitmap word, so
 * that each of the TBRE's bitmap lookups belongs to one slot. The fault hits slot t (2-13):
 *
 *  - word_err  (always first): the response to the TBRE's read of one word of slot t (lower or
 *              upper) answers d_error with correct integrity. Every object is marked, so a sweep
 *              without the error would untag every slot.
 *  - word_intg: the same response with one bit of its response or data integrity flipped.
 *  - bm_err:   the response to the TBRE's bitmap lookup for slot t's object answers d_error;
 *              nothing is marked.
 *  - bm_intg:  that response with one integrity bit flipped.
 * `fault` selects the second case (1 word_intg, 2 bm_err, 3 bm_intg; 0 draws one); each run
 * does word_err first, as it raises no alert.
 *
 * Checks (cheriot_mem_sys theory_of_operation.md "Error Handling", "Revocation Engine";
 * programmers_guide.md "Errors"; registers.md TBRE_STATUS, TBRE_EPOCH):
 *  - the bench's injection took effect (its answer);
 *  - the sweep ends (busy falls) with TBRE_STATUS.sweep_err, and TBRE_EPOCH is unchanged;
 *  - on a swept word, slot t keeps its tag; cheriot_mem_sys's fatal_fault alert reaches the
 *    alert handler for the integrity fault and not for the error response ("Error response to a
 *    read of the revocation engine: word not cleared, sweep_err"). The other slots are not
 *    checked: "a sweep with an error may have left revoked capabilities tagged";
 *  - on the bitmap lookup, slot t loses its tag (counted as revoked) and the others, unmarked,
 *    keep theirs; fatal_fault raised for the error and the integrity fault alike;
 *  - after writing 1 to sweep_err, the next sweep over the same slots (the same marks) ends
 *    without an error, the epoch advances by 2, and every slot holds the tag the expression gives.
 * The fatal alert latches until reset: once raised, it is declared to the status module.
 * The marks are set only inside op windows (eg2_revoke.h).
 *
 * `model_gaps` (eg2sim): no bench and no revoking TBRE: the injected cases' checks and the revoked
 * tags are skipped; the sweeps and the epoch are checked.
 *
 * Built with EG2_TBRE_FAULT_INJECT_FAIL, the epoch after the first recovery sweep is expected to
 * have advanced by 4: the run must fail with `MISMATCH epoch after the next sweep`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TBRE_FAULT_PARAMS(X) \
  X(fault, 0, 0, 3)          \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TBRE_FAULT_PARAMS);

enum {
  kSlots = kEg2ArenaB,
  kNum = 16,
  kOps = kEg2WorkBase,
  kMaxOps = 64,
  kOut = kOps + 12 * kMaxOps,
  kSweepErr = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_SWEEP_ERR_BIT,
  kBusy = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT,
};

/** The bench's fault kinds (tbre_fault_arm's first operand). */
typedef enum fault_kind {
  kWordErr = 0,
  kWordIntg = 1,
  kBmErr = 2,
  kBmIntg = 3,
} fault_kind_t;

static const char *const kFaultName[4] = {"word_err", "word_intg", "bm_err", "bm_intg"};

static eg2_tally_t tally;
static bool gaps, bench;
static int alert;
static bool alert_seen;

static uint32_t obj_addr(uint32_t i) { return kEg2ArenaA + 256 * i; }
static uint32_t slot_addr(uint32_t i) { return kSlots + 8 * i; }

/** One window: the marks, a sweep over the slots, the wait, the marks cleared. */
static uint32_t sweep_window(bool marked, uint32_t *status_out) {
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  for (uint32_t i = 0; i < kNum && marked; ++i) {
    uint32_t a = obj_addr(i);
    eg2_rv_op_revbm(&o, (a - kEg2MsHeapBase) / 256, 1u << (((a - kEg2MsHeapBase) / 8) % 32));
  }
  eg2_rv_op_sweep(&o, kSlots, kNum);
  uint32_t wait = eg2_rv_op_sweep_wait(&o);
  for (uint32_t i = 0; i < kNum && marked; ++i) {
    eg2_rv_op_revbm(&o, (obj_addr(i) - kEg2MsHeapBase) / 256, 0);
  }
  eg2_rv_ops_run(&o, kOut);
  *status_out = eg2_ms_tbre_status();
  return eg2_ms_read32(kOut + 4 * wait);
}

static bool fill(void) {
  for (uint32_t i = 0; i < kNum; ++i) {
    eg2_ms_csc(slot_addr(i), eg2_ms_object_cap(obj_addr(i), 8, false));
    if (!eg2_check(&tally, "slot tagged", i, eg2_ms_tag(slot_addr(i)), 1)) {
      return false;
    }
  }
  return true;
}

static void one_case(fault_kind_t kind, uint32_t idx) {
  ++tally.planned;
  if (!fill()) {
    return;
  }
  bool on_word = kind == kWordErr || kind == kWordIntg;
  uint32_t t = eg2_rand_range(2, kNum - 3);
  uint32_t target = on_word ? slot_addr(t) + 4 * eg2_pick(2) : obj_addr(t);
  eg2_put_kv("EG2 tbre_fault case", idx);
  eg2_puts(" ");
  eg2_puts(kFaultName[kind]);
  eg2_put_kv(" slot", t);
  eg2_put_kv(" target", target);
  eg2_puts("\r\n");

  uint32_t epoch0 = eg2_ms_tbre_epoch();
  (void)eg2_ms_bench("tbre_fault_arm", kind, target, bench);
  uint32_t status;
  uint32_t wait = sweep_window(on_word, &status);
  (void)eg2_ms_bench("tbre_fault_end", 0, 0, bench);
  uint32_t hit = bench ? eg2_ms_bench_result() : 0;

  eg2_check(&tally, "sweep ended", idx, wait != 0 ? 1u : 0u, 1u);
  if (!gaps) {
    eg2_check(&tally, "injection took effect", idx, hit & 1, 1);
    eg2_check(&tally, "TBRE_STATUS after the faulted sweep", idx, status & (kSweepErr | kBusy),
              kSweepErr);
    eg2_check(&tally, "epoch after the faulted sweep", idx, eg2_ms_tbre_epoch(), epoch0);
    if (on_word) {
      eg2_check(&tally, "faulted slot keeps its tag", idx, eg2_ms_tag(slot_addr(t)), 1);
    } else {
      for (uint32_t i = 0; i < kNum; ++i) {
        eg2_check(&tally, i == t ? "slot of the failed lookup revoked" : "unmarked slot's tag",
                  idx * kNum + i, eg2_ms_tag(slot_addr(i)), i == t ? 0u : 1u);
      }
    }
    bool raised = kind != kWordErr;
    if (alert >= 0) {
      bool got = eg2_status_wait_alert((size_t)alert, raised ? 100000 : 2000);
      eg2_check(&tally, "fatal_fault alert", idx, got, raised);
      if (got) {
        alert_seen = true;
        eg2_status_expect_alert((size_t)alert);
      }
    }
  }

  // The firmware clears the error and sweeps again, with the same marks.
  eg2_ms_tbre_clear();
  uint32_t epoch1 = eg2_ms_tbre_epoch();
  wait = sweep_window(on_word, &status);
  eg2_check(&tally, "next sweep ended", idx, wait != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "TBRE_STATUS after the next sweep", idx, status, 0);
  uint32_t exp_epoch = epoch1 + 2;
#ifdef EG2_TBRE_FAULT_INJECT_FAIL
  if (idx == 0) {
    exp_epoch += 2;
  }
#endif
  eg2_check(&tally, "epoch after the next sweep", idx, eg2_ms_tbre_epoch(), exp_epoch);
  if (!gaps) {
    for (uint32_t i = 0; i < kNum; ++i) {
      // Marked (word cases): every slot untagged now. Unmarked (bitmap cases): slot t stays
      // untagged from the faulted sweep, the others tagged.
      uint32_t exp = on_word ? 0 : (i == t ? 0 : 1);
      eg2_check(&tally, "tag after the next sweep", idx * kNum + i, eg2_ms_tag(slot_addr(i)), exp);
    }
  }
  eg2_ms_tbre_clear();
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  bench = !gaps;
  fault_kind_t second =
      (fault_kind_t)(EG2_PARAM(fault) != 0 ? EG2_PARAM(fault) : eg2_rand_range(1, 3));
  alert = eg2_status_alert_find("cheriot_mem_sys_fatal_fault");
  eg2_check(&tally, "fatal_fault alert known", 0, alert >= 0, 1);
  eg2_puts("EG2 tbre_fault second case: ");
  eg2_puts(kFaultName[second]);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  one_case(kWordErr, 0);
  one_case(second, 1);
  eg2_put_kv("EG2 tbre_fault alert seen", alert_seen);
  eg2_puts("\r\n");
  eg2_tally_print("tbre_fault", &tally);
  return eg2_tally_ok(&tally);
}
