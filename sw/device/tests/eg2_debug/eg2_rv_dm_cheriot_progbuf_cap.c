// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_rv_dm_cheriot_progbuf_cap (EG2 verification spec REQ_CDM_09; "Debug Controller", knobs in
 * "Debug in CHERIoT mode"), CHERIoT mode only.
 *
 * REQ_CDM_09: CLC/CSC run by debug abstract commands or the program buffer keep tags and are not
 * split by other traffic (RFC CHERIoT Memory Subsystem "Background"). The bench
 * (env/seq_lib/chip_rv_dm_cheriot_progbuf_cap_vseq.sv) halts the core while it waits here and runs
 * up to 128 drawn commands; the CLC/CSC ones copy a source slot to a destination slot from the
 * program buffer, in a loop, while it makes SBA traffic on the bus, and check the tag and both
 * words.
 *
 * The region R (sec SRAM's work area, eg2_cheriot_rand.h; offsets shared with the vseq):
 *   R + 0x00  4 source slots, the capability shapes (cap_class): 0 a data capability of 16 bytes,
 *             1 one with GL, LD and MC only, 2 a sealed one (object type 9), 3 an untagged one
 *   R + 0x40  8 destination slots, zero (untagged) at the start
 *   R + 0x80  4 junk words, zero at the start, which the bench writes over SBA
 * ca1 = an authority over [R, R + 0xa0) while the core waits; scratch word 1 = R, word 2 = the
 * sources' tags (bit s).
 * After resume the bench has left the destination map in scratch words 8-15 (source + 1, or 0)
 * and the junk words' last values in scratch words 16-19. Checked here: every destination holds
 * its source exactly, tag included (CSetEqualExact, so both halves and the tag are one access's);
 * an untouched destination is still zero; the sources are unchanged; the junk words hold the
 * bench's values.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_gaps.
 *
 * | Skipped with model_gaps = 1                                | Why                             |
 * |------------------------------------------------------------|---------------------------------|
 * | the bench's halt, commands and done word; the destination  | eg2sim's debugger is driven by  |
 * | and junk checks after resume                               | --stim, which model_tests.sh    |
 * |                                                            | does not pass; its CHERIoT      |
 * |                                                            | debug mode lacks the program    |
 * |                                                            | buffer and dcsr.step            |
 *
 * Built with EG2_RV_DM_CHERIOT_PROGBUF_CAP_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define PROGBUF_CAP_PARAMS(X)                \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(PROGBUF_CAP_PARAMS)

// Shared with chip_rv_dm_cheriot_progbuf_cap_vseq.sv.
enum {
  kEg2DebugIdProgbufCap = 0x42,
  kNumSrc = 4,
  kNumDst = 8,
  kNumJunk = 4,
  kRegion = kEg2WorkBase,
  kOffSrc = 0x00,
  kOffDst = 0x40,
  kOffJunk = 0x80,
  kRegionSize = 0xa0,
  kObjects = kEg2WorkBase + 0x100,
  kTagMask = 0x7,
  kScratchMap = 8,
  kScratchJunk = 16,
};

static bool model_gaps(void) { return EG2_PARAM(model_gaps) != 0; }

#ifdef __CHERIOT__

// A switch, not a table of capabilities: an initialised global would be in .data (F21).
static void *src_cap(uint32_t s) {
  switch (s) {
    case 0:
      return eg2_ms_data_cap(kObjects, 16);
    case 1:
      return __builtin_cheri_perms_and(eg2_ms_data_cap(kObjects + 0x20, 32),
                                       kEg2PermGl | kEg2PermLd | kEg2PermMc);
    case 2:
      return eg2_ms_object_cap(kObjects + 0x40, 8, true);
    default:
      return __builtin_cheri_tag_clear(eg2_ms_data_cap(kObjects + 0x60, 64));
  }
}

static bool run(void) {
  volatile uint32_t *scratch = eg2_dm_scratch();
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  for (uint32_t i = kScratchMap; i < kScratchJunk + kNumJunk; ++i) {
    scratch[i] = 0;
  }
  eg2_ms_revbm_clear();
  for (uint32_t off = 0; off < kRegionSize; off += 4) {
    eg2_ms_write32(kRegion + off, 0);
  }
  bool ok = true;
  for (uint32_t s = 0; s < kNumSrc; ++s) {
    uint32_t addr = kRegion + kOffSrc + 8 * s;
    void *c = src_cap(s);
    eg2_ms_csc(addr, c);
    eg2_chk_step("source", s);
    ok = eg2_chk_eq("construction: the source's tag", (kTagMask >> s) & 1u, eg2_ms_tag(addr)) &&
         ok;
    ok = eg2_chk_true("construction: the source stored whole",
                      __builtin_cheri_equal_exact(eg2_ms_clc(addr), c)) &&
         ok;
  }
  scratch[kEg2DmxScratchArg1] = kRegion;
  scratch[kEg2DmxScratchArg2] = kTagMask;
  void *auth = eg2_ms_data_cap(kRegion, kRegionSize);
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  eg2_probe_begin();
  bool trapped = eg2_probe_call(&ctx, auth, NULL, eg2_dmx_park);
  eg2_probe_end();
  ok = eg2_chk_eq("no trap while the bench used the program buffer", 0, trapped ? 1u : 0u) && ok;
  for (uint32_t s = 0; s < kNumSrc; ++s) {
    eg2_chk_step("source after resume", s);
    ok = eg2_chk_true("the source unchanged",
                      __builtin_cheri_equal_exact(eg2_ms_clc(kRegion + kOffSrc + 8 * s),
                                                  src_cap(s))) &&
         ok;
  }
  if (model_gaps()) {
    eg2_chk_skip("the program buffer's CLC/CSC copies and the bench's SBA traffic (no debugger)");
    return ok;
  }
  for (uint32_t j = 0; j < kNumDst; ++j) {
    uint32_t addr = kRegion + kOffDst + 8 * j;
    uint32_t m = scratch[kScratchMap + j];
    eg2_chk_step("destination (source + 1)", j << 8 | m);
    if (m == 0) {
      ok = eg2_chk_eq("untouched destination: word 0", 0, eg2_ms_read32(addr)) && ok;
      ok = eg2_chk_eq("untouched destination: word 1", 0, eg2_ms_read32(addr + 4)) && ok;
    } else if (m <= kNumSrc) {
      void *got = eg2_ms_clc(addr);
      ok = eg2_chk_eq("destination: the source's tag", (kTagMask >> (m - 1)) & 1u,
                      eg2_ms_cap_tag(got)) &&
           ok;
      ok = eg2_chk_true("destination: the source exactly (CSetEqualExact)",
                        __builtin_cheri_equal_exact(got, src_cap(m - 1))) &&
           ok;
    } else {
      ok = eg2_chk_true("destination map entry known", false) && ok;
    }
  }
  for (uint32_t i = 0; i < kNumJunk; ++i) {
    eg2_chk_step("junk word", i);
    ok = eg2_chk_eq("junk word: the bench's last SBA write", scratch[kScratchJunk + i],
                    eg2_ms_read32(kRegion + kOffJunk + 4 * i)) &&
         ok;
  }
  return ok;
}

#endif  // __CHERIOT__

static bool action(uint32_t kind, uint32_t arg) {
  if (kind != kEg2DmAction || arg != 0) {
    return eg2_chk_true("action known", false);
  }
#ifdef __CHERIOT__
  return run();
#else
  return eg2_chk_true("CHERIoT mode only", false);
#endif
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
      .test_id = kEg2DebugIdProgbufCap,
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
  // eg2sim: 8 + 1 + 4 checks; the chip adds 16 destination and 4 junk checks and the done word.
  return eg2_chk_summary(model_gaps() ? 13 : 30) && ok;
}
