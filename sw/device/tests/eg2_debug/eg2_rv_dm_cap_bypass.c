// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_cap_bypass (EG2 verification spec REQ_CDM_07, REQ_CDM_08, REQ_CMS_11; "Debug
 * Controller" / "Corner cases"; the chip form of the core specification's REQ_DBG_03 and
 * REQ_DBG_05), both modes.
 *
 * Phases (each a command of eg2_dm_corners.h's protocol, kind kEg2DmAction, arg = the phase; the
 * bench's part is env/seq_lib/chip_eg2_rv_dm_cap_bypass_vseq.sv):
 *   0 CHERIoT: CSpecialRW of SCRs 24, 25 and 26 (read and write) outside debug mode: each is an
 *     illegal instruction, mcause 2, MEPCC at the access (core REQ_SCR_01, REQ_DBG_03: "Outside
 *     debug mode an access to SCRs 24-26 is an illegal instruction").
 *   1 CHERIoT: the core waits in eg2_dmx_park with three authorities in the context (offsets
 *     from each authority's cursor: word at +0x10, store at +0x14, capability at +0x18, CSC target
 *     at +0x20):
 *       auth_oob  tagged, bounds [R, R + 0x10): every access is beyond its top
 *       auth_ut   untagged, cursor R + 0x40
 *       auth_np   tagged, bounds [R + 0x80, R + 0xa8), GL its only permission
 *     The bench runs `clc a1, auth; lw a2, 0x10(a1); sw t2, 0x14(a1); clc t0, 0x18(a1);
 *     csc a0, 0x20(a1); cgettag t1, t0; csc t0, out[k](a0); ebreak` for each (cmderr 0, a2 the
 *     word, t1 = 1), then the SCRs: DEPCC read (tagged, address = dpc), DScratchC1 and
 *     DScratchC0 written with ca0 and read back into out[3], out[4]. Every fetch is from the
 *     debug memory, outside PCC's bounds. Checked here after resume: each SW stored the bench's
 *     t2, each CSC stored the context capability with its tag, each CLC loaded the stored
 *     capability unchanged (tag, LG/LM: core REQ_DBG_03 "a CLC applies no tag strip or LG/LM
 *     attenuation"), and the debug SCRs held ca0 with its tag.
 *   2 CHERIoT: a CLC from the program buffer of a capability based in a granule marked in the
 *     revocation bitmap loads with tag 0: the load filter applies in debug mode too (spec
 *     REQ_CMS_11: every CLC). The bench reads the tag (t1); here out[5] is checked untagged.
 *   3, 4, 5 CHERIoT: the bench sets DEPCC to the landing capability (ca4) untagged, sealed
 *     (object type 7) and without EX, then resumes. The first fetch must fault (core REQ_DBG_03):
 *     mcause 28, MEPCC.address = the landing address, mtval 0x402 for the untagged one (PCC, tag
 *     violation; CHERIoT-Sail ext_fetch_check_pc), mtval[10:5] = 0x20 (PCC) for the other two
 *     (CHERIoT-Sail has no behaviour for a sealed or non-executable PCC).
 *   6 RISC-V: PMP entry 0 locked over rv_dm's memory with no permission; an M-mode load from it
 *     faults (mcause 5); then the core waits and the bench halts it, runs abstract commands and a
 *     program buffer and resumes: the debug memory stays reachable in debug mode (RISC-V Debug
 *     Specification A.2; core REQ_DBG_05). Last phase: the entry stays locked until reset.
 *
 * EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
 * input, so its base debug ROM runs on a core in CHERIoT mode; phases 1-5 need the halt.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_gaps.
 *
 * | Skipped with model_gaps = 1                       | Why                                     |
 * |---------------------------------------------------|-----------------------------------------|
 * | the bench's halts, program buffers and done word  | eg2sim has no debug module, JTAG or SBA |
 * | phases 1-5's results; the fault after dret        | they are the program buffer's           |
 * | phase 6's M-mode fault on rv_dm's memory          | eg2sim has no debug memory              |
 * | phase 2: the marked slot's tag outside debug mode | eg2sim has no load filter               |
 *
 * Built with EG2_RV_DM_CAP_BYPASS_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `DEBUG FAIL`.
 */

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define CAP_BYPASS_PARAMS(X)                 \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(CAP_BYPASS_PARAMS)

// Shared with chip_eg2_rv_dm_cap_bypass_vseq.sv.
enum {
  kPhScrOutside = 0,
  kPhBypass = 1,
  kPhMarked = 2,
  kPhDepccUntagged = 3,
  kPhDepccSealed = 4,
  kPhDepccNoEx = 5,
  kPhPmp = 6,
  kWordOob = 0x0b0b0001,
  kWordUt = 0x0b0b0002,
  kWordNp = 0x0b0b0003,
  kStoreOob = 0x570000a1,
  kStoreUt = 0x570000a2,
  kStoreNp = 0x570000a3,
  kSealOtype = 7,
  // Region offsets: authority cursors and their accesses.
  kCurOob = 0x00,
  kCurUt = 0x40,
  kCurNp = 0x80,
  kOffWord = 0x10,
  kOffStore = 0x14,
  kOffCap = 0x18,
  kOffCsc = 0x20,
  kRegionSize = 0xc0,
  kMcauseIllegal = 2,
  kMcauseLoadAccess = 5,
  kMcauseCheri = 28,
  kMtvalPccTag = (0x20u << 5) | 0x02,
};

// The CHERIoT phases' memory: sec SRAM's work area (eg2_cheriot_rand.h; initialised by
// eg2_sec_sram_init), in the revocable window, reached only by address through eg2_ms_* (F24),
// and outside .data (the CHERIoT images keep their globals under 4 KiB, F21).
enum {
  kRegion = kEg2WorkBase,
  kMarkObj = kEg2WorkBase + 0x100,
  kMarkSlot = kEg2WorkBase + 0x108,
};

static bool model_gaps(void) { return EG2_PARAM(model_gaps) != 0; }

// The stage, run under the probe; in eg2sim (no bench) the stop word is set first, so it returns.
static bool run_stage(eg2_dmx_ctx_t *ctx, char *stage) {
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  return eg2_probe_call(ctx, NULL, NULL, stage);
}

#ifdef __CHERIOT__

static uint32_t cap_addr(void *c) { return (uint32_t)__builtin_cheri_address_get(c); }

// A switch, not a table: a table of code pointers would be an initialised global (F21).
static char *scr_stub(int i) {
  switch (i) {
    case 0:
      return eg2_dmx_scr24_r;
    case 1:
      return eg2_dmx_scr25_r;
    case 2:
      return eg2_dmx_scr26_r;
    case 3:
      return eg2_dmx_scr24_w;
    case 4:
      return eg2_dmx_scr25_w;
    default:
      return eg2_dmx_scr26_w;
  }
}

static bool phase_scr_outside(void) {
  bool ok = true;
  void *val = eg2_ms_data_cap(kRegion, 8);
  for (int i = 0; i < 6; ++i) {
    bool trapped = eg2_probe_call(NULL, val, NULL, scr_stub(i));
    eg2_chk_step(i < 3 ? "SCR read outside debug mode" : "SCR write outside debug mode",
                 24 + (uint32_t)(i % 3));
    ok = eg2_chk_true("SCR 24-26 access outside debug mode traps", trapped) && ok;
    ok = eg2_chk_eq("SCR 24-26 outside debug mode: mcause (illegal instruction)", kMcauseIllegal,
                    eg2_trap_rec.mcause) &&
         ok;
    ok = eg2_chk_eq("SCR 24-26 outside debug mode: MEPCC at the access", cap_addr(scr_stub(i)),
                    cap_addr(eg2_trap_rec.mepcc.p)) &&
         ok;
  }
  return ok;
}

static void region_init(uint32_t r) {
  for (uint32_t off = 0; off < kRegionSize; off += 4) {
    eg2_ms_write32(r + off, 0);
  }
  eg2_ms_write32(r + kCurOob + kOffWord, kWordOob);
  eg2_ms_write32(r + kCurUt + kOffWord, kWordUt);
  eg2_ms_write32(r + kCurNp + kOffWord, kWordNp);
  // The capabilities the CLCs load: R's capability with its cursor on the slot (global, LG, LM).
  for (uint32_t k = 0; k < 3; ++k) {
    uint32_t slot = r + kCurOob + 0x40 * k + kOffCap;
    eg2_ms_csc(slot, eg2_ms_data_cap(slot, 8));
  }
}

static bool phase_bypass(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  uint32_t r = kRegion;
  region_init(r);
  void *oob = eg2_ms_data_cap(r + kCurOob, 0x10);
  void *ut = __builtin_cheri_tag_clear(eg2_ms_data_cap(r + kCurUt, 0x30));
  void *np = __builtin_cheri_perms_and(eg2_ms_data_cap(r + kCurNp, 0x28), kEg2PermGl);
  ok = eg2_chk_eq("construction: auth_oob length", 0x10, __builtin_cheri_length_get(oob)) && ok;
  ok = eg2_chk_eq("construction: auth_ut untagged", 0, __builtin_cheri_tag_get(ut)) && ok;
  ok = eg2_chk_eq("construction: auth_np tagged", 1, __builtin_cheri_tag_get(np)) && ok;
  ok = eg2_chk_eq("construction: auth_np permissions (GL only)", kEg2PermGl,
                  __builtin_cheri_perms_get(np) & kEg2PermAll) &&
       ok;
  ctx->auth_oob.p = oob;
  ctx->auth_ut.p = ut;
  ctx->auth_np.p = np;
  bool trapped = run_stage(ctx, eg2_dmx_park);
  ok = eg2_chk_true("no trap while the bench used the program buffer", !trapped) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phase 1: the program buffer's loads, stores and SCR accesses (no debug module)");
    return ok;
  }
  for (uint32_t k = 0; k < 3; ++k) {
    // Cursors kCurOob, kCurUt, kCurNp are 0x40 apart; the stores kStoreOob + k.
    uint32_t base = r + kCurOob + 0x40 * k;
    eg2_chk_step("authority", (uint32_t)k);
    ok = eg2_chk_eq("SW in debug mode stored t2", kStoreOob + k, eg2_ms_read32(base + kOffStore)) &&
         ok;
    void *stored = eg2_ms_clc(base + kOffCsc);
    ok = eg2_chk_eq("CSC in debug mode: tag kept", 1, eg2_ms_cap_tag(stored)) && ok;
    ok = eg2_chk_true("CSC in debug mode stored the context capability",
                      __builtin_cheri_equal_exact(stored, (void *)ctx)) &&
         ok;
    void *loaded = ctx->out[k].p;
    void *in_mem = eg2_ms_clc(base + kOffCap);
    ok = eg2_chk_eq("CLC in debug mode: tag kept", 1, eg2_ms_cap_tag(loaded)) && ok;
    ok = eg2_chk_true("CLC in debug mode: the capability unattenuated (LG, LM)",
                      __builtin_cheri_equal_exact(loaded, in_mem)) &&
         ok;
  }
  void *dsc1 = ctx->out[3].p;
  void *dsc0 = ctx->out[4].p;
  ok = eg2_chk_eq("DScratchC1 kept the tag", 1, eg2_ms_cap_tag(dsc1)) && ok;
  ok = eg2_chk_true("DScratchC1 written and read back",
                    __builtin_cheri_equal_exact(dsc1, (void *)ctx)) &&
       ok;
  ok = eg2_chk_eq("DScratchC0 kept the tag", 1, eg2_ms_cap_tag(dsc0)) && ok;
  ok = eg2_chk_true("DScratchC0 written and read back",
                    __builtin_cheri_equal_exact(dsc0, (void *)ctx)) &&
       ok;
  return ok;
}

static bool phase_marked(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  uint32_t obj = kMarkObj;
  uint32_t slot = kMarkSlot;
  eg2_ms_revbm_clear();
  eg2_ms_csc(slot, eg2_ms_object_cap(obj, 8, false));
  ok = eg2_chk_eq("marked slot: tagged before the mark", 1, eg2_ms_tag(slot)) && ok;
  eg2_ms_revbm_mark(obj, true);
  if (!model_gaps()) {
    ok = eg2_chk_eq("marked slot: a CLC outside debug mode loads tag 0", 0, eg2_ms_tag(slot)) &&
         ok;
  }
  ctx->auth_mark.p = eg2_ms_auth(slot);
  ctx->out[5].p = eg2_ms_auth(slot);  // tagged, so a CSC of an untagged value shows
  bool trapped = run_stage(ctx, eg2_dmx_park);
  ok = eg2_chk_true("no trap while the bench used the program buffer", !trapped) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phase 2: the program buffer's CLC of the marked capability (no debug module)");
  } else {
    ok = eg2_chk_eq("CLC in debug mode of a marked capability: tag 0 (load filter)", 0,
                    eg2_ms_cap_tag(ctx->out[5].p)) &&
         ok;
  }
  eg2_ms_revbm_mark(obj, false);
  return ok;
}

static bool phase_depcc(eg2_dmx_ctx_t *ctx, uint32_t phase) {
  bool ok = true;
  bool trapped = run_stage(ctx, eg2_dmx_park);
  if (model_gaps()) {
    eg2_chk_skip("phases 3-5: the fault after dret through a bad DEPCC (no debug module)");
    return ok;
  }
  const char *what = phase == kPhDepccUntagged ? "untagged DEPCC"
                     : phase == kPhDepccSealed ? "sealed DEPCC"
                                               : "DEPCC without EX";
  eg2_chk_step(what, phase);
  ok = eg2_chk_true("dret through a bad DEPCC: the first fetch traps", trapped) && ok;
  ok = eg2_chk_eq("bad DEPCC: mcause (CHERI fault)", kMcauseCheri, eg2_trap_rec.mcause) && ok;
  ok = eg2_chk_eq("bad DEPCC: MEPCC.address = the landing address", ctx->landing,
                  cap_addr(eg2_trap_rec.mepcc.p)) &&
       ok;
  if (phase == kPhDepccUntagged) {
    ok = eg2_chk_eq("untagged DEPCC: mtval (PCC, tag violation)", kMtvalPccTag,
                    eg2_trap_rec.mtval) &&
         ok;
  } else {
    ok = eg2_chk_eq("bad DEPCC: mtval[10:5] (PCC)", 0x20, (eg2_trap_rec.mtval >> 5) & 0x3f) && ok;
  }
  return ok;
}

#else  // RISC-V mode

static bool phase_pmp(eg2_dmx_ctx_t *ctx) {
  bool ok = true;
  uint32_t cfg0;
  __asm__ volatile("csrr %0, pmpcfg0" : "=r"(cfg0));
  if ((cfg0 & 0xffu) != 0) {
    // The test ROM leaves entries 0-14 off (test_rom_start.S); anything else is a set-up change.
    return eg2_chk_eq("PMP entry 0 free for the test", 0, cfg0 & 0xffu);
  }
  // NAPOT over rv_dm's 4 KiB memory; locked, no permission (L | A = NAPOT).
  uint32_t addr = (TOP_EARLGREY_RV_DM_MEM_BASE_ADDR >> 2) | ((0x1000u >> 3) - 1);
  __asm__ volatile("csrw pmpaddr0, %0" : : "r"(addr));
  __asm__ volatile("csrw pmpcfg0, %0" : : "r"((cfg0 & ~0xffu) | 0x98u));
  uint32_t got;
  __asm__ volatile("csrr %0, pmpcfg0" : "=r"(got));
  ok = eg2_chk_eq("PMP entry 0: locked, NAPOT, no permission", 0x98, got & 0xffu) && ok;
  bool trapped = eg2_probe_call((void *)TOP_EARLGREY_RV_DM_MEM_BASE_ADDR, NULL, NULL, eg2_stub_lw);
  if (model_gaps()) {
    eg2_chk_skip("phase 6: the M-mode fault on rv_dm's memory (no debug memory in eg2sim)");
  } else {
    ok = eg2_chk_true("M-mode load from rv_dm's memory under the locked entry traps", trapped) &&
         ok;
    ok = eg2_chk_eq("M-mode load from rv_dm's memory: mcause (load access fault)",
                    kMcauseLoadAccess, eg2_trap_rec.mcause) &&
         ok;
  }
  trapped = run_stage(ctx, eg2_dmx_park);
  ok = eg2_chk_true("no trap across the halt under the locked entry", !trapped) && ok;
  return ok;
}

#endif  // __CHERIOT__

static bool run_phase(uint32_t phase) {
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  bool ok = true;
  eg2_chk_step("phase", phase);
#ifdef __CHERIOT__
  // The sealer (phase 4): the sealing root, MScratchC as the framework leaves it, read before the
  // probe puts its trap record there; its address is the object type.
  ctx.sealer.p = __builtin_cheri_address_set(eg2_ms_sealing_cap(), kSealOtype);
#endif
  eg2_probe_begin();
  switch (phase) {
#ifdef __CHERIOT__
    case kPhScrOutside:
      ok = phase_scr_outside();
      break;
    case kPhBypass:
      ok = phase_bypass(&ctx);
      break;
    case kPhMarked:
      ok = phase_marked(&ctx);
      break;
    case kPhDepccUntagged:
    case kPhDepccSealed:
    case kPhDepccNoEx:
      ok = phase_depcc(&ctx, phase);
      break;
#else
    case kPhPmp:
      ok = phase_pmp(&ctx);
      break;
#endif
    default:
      ok = eg2_chk_true("phase known in this mode", false);
      break;
  }
  eg2_probe_end();
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  if (kind != kEg2DmAction) {
    return eg2_chk_true("action known", false);
  }
  if (!model_gaps()) {
    return run_phase(arg);
  }
  // eg2sim: every phase of this mode once, without the bench.
  bool ok = true;
#ifdef __CHERIOT__
  for (uint32_t p = kPhScrOutside; p <= kPhDepccNoEx; ++p) {
    ok = run_phase(p) && ok;
  }
#else
  ok = run_phase(kPhPmp) && ok;
#endif
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
#ifdef __CHERIOT__
  if (!eg2_sec_sram_init()) {
    return eg2_chk_true("sec SRAM initialised", false);
  }
#endif
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdCapBypass,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = 0,
      .model_actions = 1,
      .wfi_ticks = 20,
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
  bool ok = eg2_dm_serve(&cfg);
  // RISC-V mode has one phase: 4 checks on the chip, 2 in eg2sim (the fault is a model gap).
#ifdef __CHERIOT__
  return eg2_chk_summary(16) && ok;
#else
  return eg2_chk_summary(model_gaps() ? 2 : 4) && ok;
#endif
}
