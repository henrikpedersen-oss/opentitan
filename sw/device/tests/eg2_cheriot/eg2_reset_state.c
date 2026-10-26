// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_reset_state (REQ_SOC_02) and, built with EG2_META_INIT_TEST,
 * chip_sw_cheriot_meta_init_after_reset (REQ_CMS_15) (EG2 spec "Mode switch and handover",
 * "Revocation").
 *
 * REQ_SOC_02: every reset (POR, software, NDM, escalation, deep-sleep wake-up, watchdog bite)
 * leaves the system in ePMP mode: CHERIOT_ENA reads MuBi4False, the switch is unlocked, misa has
 * I=1/E=0 (RFC CHERIoT in OpenTitan p2 "Background").
 * REQ_CMS_15: after every reset type the meta SRAM is initialised before CHERIOT_LOCK, so CHERIoT
 * mode starts with no tag and no revocation bit set; a reset during a sweep leaves no stale tag
 * (RFC CHERIoT Memory Subsystem "Unified Meta SRAM").
 *
 * Boot 1 (power-on): a CHERIoT session (`session_len` loop iterations), then capabilities stored in
 * sec SRAM's arena B (`tags_set`, or `n_tags` with as many bitmap bits set over arena A), for
 * meta_init a sweep started when the reset comes (`sweep_running`), then the reset (`reset_type`;
 * POR and NDM are bench steps). Every choice is drawn from the seed before anything else, so the
 * boot after the reset draws the same plan.
 *
 * Every boot, before the switch (eg2_reset_late.S): CHERIOT_ENA, misa and the reset reason are
 * recorded; the switch then locks CHERIoT mode, which also shows it was unlocked. Boot 2 checks:
 * the reset reason of the planned type, CHERIOT_ENA MuBi4False and misa I=1/E=0 before the switch;
 * then every capability stored before the reset reloads with tag 0 and every bitmap word reads 0.
 *
 * The meta SRAM on boot 2 is the boot chain's to initialise (REQ_CMS_15); neither the ROM, the
 * test ROM nor anything but the EG2 start-up code does (rtl_tickets F11). meta_init_after_reset
 * therefore leaves it alone on boot 2 (`rely_on_crt_meta_init` 0) and is expected to fail until F11
 * is fixed: on the chip the first store in CHERIoT mode meets the uninitialised meta SRAM. In
 * reset_state the knob defaults to 1 (the start-up code zeroes the meta SRAM on every boot), so
 * that its mode checks run; its tag check is then met by that initialisation, and REQ_CMS_15 is
 * meta_init_after_reset's.
 *
 * The SystemC model has no POR or NDM without the bench: with `model_gaps` they become software
 * resets.
 *
 * Built with the INJECT_FAIL define, boot 2 expects CHERIOT_ENA to read MuBi4True: the run must
 * fail with `MISMATCH CHERIOT_ENA after the reset`.
 */

#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_reset_params.h"

EG2_LATE_TEST_PARAMS(EG2_RESET_PARAMS);

#if defined(EG2_RESET_STATE_INJECT_FAIL) || defined(EG2_META_INIT_AFTER_RESET_INJECT_FAIL)
#define INJECT 1
#else
#define INJECT 0
#endif

#ifdef EG2_META_INIT_TEST
#define TEST_NAME "meta_init_after_reset"
#else
#define TEST_NAME "reset_state"
#endif

static eg2_tally_t tally;

typedef struct plan {
  eg2_ms_reset_kind_t kind;
  uint32_t session_len;
  uint32_t n_caps;
  bool sweep;
} plan_t;

/** The plan, from the seed: the same on every boot. */
static plan_t draw_plan(bool bench) {
  plan_t p;
  uint32_t t = EG2_PARAM(reset_type) != 0 ? EG2_PARAM(reset_type) : eg2_rand_range(1, 6);
  p.kind = (eg2_ms_reset_kind_t)(t == 1   ? kEg2MsResetKindPor
                                 : t == 2 ? kEg2MsResetKindSw
                                 : t == 3 ? kEg2MsResetKindNdm
                                 : t == 4 ? kEg2MsResetKindEscalation
                                 : t == 5 ? kEg2MsResetKindDeepSleep
                                          : kEg2MsResetKindWatchdog);
  if (!bench && (p.kind == kEg2MsResetKindPor || p.kind == kEg2MsResetKindNdm)) {
    p.kind = kEg2MsResetKindSw;
  }
  // Log-uniform 10-10,000.
  uint32_t e = eg2_rand_range(1, 4);
  uint32_t lo = e == 1 ? 10 : e == 2 ? 100 : e == 3 ? 1000 : 1000;
  uint32_t hi = e == 1 ? 99 : e == 2 ? 999 : 9999;
  p.session_len = EG2_PARAM(session_len) != 0 ? EG2_PARAM(session_len) : eg2_rand_range(lo, hi);
#ifdef EG2_META_INIT_TEST
  p.n_caps = EG2_PARAM(n_tags) != 257 ? EG2_PARAM(n_tags) : eg2_rand_range(0, 256);
  p.sweep = EG2_PARAM(sweep_running) != 2 ? EG2_PARAM(sweep_running) != 0 : eg2_pick(2) != 0;
#else
  p.n_caps = EG2_PARAM(tags_set) != 65 ? EG2_PARAM(tags_set) : eg2_rand_range(0, 64);
  p.sweep = false;
#endif
  return p;
}

static void boot_1(const plan_t *p, bool bench) {
  eg2_puts("EG2 " TEST_NAME " boot 1: a session, then the ");
  eg2_puts(eg2_ms_reset_name(p->kind));
  eg2_puts(" reset\r\n");
  // The session: integer and capability work for session_len iterations.
  void *c = eg2_ms_object_cap(kEg2ArenaA, 64, false);
  for (volatile uint32_t i = 0; i < p->session_len; ++i) {
    __asm__ volatile("cincoffsetimm %0, %0, 1" : "+C"(c));
  }
  eg2_ms_revbm_clear();
  for (uint32_t i = 0; i < p->n_caps; ++i) {
    eg2_ms_csc(kEg2ArenaB + 8 * i, eg2_ms_object_cap(kEg2ArenaA + 8 * i, 8, false));
#ifdef EG2_META_INIT_TEST
    eg2_ms_revbm_mark(kEg2ArenaA + 8 * i, true);
#endif
  }
  if (p->sweep) {
    // A sweep over the whole main and sec SRAM region, still running at the reset.
    eg2_ms_sweep_t sw;
    eg2_ms_tbre_start(kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8, &sw);
  }
  eg2_ms_reset(p->kind, bench);
}

static bool boot_2(const plan_t *p) {
  uint32_t ena = eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_ENA);
  uint32_t misa = eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_MISA);
  uint32_t reason = eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_REASON);
  uint32_t inited = eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_META_INIT);
  eg2_put_kv("EG2 " TEST_NAME " boot 2: reason", reason);
  eg2_put_kv(" CHERIOT_ENA", ena);
  eg2_put_kv(" misa", misa);
  eg2_puts(inited ? " (meta SRAM zeroed by the start-up code)\r\n"
                  : " (meta SRAM left to the boot chain)\r\n");
  uint32_t exp_reason = eg2_ms_reset_expected(p->kind);
  if (p->kind == kEg2MsResetKindPor) {
    exp_reason = kEg2MsResetPor;
  }
  eg2_check(&tally, "reset reason", 0, reason & exp_reason, exp_reason);
  eg2_check(&tally, "CHERIOT_ENA after the reset", 0, ena & 0xf, INJECT ? 0x6u : 0x9u);
  eg2_check(&tally, "misa I,E after the reset", 0, misa & ((1u << 8) | (1u << 4)), 1u << 8);

  // No tag and no revocation bit left.
  eg2_probe_begin();
  for (uint32_t i = 0; i < p->n_caps; ++i) {
    ++tally.planned;
    bool t = eg2_probe_call(eg2_ms_auth(kEg2ArenaB + 8 * i), NULL, NULL, eg2_stub_clc);
    if (eg2_check(&tally, "clc trapped", i, t ? eg2_trap_rec.mcause : 0, 0)) {
      eg2_check(&tally, "tag left", i, eg2_ms_cap_tag(eg2_trap_rec.cap.p), 0);
    }
    ++tally.done;
  }
  for (uint32_t w = 0; w < kEg2MsRevbmSize / 4; ++w) {
    bool t = eg2_probe_call(eg2_mem(kEg2MsMetaBase + 4 * w, 4), NULL, NULL, eg2_stub_lw);
    if (eg2_check(&tally, "bitmap read trapped", w, t ? eg2_trap_rec.mcause : 0, 0)) {
      eg2_check(&tally, "bitmap word left", w, eg2_trap_rec.value, 0);
    }
  }
  eg2_probe_end();
  eg2_tally_print(TEST_NAME, &tally);
  return eg2_tally_ok(&tally);
}

bool eg2_test_main(void) {
  bool bench = EG2_PARAM(model_gaps) == 0;
  plan_t p = draw_plan(bench);
  uint32_t reason = eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_REASON);
  bool later = !(reason & kEg2MsResetPor) || EG2_PARAM(por_done) != 0;
  if (!later) {
    // The first boot is a power-on reset too: its state before the switch is checked as well.
    eg2_check(&tally, "CHERIOT_ENA after power-on", 0,
              eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_ENA) & 0xf, 0x9);
    eg2_check(&tally, "misa I,E after power-on", 0,
              eg2_late_word(EG2_LATE_INFO + EG2_RESET_INFO_MISA) & ((1u << 8) | (1u << 4)),
              1u << 8);
    if (!eg2_sec_sram_init()) {
      return false;
    }
    boot_1(&p, bench);
    eg2_puts("EG2 no reset came\r\n");
    return false;
  }
  return boot_2(&p);
}
