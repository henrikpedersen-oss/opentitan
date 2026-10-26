// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_sweep_irq (EG2 spec "Revocation"; REQ_CMS_12).
 *
 * The TBRE completion interrupt reaches the core through the PLIC when a sweep ends, and every
 * tag of that sweep is final by then; sweeps cover main and sec SRAM and RRAM (RFC CHERIoT Memory
 * Subsystem "Optional Background Revocation Engine"; cheriot_mem_sys theory_of_operation.md
 * "Revocation Engine": the epoch counts each completed sweep by 2, `tbre_done` when the engine
 * stops being active).
 *
 * Slots of capabilities in the swept memory (`sweep_region`: sec SRAM's arena B in the main/sec
 * SRAM region, or the test's RRAM area, where each capability is programmed through rram_ctrl
 * and then tagged by a verified CSC), each based on an object in sec SRAM's arena A, `n_marked`
 * of whose granules are marked in the bitmap. The sweep (`sweep_size`: one capability, a small
 * range of 2-64, or from the region's base to its top) starts at the first slot. The test waits
 * for the interrupt (`wait`: WFI with the PLIC source enabled, or polling the PLIC's pending bit),
 * then:
 *  - the PLIC claim returns tbre_done's source, TBRE_STATUS is idle with no error, and
 *    TBRE_EPOCH advanced by 2;
 *  - the bitmap is cleared, so the load filter no longer revokes, and every slot is reloaded: a
 *    swept slot based in a marked granule must have lost its tag in memory, every other slot keeps
 *    it.
 *
 * The SystemC model's engine (the Earl Grey 2 board) sweeps without revoking: with `model_gaps`
 * the revoked slots' tags are not checked.
 *
 * Built with EG2_TBRE_SWEEP_IRQ_INJECT_FAIL, the epoch is expected to advance by 4: the run must
 * fail with `MISMATCH epoch`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define TBRE_SWEEP_IRQ_PARAMS(X) \
  X(sweep_region, 0, 0, 2)       \
  X(sweep_size, 0, 0, 3)         \
  X(n_marked, 65, 0, 65)         \
  X(wait, 2, 0, 2)               \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TBRE_SWEEP_IRQ_PARAMS);

enum {
  kSlots = 64,
  kObjects = kEg2ArenaA,  // the objects the slots point at, one granule each
  kSramSlots = kEg2ArenaB,
  kTags = kEg2WorkBase,   // per slot: tag after the sweep
  kMarkedSet = kEg2WorkBase + 4 * kSlots,  // per object: marked
  kMipMeip = 1u << 11,
};

static eg2_tally_t tally;

static uint32_t slot_addr(bool rram, uint32_t i) {
  // In RRAM one capability per 16-byte RRAM word, so that programming one leaves the others.
  return rram ? kEg2MsRramArea + 16 * i : kSramSlots + 8 * i;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t region = EG2_PARAM(sweep_region) != 0 ? EG2_PARAM(sweep_region) : eg2_rand_range(1, 2);
  bool rram = region == 2;
  uint32_t size = EG2_PARAM(sweep_size);
  if (size == 0) {
    uint32_t d = eg2_rand_range(0, 99);
    size = d < 20 ? 1 : d < 60 ? 2 : 3;
  }
  uint32_t n_marked = EG2_PARAM(n_marked);
  if (n_marked == 65) {
    n_marked = eg2_rand_range(0, 9) == 0 ? 0 : eg2_rand_range(1, 64);
  }
  uint32_t wait = EG2_PARAM(wait) == 2 ? eg2_pick(2) : EG2_PARAM(wait);
  eg2_puts(rram ? "EG2 tbre_sweep_irq: RRAM" : "EG2 tbre_sweep_irq: main and sec SRAM");
  eg2_put_kv(" size class", size);
  eg2_put_kv(" n_marked", n_marked);
  eg2_puts(wait == 0 ? " wait WFI\r\n" : " wait polling\r\n");
  if (rram && !eg2_ms_rram_setup()) {
    eg2_puts("EG2 rram_ctrl initialisation did not complete\r\n");
    return false;
  }
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();

  // The objects' marks: n_marked of kSlots objects.
  for (uint32_t i = 0; i < kSlots; ++i) {
    eg2_ms_write32(kMarkedSet + 4 * i, 0);
  }
  for (uint32_t k = 0; k < n_marked; ++k) {
    eg2_ms_write32(kMarkedSet + 4 * eg2_pick(kSlots), 1);
  }

  // The slots: slot i points at object i.
  for (uint32_t i = 0; i < kSlots; ++i) {
    void *c = eg2_ms_object_cap(kObjects + 8 * i, 8, false);
    uint32_t a = slot_addr(rram, i);
    if (rram) {
      uint32_t w[4] = {eg2_ms_cap_addr(c), eg2_ms_cap_high(c), 0, 0};
      uint32_t err;
      if (!eg2_ms_rram_program(a, w, 4, &err)) {
        eg2_ms_note("rram_ctrl program failed, ERR_CODE", err);
        return false;
      }
    }
    eg2_ms_csc(a, c);
    if (!eg2_check(&tally, "slot tagged before the sweep", i, eg2_ms_tag(a), 1)) {
      return false;
    }
  }
  for (uint32_t i = 0; i < kSlots; ++i) {
    if (eg2_ms_read32(kMarkedSet + 4 * i)) {
      eg2_ms_revbm_mark(kObjects + 8 * i, true);
    }
  }

  // The sweep: from the first slot over one or a few capabilities, or the whole region.
  // A whole-RRAM sweep stops below the OTP pages, which the host window does not reach: a sweep
  // into them reads error responses and sets TBRE_STATUS.sweep_err (regr-vlt 2026-10-26).
  uint32_t top = rram ? kEg2MsNvmHostTop : kEg2MsHeapTop;
  uint32_t base = size == 3 ? (rram ? kEg2MsNvmBase : kEg2MsHeapBase) : slot_addr(rram, 0);
  uint32_t n_caps = size == 1 ? 1 : size == 2 ? eg2_rand_range(2, 64) : (top - base) / 8;
  // RRAM slots are 16 bytes apart: the swept slots are those below base + 8 * n_caps.
  uint32_t swept_top = base + 8 * n_caps;
  eg2_put_kv("EG2 sweep base", base);
  eg2_put_kv(" caps", n_caps);
  eg2_puts("\r\n");
  eg2_ms_plic_enable(kEg2MsTbreIrq, true);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_ENABLE_TBRE_DONE_BIT);
  uint32_t mie_old;
  __asm__ volatile("csrrs %0, mie, %1" : "=r"(mie_old) : "r"(kMipMeip));
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(base, n_caps, &sw);
  eg2_check(&tally, "start taken", 0, sw.started ? 1 : 0, 1);
  uint32_t polls = 0;
  if (wait == 0) {
    // mstatus.MIE is clear: the pending interrupt ends the WFI without a trap.
    while (!eg2_ms_plic_pending(kEg2MsTbreIrq) && polls < 64) {
      __asm__ volatile("wfi");
      ++polls;
    }
  } else {
    while (!eg2_ms_plic_pending(kEg2MsTbreIrq) && polls < 10000000) {
      ++polls;
    }
  }
  uint32_t id = eg2_ms_plic_claim();
  // At the interrupt: the bitmap cleared first, so the reload sees what the sweep left in memory.
  eg2_ms_revbm_clear();
  for (uint32_t i = 0; i < kSlots; ++i) {
    eg2_ms_write32(kTags + 4 * i, eg2_ms_tag(slot_addr(rram, i)));
  }
  eg2_ms_tbre_wait(&sw, 10);
  eg2_check(&tally, "PLIC claim", 0, id, kEg2MsTbreIrq);
  eg2_ms_plic_complete(id);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_STATE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_STATE_TBRE_DONE_BIT);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_ENABLE_REG_OFFSET, 0);
  eg2_ms_plic_enable(kEg2MsTbreIrq, false);
  __asm__ volatile("csrw mie, %0" : : "r"(mie_old));
  eg2_check(&tally, "idle at the interrupt", 0, sw.done ? 1 : 0, 1);
  eg2_check(&tally, "TBRE_STATUS", 0, sw.status, 0);
  uint32_t exp_epoch = sw.epoch_before + 2;
#ifdef EG2_TBRE_SWEEP_IRQ_INJECT_FAIL
  exp_epoch += 2;
#endif
  eg2_check(&tally, "epoch", 0, sw.epoch_after, exp_epoch);

  for (uint32_t i = 0; i < kSlots; ++i) {
    ++tally.planned;
    bool swept = slot_addr(rram, i) < swept_top;
    bool revoked = swept && eg2_ms_read32(kMarkedSet + 4 * i);
    if (!(gaps && revoked)) {
      eg2_check(&tally, revoked ? "revoked slot's tag" : "slot's tag", i,
                eg2_ms_read32(kTags + 4 * i), revoked ? 0 : 1);
    }
    ++tally.done;
  }
  eg2_tally_print("tbre_sweep_irq", &tally);
  return eg2_tally_ok(&tally);
}
