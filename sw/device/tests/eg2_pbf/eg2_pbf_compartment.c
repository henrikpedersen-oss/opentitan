// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_compartment_<area> (EG2 verification spec REQ_PBF_22, "Post-boot features in
 * CHERIoT mode"): an area's feature reached from a compartment with only the capability it was
 * granted, and a capability fault for one access beyond the grant. CHERIoT mode only.
 *
 * A compartment here is what the CHERIoT RTOS would give one: a capability to the area's device
 * window and nothing else of it. The grant is derived from the memory root (MTDC), as the RTOS's
 * loader derives a compartment's MMIO import:
 *   grant 0 exact     the window's capability, exact bounds (eg2_dev, REQ_PER_01);
 *   grant 1 narrower  the same base, bounds cut to the smallest representable length that still
 *                     covers the registers the feature uses (CHERIoT ISA, CSetBoundsExact on a
 *                     length rounded with CRepresentableAlignmentMask's rule);
 *   grant 2 none      no capability: the window's capability with its tag cleared.
 * Then:
 *   1. The grant: tag, base and length as derived (the ISA's CGetTag, CGetBase, CGetLen).
 *   2. The feature through the grant: a register with a documented reset value reads it (the
 *      register description's reset value in the bits the generated table predicts,
 *      eg2_regmem_tables.h), and a read-write register reads back what was written, then is
 *      restored. These are the registers eg2_connectivity reads and writes per block. With no
 *      grant the first access is a CHERI fault (mcause 0x1c) with a tag violation (mtval cause
 *      0x02, CHERIoT ISA "Exception codes").
 *   3. One access beyond the grant (`outside`):
 *        0 past the window: a load one word past the grant's top: bounds violation (0x01);
 *        1 another device: a load at the next area's device window through the grant: a bounds
 *          violation, or a tag violation if moving the address that far made the capability
 *          unrepresentable (the ISA clears the tag then; the test reads the tag to know which);
 *        2 without the permission: a store through the grant with PERMIT_STORE removed: a
 *          permit-store violation (0x13); the register keeps its value.
 *      With no grant each of them is a tag violation.
 *
 * The spec's compartment runs under the CHERIoT RTOS; this tree has no RTOS port for the Earl Grey
 * 2 chip, so the compartment is bare metal (reported as a gap). The feature's full operation
 * (an encryption, a seed, a key) runs in the area's `_sb` test, whose CHERIoT build reaches the
 * device through the same exact-bounds capability; here the feature is the register interface.
 *
 * Knobs: area (0 sym_crypto/aes, 1 otbn, 2 entropy/csrng, 3 periph/uart1, 4 boot services/
 * sram_ctrl_ret, 5 keymgr/keymgr_dpe, 6 lc_otp/otp_ctrl, 7 alert/alert_handler, 8 power/pwrmgr,
 * 9 memory/sram_ctrl_main, 10 sensor/sensor_ctrl, 11 cheriot/cheriot_mem_sys; the debug area,
 * REQ_PBF_20, belongs to the debug tests), grant, outside, model_gaps (1: the mtval cause codes
 * are not checked, eg2sim).
 * Built with EG2_PBF_INJECT_FAIL the first check expects a wrong value: `PBF FAIL`.
 */

#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem_tables.h"

EG2_TEST_CONFIG();

#define COMPARTMENT_PARAMS(X) \
  X(area, 0, 0, 11)           \
  X(grant, 0, 0, 2)           \
  X(outside, 0, 0, 2)         \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(COMPARTMENT_PARAMS)

enum {
  kNone = 0xffff,
  kNumAreas = 12,
  kGrantExact = 0,
  kGrantNarrow = 1,
  kGrantNone = 2,
  kOutsidePast = 0,
  kOutsideOther = 1,
  kOutsidePerm = 2,
  // CHERIoT ISA exception codes (mtval[4:0] of a CHERI exception).
  kCapCauseBounds = 0x01,
  kCapCauseTag = 0x02,
  kCapCausePermitStore = 0x13,
  // CHERIoT ISA architectural permission bit of Store (CGetPerm).
  kPermStore = 1u << 2,
};

// The areas' devices and names: offsets, no pointers (the CHERIoT build's .data stays small).
static const uint8_t kAreaDev[kNumAreas] = {
    kEg2DevAes,          kEg2DevOtbn,          kEg2DevCsrng,      kEg2DevUart1,
    kEg2DevSramCtrlRetRegs, kEg2DevKeymgrDpe,  kEg2DevOtpCtrlCore, kEg2DevAlertHandler,
    kEg2DevPwrmgr,       kEg2DevSramCtrlMainRegs, kEg2DevSensorCtrl, kEg2DevCheriotMemSysRegs,
};
static const char kAreaNames[] =
    "sym_crypto\0otbn\0entropy\0periph\0boot_services\0keymgr\0lc_otp\0alert\0power\0memory\0"
    "sensor\0cheriot";
static const uint8_t kAreaName[kNumAreas] = {0, 11, 16, 24, 31, 45, 52, 59, 65, 71, 78, 85};

static const char *area_name(uint32_t a) { return &kAreaNames[kAreaName[a]]; }

#ifdef __CHERIOT__

static const eg2_rm_block_t *block_of(uint32_t base) {
  for (uint32_t i = 0; i < sizeof(kEg2RmBlocks) / sizeof(kEg2RmBlocks[0]); ++i) {
    if (kEg2RmBlocks[i].base == base) {
      return &kEg2RmBlocks[i];
    }
  }
  return NULL;
}

// The windows as the top description gives them (the expected base and length of a grant).
#define DEV_BASE_(id, name, base, size) (base),
#define DEV_SIZE_(id, name, base, size) (size),
static const uint32_t kDevBase[] = {EG2_DEV_TABLE(DEV_BASE_)};
static const uint32_t kDevSize[] = {EG2_DEV_TABLE(DEV_SIZE_)};
#undef DEV_BASE_
#undef DEV_SIZE_

static uint32_t dev_base(uint32_t dev) { return kDevBase[dev]; }

static uint32_t dev_size(uint32_t dev) { return kDevSize[dev]; }

static bool gaps;

/** Checks an access's trap: CHERI exception with the given cause code (unless model_gaps). */
static bool check_fault(const char *what, uint32_t mcause, const eg2_rm_out_t *out,
                        uint32_t cap_cause) {
  bool ok = eg2_pbf_check(what, kEg2RmCauseCheri, mcause);
  if (gaps) {
    eg2_pbf_model_gap("the CHERI exception code in mtval");
    return ok;
  }
  return eg2_pbf_check("  its mtval exception code", cap_cause, out->mtval & 0x1f) && ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("compartment", EG2_PBF_INJECT);
  eg2_pbf_boot_stage();
  eg2_pbf_gap("the compartment under the CHERIoT RTOS",
              "no RTOS port for the Earl Grey 2 chip in this tree; the compartment's grant is "
              "derived bare metal as the RTOS loader derives an MMIO import");
  gaps = EG2_PARAM(model_gaps) != 0;
  eg2_rm_irq_off();

  uint32_t area = EG2_PARAM(area), grant = EG2_PARAM(grant), outside = EG2_PARAM(outside);
  uint32_t dev = kAreaDev[area];
  uint32_t base = dev_base(dev), size = dev_size(dev);
  eg2_puts("PBF compartment: area ");
  eg2_puts(area_name(area));
  eg2_puts(", device ");
  eg2_puts(eg2_dev_name((eg2_dev_id_t)dev));
  eg2_puts(grant == kGrantExact ? ", grant exact" : grant == kGrantNarrow ? ", grant narrower"
                                                                         : ", no grant");
  eg2_puts(outside == kOutsidePast ? ", outside: past the window\r\n"
           : outside == kOutsideOther ? ", outside: another device\r\n"
                                      : ", outside: without the permission\r\n");

  const eg2_rm_block_t *b = block_of(base);
  if (!eg2_pbf_step("the area's device has a register table", b != NULL)) {
    return false;
  }
  const eg2_rm_reg_t *rd = b->probe_rd != kNone ? &kEg2RmRegs[b->probe_rd] : NULL;
  const eg2_rm_reg_t *wr = b->probe_wr != kNone ? &kEg2RmRegs[b->probe_wr] : NULL;
  uint32_t top = 4;
  if (rd != NULL && rd->offset + 4 > top) {
    top = rd->offset + 4;
  }
  if (wr != NULL && wr->offset + 4 > top) {
    top = wr->offset + 4;
  }

  // The grant.
  void *window = (void *)eg2_dev((eg2_dev_id_t)dev);
  void *g = window;
  uint32_t glen = size;
  if (grant == kGrantNarrow) {
    glen = (uint32_t)__builtin_cheri_round_representable_length(top);
    if (glen >= size) {
      eg2_pbf_gap("a narrower grant",
                  "the registers the feature uses span the window's representable length");
      glen = size;
    }
    g = __builtin_cheri_bounds_set_exact(window, glen);
  } else if (grant == kGrantNone) {
    g = __builtin_cheri_tag_clear(window);
  }
  ok = eg2_pbf_check("grant tag", grant == kGrantNone ? 0 : 1,
                     (uint32_t)__builtin_cheri_tag_get(g)) &&
       ok;
  ok = eg2_pbf_check("grant base", base, (uint32_t)__builtin_cheri_base_get(g)) && ok;
  ok = eg2_pbf_check("grant length", glen, (uint32_t)__builtin_cheri_length_get(g)) && ok;

  // The feature through the grant.
  eg2_rm_out_t out;
  uint32_t traps = 0;
  if (rd != NULL) {
    uint32_t cause = eg2_rm_probe((volatile uint8_t *)g + rd->offset, kEg2RmLw, 0, &out);
    if (grant == kGrantNone) {
      ok = check_fault("read without a grant (mcause)", cause, &out, kCapCauseTag) && ok;
      ++traps;
    } else {
      uint32_t known = kEg2RmMasks[rd->masks].known;
      ok = eg2_pbf_check("read through the grant (mcause)", 0, cause) && ok;
      ok = eg2_pbf_check("  its value in the documented bits (reset value)", rd->resval & known,
                         out.value & known) &&
           ok;
    }
  } else {
    eg2_pbf_gap("the read", "the device has no register the table predicts");
  }
  uint32_t wr_orig = 0, wr_bits = 0;
  bool wr_open = false;
  if (wr != NULL) {
    const eg2_rm_masks_t *m = &kEg2RmMasks[wr->masks];
    volatile uint32_t *w = (volatile uint32_t *)((volatile uint8_t *)window + wr->offset);
    wr_open = true;
    if (wr->regwen != kNone) {
      uint32_t lock = *(volatile uint32_t *)((volatile uint8_t *)window +
                                             kEg2RmRegs[wr->regwen].offset);
      wr_open = (wr->flags & kEg2RmWenMubi) ? (lock & 0xf) == 0x6 : (lock & 1) != 0;
    }
    if (wr_open) {
      wr_orig = *w;
      wr_bits = m->rw & ~m->mubi;
      uint32_t v = (wr_orig & ~m->neutral0) ^ wr_bits;
      int n = (wr->flags & kEg2RmShadowed) ? 2 : 1;
      volatile uint8_t *gw = (volatile uint8_t *)g + wr->offset;
      uint32_t cause = 0;
      for (int i = 0; i < n; ++i) {
        cause |= eg2_rm_probe(gw, kEg2RmSw, v, &out);
      }
      if (grant == kGrantNone) {
        ok = check_fault("write without a grant (mcause)", cause, &out, kCapCauseTag) && ok;
        ok = eg2_pbf_check("  the register unchanged", wr_orig & wr_bits, *w & wr_bits) && ok;
        ++traps;
      } else {
        ok = eg2_pbf_check("write through the grant (mcause)", 0, cause) && ok;
        cause = eg2_rm_probe(gw, kEg2RmLw, 0, &out);
        ok = eg2_pbf_check("  read back through the grant", v & wr_bits, out.value & wr_bits) &&
             ok;
        for (int i = 0; i < n; ++i) {
          *w = wr_orig & ~m->neutral0;
        }
      }
    } else {
      eg2_pbf_gap("the write", "the register's REGWEN is closed");
    }
  }

  // One access beyond the grant.
  uint32_t cause;
  if (outside == kOutsidePast) {
    cause = eg2_rm_probe((volatile uint8_t *)g + glen, kEg2RmLw, 0, &out);
    ok = check_fault("load one word past the grant (mcause)", cause, &out,
                     grant == kGrantNone ? kCapCauseTag : kCapCauseBounds) &&
         ok;
  } else if (outside == kOutsideOther) {
    uint32_t other = kAreaDev[(area + 1) % kNumAreas];
    uint32_t obase = dev_base(other);
    void *p = __builtin_cheri_address_set(g, obase);
    bool tagged = __builtin_cheri_tag_get(p);
    eg2_puts("PBF compartment: another device ");
    eg2_puts(eg2_dev_name((eg2_dev_id_t)other));
    eg2_puts(tagged ? " (address representable)\r\n" : " (address unrepresentable: tag cleared)\r\n");
    cause = eg2_rm_probe(p, kEg2RmLw, 0, &out);
    ok = check_fault("load at another device through the grant (mcause)", cause, &out,
                     (grant == kGrantNone || !tagged) ? kCapCauseTag : kCapCauseBounds) &&
         ok;
  } else {
    // CAndPerm without the Store permission (CHERIoT ISA permission bit 2, SD).
    void *p = g;
    __asm__ volatile("candperm %0, %0, %1" : "+C"(p) : "r"(~(uint32_t)kPermStore));
    const eg2_rm_reg_t *t = (wr != NULL && wr_open) ? wr : rd;
    if (t == NULL) {
      eg2_pbf_gap("the store without the permission", "no register to aim at");
    } else {
      volatile uint32_t *w = (volatile uint32_t *)((volatile uint8_t *)window + t->offset);
      uint32_t bits = t == wr ? wr_bits : kEg2RmMasks[t->masks].known;
      uint32_t before = *w;
      cause = eg2_rm_probe((volatile uint8_t *)p + t->offset, kEg2RmSw, before ^ bits, &out);
      ok = check_fault("store without PERMIT_STORE (mcause)", cause, &out,
                       grant == kGrantNone ? kCapCauseTag : kCapCausePermitStore) &&
           ok;
      ok = eg2_pbf_check("  the register unchanged", before & bits, *w & bits) && ok;
    }
  }
  ++traps;
  eg2_pbf_activity("capability faults", traps);
  return ok;
}

#else  // !__CHERIOT__

bool eg2_test_main(void) {
  eg2_pbf_begin("compartment", EG2_PBF_INJECT);
  eg2_pbf_gap("compartments", "CHERIoT mode only");
  return false;
}

#endif  // __CHERIOT__
