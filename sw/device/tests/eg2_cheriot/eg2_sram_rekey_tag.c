// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_sram_rekey_tag (EG2 spec "Memory subsystem in CHERIoT mode"; REQ_CMS_16).
 *
 * No write by a host other than the core's CSC leaves a tag set: an INIT or a key renewal of
 * sram_ctrl_main/sec over a tagged granule leaves tag 0 (RFC CHERIoT Memory Subsystem
 * "Lockstepped CHERIoT Subsystem": "any write by a non-CHERIoT host ... must clear"; RFC CHERIoT
 * in OpenTitan p3-4).
 *
 * `n_caps` capabilities are stored in sec SRAM (`range_class`: one granule, a partial granule's
 * neighbourhood (two capabilities in adjacent granules), many granules, spread over the whole of
 * sec SRAM), and as many in the main SRAM arena, which is not re-keyed. Then sram_ctrl_sec gets
 * `writer`: INIT after a new key (the documented initialisation), or a key renewal alone. Every
 * capability is reloaded:
 *  - in sec SRAM after INIT: no trap, tag 0;
 *  - in sec SRAM after a renewal alone, the old contents are unreadable (sram_ctrl
 *    theory_of_operation.md): the CLC either traps with a load access fault, the integrity error
 *    raising sram_ctrl_sec's fatal_error (declared to the status module), or returns tag 0; never
 *    tag 1;
 *  - in main SRAM: tagged and unchanged.
 * Only sec SRAM is re-keyed: main SRAM holds the test's stack and globals.
 *
 * Expected to fail on the RTL of 2026-10-24: no RTL path clears a tag on INIT or a key renewal
 * (dv_todo, mutation library: "Not expressible as a mutation: REQ_CMS_16"); rtl_tickets F50.
 *
 * The SystemC model keeps tags through INIT and renewal (its sram_ctrl does not touch the meta
 * SRAM): with `model_gaps` sec SRAM's capabilities are not checked.
 *
 * Built with EG2_SRAM_REKEY_TAG_INJECT_FAIL, the first main SRAM capability is expected untagged:
 * the run must fail with `MISMATCH main tag`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define SRAM_REKEY_TAG_PARAMS(X) \
  X(writer, 0, 0, 2)             \
  X(n_caps, 0, 0, 64)            \
  X(range_class, 0, 0, 4)        \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(SRAM_REKEY_TAG_PARAMS);

enum {
  kMaxCaps = 64,
  // The sec SRAM slots' addresses are kept in the main SRAM arena's second half (sec SRAM is wiped).
  kMainSlots = 0,
  kAddrTable = kEg2MainArenaSize / 2,
};

static eg2_tally_t tally;

static uint32_t sec_slot(uint32_t cls, uint32_t i, uint32_t n) {
  switch (cls) {
    case 1:  // one granule
      return kEg2SecBase + 8 * 100;
    case 2:  // a granule and its neighbours
      return kEg2SecBase + 8 * (200 + i);
    case 3:  // many granules, a run of them
      return kEg2SecBase + 8 * (1000 + 3 * i);
    default:  // spread over the whole of sec SRAM
      return kEg2SecBase + 8 * ((i * (kEg2SecSize / 8 - 1)) / (n > 1 ? n - 1 : 1));
  }
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t writer = EG2_PARAM(writer) != 0 ? EG2_PARAM(writer) : eg2_rand_range(1, 2);
  uint32_t cls = EG2_PARAM(range_class);
  if (cls == 0) {
    uint32_t d = eg2_rand_range(0, 99);
    cls = d < 30 ? 1 : d < 50 ? 2 : d < 80 ? 3 : 4;
  }
  uint32_t n = EG2_PARAM(n_caps) != 0 ? EG2_PARAM(n_caps) : eg2_rand_range(1, 64);
  if (cls == 1) {
    n = 1;
  } else if (cls == 2 && n > 3) {
    n = 3;
  }
  eg2_puts(writer == 1 ? "EG2 sram_rekey_tag: INIT" : "EG2 sram_rekey_tag: key renewal");
  eg2_put_kv(" range_class", cls);
  eg2_put_kv(" n_caps", n);
  eg2_puts("\r\n");
  uint32_t arena = eg2_main_arena();
  for (uint32_t i = 0; i < n; ++i) {
    void *c = eg2_ms_object_cap(kEg2MsHeapBase + 8 * eg2_rand_range(0, 1000), 16, false);
    uint32_t s = sec_slot(cls, i, n);
    eg2_ms_csc(s, c);
    eg2_ms_write32(arena + kAddrTable + 4 * i, s);
    eg2_ms_csc(arena + kMainSlots + 8 * i, c);
  }

  int alert = eg2_status_alert_find("sram_ctrl_sec_fatal_error");
  bool done = eg2_ms_sram_ctrl(kEg2DevSramCtrlSecRegs, true, writer == 1);
  eg2_check(&tally, "sram_ctrl_sec operation done", 0, done ? 1 : 0, 1);

  eg2_probe_begin();
  bool integrity_error = false;
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    uint32_t s = eg2_ms_read32(arena + kAddrTable + 4 * i);
    bool trapped = eg2_probe_call(eg2_ms_auth(s), NULL, NULL, eg2_stub_clc);
    void *got = eg2_trap_rec.cap.p;
    if (!gaps) {
      if (trapped) {
        // Only a renewal without INIT may leave words that fail their integrity check.
        eg2_check(&tally, "sec clc mcause", i, eg2_trap_rec.mcause,
                  writer == 2 ? kEg2McauseLoadFault : 0);
        integrity_error = true;
      } else {
        eg2_check(&tally, "sec tag", i, eg2_ms_cap_tag(got), 0);
      }
    }
    ++tally.done;
  }
  eg2_probe_end();
  if (integrity_error && alert >= 0) {
    // What a read of words written under the old key causes (sram_ctrl "Integrity Error
    // Handling"); declared, so the status sweep expects it.
    eg2_status_expect_alert((size_t)alert);
  }
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    uint32_t exp = 1;
#ifdef EG2_SRAM_REKEY_TAG_INJECT_FAIL
    if (i == 0) {
      exp = 0;
    }
#endif
    eg2_check(&tally, "main tag", i, eg2_ms_tag(arena + kMainSlots + 8 * i), exp);
    ++tally.done;
  }
  // sec SRAM usable again for the status sweep and whatever follows.
  (void)eg2_ms_sram_ctrl(kEg2DevSramCtrlSecRegs, false, true);
  eg2_tally_print("sram_rekey_tag", &tally);
  return eg2_tally_ok(&tally);
}
