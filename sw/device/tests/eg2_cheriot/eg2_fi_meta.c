// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_cheriot_fi_meta (EG2 spec "Memory subsystem in CHERIoT mode"; REQ_CMS_18): a fault
 * campaign on the paths of tags and revocation bits.
 *
 * With CHERIoT on, end-to-end integrity still detects corruption, and a fault on the tag sideband,
 * on the bitmap bus or on the meta SRAM port never turns a tag 0 into 1 or clears a revocation bit
 * undetected (RFC CHERIoT in OpenTitan p4; RFC CHERIoT Memory Subsystem "Security
 * Considerations", "Verification Strategy").
 *
 * `campaign_size` rounds. Each round stores capabilities (tag 1) and integer words (tag 0) in sec
 * SRAM's arena B, marks the granule some of the capabilities are based in, and asks the bench for
 * one fault (`fi <round> <meta word of the first slot's tag>`): the bench draws the site (command
 * integrity, data integrity, the tag sideband, a bitmap response, the meta SRAM port), the
 * direction, the bit and the moment, and corrupts one transfer while the round's loads run. The
 * round then reloads every slot:
 *  - an integer granule never comes back tagged; a capability based in a marked granule never
 *    comes back tagged (a cleared revocation bit or a lost lookup);
 *  - any other difference (a capability untagged, a word changed, a trap) must come with an alert:
 *    the alerts of the paths (cheriot_mem_sys fatal_fault, sram_ctrl_meta and sram_ctrl_sec
 *    fatal_error, rv_core_ibex fatal_hw_err) escalate to a reset (class A), and the boot after it
 *    finds the round's alert in rstmgr's crash dump. A round without a difference may have an
 *    alert too (a detected fault on a transfer whose result is not used).
 * The round's results go to the boot record as they are made, so a reset in the middle of a round
 * loses nothing. The campaign continues from the next round on the boot after a reset.
 *
 * Liveness: rounds completed (or ended by a detected fault) against `campaign_size`.
 *
 * Without a bench (`model_gaps`) no fault is injected: every round must be clean.
 *
 * Built with EG2_FI_META_INJECT_FAIL, one more round is planned than run: the run must fail with
 * `EG2 fi_meta: <n>/<n+1> done`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define FI_META_PARAMS(X)          \
  X(campaign_size, 0, 0, 1000)     \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(FI_META_PARAMS);

enum {
  kCaps = 8,
  kInts = 8,
  kSlots = kEg2ArenaB,
  kObjects = kEg2ArenaA,
  // Boot record words.
  kRecRound = 0,       // the round in progress (0: none yet)
  kRecDone = 1,        // rounds finished
  kRecDetected = 2,    // rounds ended by an escalation
  kRecViolations = 3,  // tagged results that must not be
  kRecUnexplained = 4, // differences without an alert
  kRecInRound = 5,     // 1 while a round runs
  kRecDiff = 6,        // the running round's differences so far
  kRecArmed = 7,
};

static eg2_tally_t tally;

static uint32_t alert_ids(int *a) {
  a[0] = eg2_status_alert_find("cheriot_mem_sys_fatal_fault");
  a[1] = eg2_status_alert_find("sram_ctrl_meta_fatal_error");
  a[2] = eg2_status_alert_find("sram_ctrl_sec_fatal_error");
  a[3] = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  return 4;
}

static void round_run(eg2_ms_rec_t *rec, uint32_t round, bool bench) {
  rec->data[kRecRound] = round;
  rec->data[kRecDiff] = 0;
  rec->data[kRecInRound] = 1;
  eg2_ms_revbm_clear();
  // Capabilities in even slots, integers in odd ones; objects 0..kCaps-1, object 0 marked.
  for (uint32_t i = 0; i < kCaps + kInts; ++i) {
    uint32_t a = kSlots + 8 * i;
    if (i % 2 == 0) {
      eg2_ms_csc(a, eg2_ms_object_cap(kObjects + 8 * (i / 2), 8, false));
    } else {
      eg2_ms_write32(a, 0x5a5a0000u + i);
      eg2_ms_write32(a + 4, 0xa5a50000u + i);
    }
  }
  eg2_ms_revbm_mark(kObjects, true);
  if (!eg2_ms_bench("fi", round, eg2_ms_tag_offset(kSlots) / 4, bench)) {
    rec->data[kRecUnexplained]++;
    return;
  }
  eg2_probe_begin();
  for (uint32_t i = 0; i < kCaps + kInts; ++i) {
    uint32_t a = kSlots + 8 * i;
    bool trapped = eg2_probe_call(eg2_ms_auth(a), NULL, NULL, eg2_stub_clc);
    void *c = eg2_trap_rec.cap.p;
    if (trapped) {
      rec->data[kRecDiff]++;
      continue;
    }
    uint32_t tag = eg2_ms_cap_tag(c);
    bool cap = i % 2 == 0;
    bool revoked = cap && i == 0;
    if (revoked && !bench) {
      continue;  // the model has no revocation load filter (eg2_mpact_cpu.h)
    }
    if (tag && (!cap || revoked)) {
      rec->data[kRecViolations]++;
    }
    bool same = cap ? (tag == (revoked ? 0u : 1u) &&
                       eg2_ms_cap_base(c) == kObjects + 8 * (i / 2))
                    : (eg2_ms_cap_addr(c) == 0x5a5a0000u + i &&
                       eg2_ms_cap_high(c) == 0xa5a50000u + i);
    if (!same) {
      rec->data[kRecDiff]++;
    }
  }
  eg2_probe_end();
  eg2_ms_revbm_clear();
  // A difference must bring an alert, which escalates: wait for the reset a while.
  if (rec->data[kRecDiff] != 0) {
    eg2_ms_spin(200000);
    rec->data[kRecUnexplained]++;
  }
  rec->data[kRecInRound] = 0;
  rec->data[kRecDone]++;
}

bool eg2_test_main(void) {
  bool bench = EG2_PARAM(model_gaps) == 0;
  eg2_ms_rec_t *rec = eg2_ms_rec_boot();
  if (!eg2_sec_sram_init()) {
    return false;
  }
  uint32_t total = EG2_PARAM(campaign_size) != 0 ? EG2_PARAM(campaign_size)
                                                 : (bench ? eg2_rand_range(100, 1000) : 8);
  int alerts[4];
  uint32_t na = alert_ids(alerts);
  if (rec->boot > 1 && rec->data[kRecInRound]) {
    // The round in progress ended with a reset: it must have been the escalation of a detected
    // fault.
    bool detected = false;
    for (uint32_t i = 0; i < na; ++i) {
      detected = detected || (alerts[i] >= 0 && eg2_ms_crashdump_cause((size_t)alerts[i]));
    }
    bool esc = (eg2_ms_reset_reason() & kEg2MsResetEscalation) != 0;
    eg2_check(&tally, "reset after a round is an escalation of its alert", rec->data[kRecRound],
              esc && detected ? 1 : 0, 1);
    rec->data[kRecDetected]++;
    rec->data[kRecDone]++;
    rec->data[kRecInRound] = 0;
  }
  for (uint32_t i = 0; i < na; ++i) {
    if (alerts[i] >= 0) {
      eg2_ms_escalation_arm((size_t)alerts[i]);
    }
  }
  while (rec->data[kRecDone] < total) {
    round_run(rec, rec->data[kRecDone] + 1, bench);
  }
  tally.planned = total;
  tally.done = rec->data[kRecDone];
#ifdef EG2_FI_META_INJECT_FAIL
  ++tally.planned;
#endif
  eg2_put_kv("EG2 fi_meta rounds", total);
  eg2_put_kv(" detected", rec->data[kRecDetected]);
  eg2_puts("\r\n");
  eg2_check(&tally, "tagged results that must be untagged", 0, rec->data[kRecViolations], 0);
  eg2_check(&tally, "differences without an alert", 0, rec->data[kRecUnexplained], 0);
  eg2_tally_print("fi_meta", &tally);
  return eg2_tally_ok(&tally);
}
