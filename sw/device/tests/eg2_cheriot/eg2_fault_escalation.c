// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Three tests of a fault the bench injects, each escalated to a reset so that the boot after it
 * judges (EG2 spec "Memory subsystem in CHERIoT mode"):
 *
 *  - chip_sw_cheriot_meta_intg_escalation (EG2_FAULT_META, REQ_CMS_13): a real fault in the meta
 *    SRAM (data or ECC bit of a tag word or a bitmap word) raises cheriot_mem_sys's fatal_fault,
 *    which escalates as configured; a CLC over the faulty word returns tag 0 (RFC CHERIoT Memory
 *    Subsystem "Security Considerations"; cheriot_mem_sys theory_of_operation.md "Error
 *    Handling": an integrity fault on a meta SRAM response raises fatal_fault).
 *  - chip_sw_rv_core_ibex_cheriot_revbm_intg (EG2_FAULT_REVBM, REQ_CMS_18): a bitmap response
 *    with corrupted integrity: the load returns untagged and fatal_hw_err fires.
 *  - chip_sw_cheriot_bus_intg (EG2_FAULT_BUS, REQ_CMS_18, REQ_TL_04): command or data integrity of
 *    the core's data bus, or the tag sideband, corrupted on one transfer: the receiver's alert, or
 *    tag 0; never a tagged result from a corrupted transfer.
 *  - chip_sw_rv_core_ibex_alerts_cheriot (EG2_FAULT_RF, REQ_CIB_02): one bit of a register's
 *    storage flipped in CHERIoT mode (`fault_site`: 1 the data word in the main register file, 2
 *    the shadow register file's copy, the 7-bit ECC of the data or of the metadata, 3 the
 *    capability metadata, 4 the tag; `fault_reg` x1-x15; `fault_bit` within the site;
 *    `fault_time` cycles after the bench step) while the test reads every register, as an integer
 *    (`beq`) and as a capability (`csetequalexact`): the major internal alert, i.e. rv_core_ibex's
 *    fatal_hw_err, escalates. The tag is covered by the metadata's ECC, not duplicated in the
 *    shadow register file as RFC CHERIoT Ibex describes (rtl_tickets.md F30). No retirement with
 *    the faulty value is the scoreboard's check.
 *
 * Boot 1: the alerts the fault may raise go to class A, which escalates to a reset on the first
 * one (escalation signal 3) with the crash dump captured; a capability is stored in sec SRAM; a
 * bench step injects the fault (`meta_flip <meta word> <site/bits>`, `revbm_corrupt <0> <field>`,
 * `bus_corrupt <site> <direction>`); the test makes its `access` (CLC, CSC, or a sweep over the
 * capability) and records what it saw in the boot record. The escalation resets the chip.
 *
 * Boot 2 (reset reason escalation): the crash dump holds one of the expected alerts' causes, and
 * nothing in boot 1 saw a tagged result from the corrupted transfer (a CLC that returned, returned
 * tag 0; a sweep reported sweep_err). A boot 1 that ends without the reset fails.
 *
 * meta_intg_escalation: the meta SRAM is built without ECC correction (top_earlgrey.hjson
 * sram_ctrl_meta, sram_ctrl.hjson EccCorrection = 0), so one flipped bit is detected like two;
 * the spec's "1 bit: corrected" does not apply to this configuration.
 *
 * Without a bench (`model_gaps`) the fault is not injected: the expected alert is raised through
 * its ALERT_TEST instead, which checks the escalation and the crash dump only.
 *
 * Built with the INJECT_FAIL define, boot 2 expects uart0's fatal_fault in the crash dump: the run
 * must fail with `MISMATCH crash dump cause`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#if defined(EG2_FAULT_META)
// site: 1 a tag word's data, 2 a bitmap word's data, 3 their ECC bits; flip_bits 1 or 2;
// access: 1 CLC, 2 CSC, 3 a sweep.
#define FAULT_PARAMS(X) \
  X(meta_site, 0, 0, 3) \
  X(flip_bits, 0, 0, 2) \
  X(access, 0, 0, 3)    \
  X(model_gaps, 0, 0, 1)
#define FAULT_NAME "meta_intg_escalation"
#elif defined(EG2_FAULT_REVBM)
// field: 1 the response integrity, 2 the data integrity, 3 the data.
#define FAULT_PARAMS(X) \
  X(field, 0, 0, 3)     \
  X(model_gaps, 0, 0, 1)
#define FAULT_NAME "revbm_intg"
#elif defined(EG2_FAULT_RF)
// fault_site: 1 data, 2 the shadow copy (ECC), 3 metadata, 4 the tag; fault_reg 1-15; fault_bit
// reduced to the site's width (64 draws); fault_time in CPU cycles (0 draws 100-10,000,
// log-uniform).
#define FAULT_PARAMS(X)        \
  X(fault_site, 0, 0, 4)       \
  X(fault_reg, 0, 0, 15)       \
  X(fault_bit, 64, 0, 64)      \
  X(fault_time, 0, 0, 10000)   \
  X(model_gaps, 0, 0, 1)
#define FAULT_NAME "rf_intg"
#else
// fault_site: 1 command integrity, 2 data integrity, 3 the tag sideband; direction: 1 request,
// 2 response.
#define FAULT_PARAMS(X) \
  X(fault_site, 0, 0, 3) \
  X(direction, 0, 0, 2)  \
  X(model_gaps, 0, 0, 1)
#define FAULT_NAME "bus_intg"
#endif
EG2_TEST_PARAMS(FAULT_PARAMS);

#if defined(EG2_META_INTG_ESCALATION_INJECT_FAIL) || defined(EG2_REVBM_INTG_INJECT_FAIL) || \
    defined(EG2_BUS_INTG_INJECT_FAIL) || defined(EG2_RF_INTG_INJECT_FAIL)
#define INJECT 1
#else
#define INJECT 0
#endif

enum {
  kSlot = kEg2ArenaB,
  kObject = kEg2ArenaA,
  // Boot record words.
  kRecArmed = 0,     // 1 once boot 1 has armed the escalation
  kRecSeen = 1,      // what boot 1 saw: 1 + tag of a CLC that returned, 0x100 + mcause of a trap
  kRecSweep = 2,     // TBRE_STATUS after a sweep
  kRecPhase = 3,     // the step boot 1 reached
  kMaxExpected = 4,
};

static eg2_tally_t tally;

/** The alerts this fault may raise (any of them escalates). */
static uint32_t expected_alerts(int *out) {
  uint32_t n = 0;
#if defined(EG2_FAULT_META)
  out[n++] = eg2_status_alert_find("cheriot_mem_sys_fatal_fault");
  out[n++] = eg2_status_alert_find("sram_ctrl_meta_fatal_error");
#elif defined(EG2_FAULT_REVBM)
  out[n++] = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  out[n++] = eg2_status_alert_find("cheriot_mem_sys_fatal_fault");
#elif defined(EG2_FAULT_RF)
  out[n++] = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
#else
  out[n++] = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  out[n++] = eg2_status_alert_find("cheriot_mem_sys_fatal_fault");
  out[n++] = eg2_status_alert_find("sram_ctrl_sec_fatal_error");
#endif
  return n;
}

#if defined(EG2_FAULT_RF)
// Reads x1-x15 `rounds` times, each as an integer (beq, both read ports) and as a capability
// (csetequalexact into zero), writing none of them: a flipped storage bit is read whatever
// register it is in.
#define EG2_RF_READ(r) "beq " #r ", " #r ", 1f\n1:\n csetequalexact zero, c" #r ", c" #r "\n"
static void read_registers(uint32_t rounds) {
  for (uint32_t i = 0; i < rounds; ++i) {
    __asm__ volatile(EG2_RF_READ(ra) EG2_RF_READ(sp) EG2_RF_READ(gp) EG2_RF_READ(tp)
                         EG2_RF_READ(t0) EG2_RF_READ(t1) EG2_RF_READ(t2) EG2_RF_READ(s0)
                             EG2_RF_READ(s1) EG2_RF_READ(a0) EG2_RF_READ(a1) EG2_RF_READ(a2)
                                 EG2_RF_READ(a3) EG2_RF_READ(a4) EG2_RF_READ(a5)
                     ::
                         : "memory");
  }
}
#endif

static void boot_1(eg2_ms_rec_t *rec, bool bench) {
  int alerts[kMaxExpected];
  uint32_t n = expected_alerts(alerts);
  for (uint32_t i = 0; i < n; ++i) {
    if (alerts[i] >= 0) {
      eg2_ms_escalation_arm((size_t)alerts[i]);
    }
  }
  rec->data[kRecArmed] = 1;
  rec->data[kRecSeen] = 0xffffffffu;
  // The capability the fault is about: based on an object, stored in sec SRAM.
  eg2_ms_revbm_clear();
  eg2_ms_csc(kSlot, eg2_ms_object_cap(kObject, 8, false));
  rec->data[kRecPhase] = 1;
  bool ok;
#if defined(EG2_FAULT_META)
  uint32_t site = EG2_PARAM(meta_site) != 0 ? EG2_PARAM(meta_site) : eg2_rand_range(1, 3);
  uint32_t bits = EG2_PARAM(flip_bits) != 0 ? EG2_PARAM(flip_bits) : eg2_rand_range(1, 2);
  uint32_t access = EG2_PARAM(access) != 0 ? EG2_PARAM(access) : eg2_rand_range(1, 3);
  // The meta SRAM word: the slot's tag word, or the bitmap word of the object's granule.
  uint32_t word;
  if (site == 2) {
    uint32_t b;
    (void)eg2_ms_revbm_bit(kObject, &word, &b);
  } else {
    word = eg2_ms_tag_offset(kSlot) / 4;
  }
  ok = eg2_ms_bench("meta_flip", word, (site << 8) | bits, bench);
#elif defined(EG2_FAULT_RF)
  uint32_t access = 0;
  uint32_t site = EG2_PARAM(fault_site) != 0 ? EG2_PARAM(fault_site) : eg2_rand_range(1, 4);
  uint32_t reg = EG2_PARAM(fault_reg) != 0 ? EG2_PARAM(fault_reg) : eg2_rand_range(1, 15);
  uint32_t bit = EG2_PARAM(fault_bit) != 64 ? EG2_PARAM(fault_bit) : eg2_rand_range(0, 63);
  uint32_t when = EG2_PARAM(fault_time);
  if (when == 0) {
    when = eg2_rand_range(0, 1) == 0 ? eg2_rand_range(100, 999) : eg2_rand_range(1000, 10000);
  }
  eg2_put_kv("EG2 rf_intg site", site);
  eg2_put_kv(" reg", reg);
  eg2_put_kv(" bit", bit);
  eg2_put_kv(" cycles", when);
  eg2_puts("\r\n");
  ok = eg2_ms_bench("rf_flip", site | reg << 8 | bit << 16, when, bench);
  if (ok) {
    rec->data[kRecPhase] = 2;
    if (!bench) {
      // No fault without the bench: the alert through its ALERT_TEST.
      eg2_status_raise_alert((size_t)alerts[0]);
    }
    // Every register read, as an integer and as a capability, until the escalation reset.
    read_registers(4000);
    rec->data[kRecPhase] = 3;
    eg2_ms_spin(1000000);
    rec->data[kRecPhase] = 4;
  }
  return;
#elif defined(EG2_FAULT_REVBM)
  uint32_t field = EG2_PARAM(field) != 0 ? EG2_PARAM(field) : eg2_rand_range(1, 3);
  uint32_t access = 1;
  ok = eg2_ms_bench("revbm_corrupt", 0, field, bench);
#else
  uint32_t fsite = EG2_PARAM(fault_site) != 0 ? EG2_PARAM(fault_site) : eg2_rand_range(1, 3);
  uint32_t dir = EG2_PARAM(direction) != 0 ? EG2_PARAM(direction) : eg2_rand_range(1, 2);
  // The tag sideband travels on requests for a CSC and on responses for a CLC.
  uint32_t access = dir == 1 ? 2 : 1;
  ok = eg2_ms_bench("bus_corrupt", fsite, dir, bench);
#endif
  if (!ok) {
    return;
  }
  rec->data[kRecPhase] = 2;
  if (!bench) {
    // No fault without the bench: the first expected alert through its ALERT_TEST.
    eg2_status_raise_alert((size_t)alerts[0]);
  }
  eg2_probe_begin();
  if (access == 3) {
    eg2_ms_revbm_mark(kObject, true);
    eg2_ms_sweep_t sw;
    eg2_ms_tbre_start(kSlot, 1, &sw);
    eg2_ms_tbre_wait(&sw, 100000);
    rec->data[kRecSweep] = sw.status;
  } else {
    bool trapped = eg2_probe_call(eg2_ms_auth(kSlot),
                                  eg2_ms_object_cap(kObject, 8, false), NULL,
                                  access == 2 ? eg2_stub_csc : eg2_stub_clc);
    void *got = eg2_trap_rec.cap.p;
    if (trapped) {
      rec->data[kRecSeen] = 0x100 + eg2_trap_rec.mcause;
    } else if (access == 1) {
      rec->data[kRecSeen] = 1 + eg2_ms_cap_tag(got);
    } else {
      // A CSC that returned: what it left, through a CLC.
      bool t2 = eg2_probe_call(eg2_ms_auth(kSlot), NULL, NULL, eg2_stub_clc);
      rec->data[kRecSeen] = t2 ? 0x100 + eg2_trap_rec.mcause : 1 + eg2_ms_cap_tag(eg2_trap_rec.cap.p);
    }
  }
  eg2_probe_end();
  rec->data[kRecPhase] = 3;
  // The escalation reset ends this boot.
  eg2_ms_spin(1000000);
  rec->data[kRecPhase] = 4;
}

static bool boot_2(eg2_ms_rec_t *rec, bool bench) {
  eg2_check(&tally, "boot 1 armed", 0, rec->data[kRecArmed], 1);
  eg2_check(&tally, "reset reason escalation", 0, eg2_ms_reset_reason() & kEg2MsResetEscalation,
            kEg2MsResetEscalation);
  int alerts[kMaxExpected];
  uint32_t n = expected_alerts(alerts);
  uint32_t any = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (alerts[i] >= 0 && eg2_ms_crashdump_cause((size_t)alerts[i])) {
      any = 1;
      eg2_puts("EG2 crash dump: ");
      eg2_puts(eg2_status_alert_name((size_t)alerts[i]));
      eg2_puts("\r\n");
    }
  }
  if (INJECT) {
    int u = eg2_status_alert_find("uart0_fatal_fault");
    any = u >= 0 && eg2_ms_crashdump_cause((size_t)u);
  }
  eg2_check(&tally, "crash dump cause", 0, any, 1);
  eg2_put_kv("EG2 boot 1 reached step", rec->data[kRecPhase]);
  eg2_put_kv(" saw", rec->data[kRecSeen]);
  eg2_put_kv(" sweep status", rec->data[kRecSweep]);
  eg2_puts("\r\n");
  if (bench) {
    // Never a tagged result from the corrupted transfer: a CLC that returned returned tag 0 (seen
    // 1); a trap (0x100 + mcause) or a core stopped by the alert before it recorded (0xffffffff,
    // REQ_CIB_02's fetch disable) is fine.
    eg2_check(&tally, "tagged result from a corrupted transfer", 0,
              rec->data[kRecSeen] == 2 ? 1 : 0, 0);
  }
  eg2_check(&tally, "the reset came before boot 1 gave up", 0, rec->data[kRecPhase] < 4 ? 1 : 0,
            1);
  return eg2_tally_ok(&tally);
}

bool eg2_test_main(void) {
  bool bench = EG2_PARAM(model_gaps) == 0;
  eg2_ms_rec_t *rec = eg2_ms_rec_boot();
  if (!eg2_sec_sram_init()) {
    return false;
  }
  eg2_put_kv("EG2 " FAULT_NAME " boot", rec->boot);
  eg2_puts("\r\n");
  if (rec->boot == 1) {
    boot_1(rec, bench);
    eg2_puts("EG2 no escalation reset\r\n");
    return false;
  }
  return boot_2(rec, bench);
}
