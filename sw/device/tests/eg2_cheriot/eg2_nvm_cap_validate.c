// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_nvm_cap_validate (EG2 spec "Memory, tags and devices"; REQ_CMS_17).
 *
 * A CSC to RRAM sets the tag when the validation matches and answers a mismatch with a bus error
 * and no alert; a sweep over RRAM clears revoked capabilities (RFC CHERIoT Memory Subsystem
 * "Verification Strategy"; cheriot_mem_sys theory_of_operation.md "Write-to-Read-and-Compare
 * Filter": the store is not written but compared with the NVM's words; "A capability store that
 * fails leaves the tag as it was and changes nothing else. Its answer is a bus error, never an
 * alert").
 *
 * `n_stores` slots in the test's RRAM area, one capability per 16-byte RRAM word. Each slot's
 * capability is programmed through rram_ctrl; half the slots are first tagged by a matching CSC
 * (so a failed store must leave a tag 1 as well as a tag 0 unchanged). Then each slot gets one CSC:
 * `match_pct` of them the programmed capability, the rest one that differs in its address word,
 * its high word or both. Expected: a match takes no trap and leaves the slot tagged with the
 * programmed words; a mismatch traps with a store access fault, and the slot's tag and words are
 * as before. No alert (the framework's status sweep). Then the granules some slots' capabilities
 * are based in are marked, the slots swept by the revocation engine, the bitmap cleared, and every
 * slot reloaded: the swept revoked ones untagged, the rest as they were.
 *
 * The SystemC model writes RRAM through the host window and has no revoking engine (dv_todo G2;
 * eg2_mpact_cpu.h): with `model_gaps` the mismatched stores and the revoked slots are not checked.
 *
 * Built with EG2_NVM_CAP_VALIDATE_INJECT_FAIL, the sweep is expected to advance the epoch by 4: the
 * run must fail with `MISMATCH epoch`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define NVM_CAP_VALIDATE_PARAMS(X) \
  X(n_stores, 0, 0, 256)           \
  X(match_pct, 101, 0, 101)        \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(NVM_CAP_VALIDATE_PARAMS);

enum {
  kMaxStores = 256,
  kObjects = kEg2ArenaA,
  // Per slot in the work area: expected tag, then whether it was a match.
  kExpTag = kEg2WorkBase,
  kExpMatch = kEg2WorkBase + 4 * kMaxStores,
  kRevoked = kEg2WorkBase + 8 * kMaxStores,
};

static eg2_tally_t tally;

static uint32_t slot(uint32_t i) { return kEg2MsRramArea + 16 * i; }

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init() || !eg2_ms_rram_setup()) {
    eg2_puts("EG2 set-up failed\r\n");
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n = EG2_PARAM(n_stores) != 0 ? EG2_PARAM(n_stores) : eg2_rand_range(8, 256);
  uint32_t pct = EG2_PARAM(match_pct);
  if (pct == 101) {
    uint32_t d = eg2_rand_range(0, 9);
    pct = d == 0 ? 0 : d == 1 ? 100 : eg2_rand_range(0, 100);
  }
  eg2_put_kv("EG2 nvm_cap_validate n_stores", n);
  eg2_put_kv(" match_pct", pct);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();

  eg2_probe_begin();
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    uint32_t a = slot(i);
    void *c = eg2_ms_object_cap(kObjects + 8 * (i % 1024), eg2_rand_range(1, 64), false);
    uint32_t w[4] = {eg2_ms_cap_addr(c), eg2_ms_cap_high(c), 0, 0};
    uint32_t err;
    if (!eg2_ms_rram_program(a, w, 4, &err)) {
      eg2_ms_note("rram_ctrl program failed, ERR_CODE", err);
      eg2_probe_end();
      return false;
    }
    uint32_t tag = 0;
    if (eg2_pick(2)) {
      bool t = eg2_probe_call(eg2_ms_auth(a), c, NULL, eg2_stub_csc);
      eg2_check(&tally, "pre-tag csc trapped", i, t ? 1 : 0, 0);
      tag = 1;
    }
    bool match = eg2_rand_range(0, 99) < pct;
    void *s = c;
    if (!match) {
      uint32_t kind = eg2_pick(3);
      if (kind != 1) {
        // Another address in bounds: the address word differs.
        __asm__ volatile("cincoffsetimm %0, %0, 1" : "+C"(s));
      }
      if (kind != 0) {
        // Fewer permissions: the high word differs.
        __asm__ volatile("candperm %0, %0, %1" : "+C"(s) : "r"(~(uint32_t)kEg2PermGl));
      }
    }
    bool trapped = eg2_probe_call(eg2_ms_auth(a), s, NULL, eg2_stub_csc);
    uint32_t mcause = trapped ? eg2_trap_rec.mcause : 0;
    if (match) {
      eg2_check(&tally, "match csc trapped", i, mcause, 0);
      tag = 1;
    } else if (!gaps) {
      eg2_check(&tally, "mismatch csc mcause", i, mcause, kEg2McauseStoreFault);
    }
    eg2_ms_write32(kExpTag + 4 * i, tag);
    eg2_ms_write32(kExpMatch + 4 * i, match ? 1 : 0);
    if (match || !gaps) {
      void *got = eg2_ms_clc(a);
      eg2_check(&tally, "tag", i, eg2_ms_cap_tag(got), tag);
      eg2_check(&tally, "lo", i, eg2_ms_cap_addr(got), w[0]);
      eg2_check(&tally, "hi", i, eg2_ms_cap_high(got), w[1]);
    }
    ++tally.done;
  }
  eg2_probe_end();
  eg2_tally_print("validation", &tally);

  // Revoke some: mark the objects of a third of the slots, sweep the slots, clear the marks.
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t r = eg2_pick(3) == 0 ? 1 : 0;
    eg2_ms_write32(kRevoked + 4 * i, r);
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (eg2_ms_read32(kRevoked + 4 * i)) {
      eg2_ms_revbm_mark(eg2_ms_cap_base(eg2_ms_clc(slot(i))), true);
    }
  }
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(slot(0), 2 * n, &sw);
  eg2_ms_tbre_wait(&sw, 10000000);
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  eg2_check(&tally, "sweep started", 0, sw.started ? 1 : 0, 1);
  eg2_check(&tally, "sweep done", 0, sw.done ? 1 : 0, 1);
  uint32_t exp_epoch = sw.epoch_before + 2;
#ifdef EG2_NVM_CAP_VALIDATE_INJECT_FAIL
  exp_epoch += 2;
#endif
  eg2_check(&tally, "epoch", 0, sw.epoch_after, exp_epoch);
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    bool match = eg2_ms_read32(kExpMatch + 4 * i) != 0;
    uint32_t tag = eg2_ms_read32(kExpTag + 4 * i);
    // A revoked slot shares its object with others when n > 1024: any slot based on a marked
    // granule is revoked.
    uint32_t base = eg2_ms_cap_base(eg2_ms_clc(slot(i)));
    uint32_t wd, bt;
    bool marked = false;
    for (uint32_t j = 0; j < n && !marked; ++j) {
      marked = eg2_ms_read32(kRevoked + 4 * j) &&
               eg2_ms_cap_base(eg2_ms_clc(slot(j))) == base && eg2_ms_revbm_bit(base, &wd, &bt);
    }
    bool revoked = marked && tag;
    if ((match || !gaps) && !(gaps && revoked)) {
      eg2_check(&tally, revoked ? "revoked tag" : "kept tag", i, eg2_ms_tag(slot(i)),
                revoked ? 0 : tag);
    }
    ++tally.done;
  }
  eg2_tally_print("nvm_cap_validate", &tally);
  return eg2_tally_ok(&tally);
}
