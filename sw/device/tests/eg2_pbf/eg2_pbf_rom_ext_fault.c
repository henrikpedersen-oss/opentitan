// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_rom_ext_fault (EG2 verification spec REQ_PBF_14, REQ_SEC_14, "Post-boot
 * faults"): a reset while ROM_EXT carries out a boot-service request; the state it keeps is the
 * old one or the new one, never neither. Behind secure boot only: the boot services are ROM_EXT's.
 *
 * Boot 1 (owner image behind ROM_EXT): the primary BL0 slot from ROM_EXT's boot log is recorded;
 * the test requests the other slot as primary (boot service "next BL0 slot", primary changed, next
 * unspecified: the request ROM_EXT keeps in its boot data in the RRAM), writes it into the
 * retention SRAM's boot-service area, asks the bench to arm the fault (bench step 1) and resets.
 * The bench (chip_sw_eg2_pbf_rom_ext_fault_vseq) waits for the RRAM controller to program after
 * that reset (ROM_EXT writing its boot data) and power-on resets the chip `fault_delay` cycles in.
 * Boot 2: the primary slot ROM_EXT reports is the old one or the requested one. Slot B holds no
 * image, so ROM_EXT boots slot A either way (its boot policy tries the other slot when the primary
 * fails verification). The test's own progress is kept in the RRAM (a word of the OTTF
 * non-volatile scratch area under a data region of its own), which a power-on reset keeps.
 *
 * `fault_point` 1 (ownership activation) is a gap: activation needs a signed ownership block for
 * the fake owner keys, which the sim_dv_rom_ext environment does not build; the run does the boot
 * service instead.
 *
 * Without ROM_EXT (the test ROM, the SystemC model, which has no OTBN for the ROM's signature
 * check) there is no boot service: the test checks only that it booted the way its run mode says
 * (ROM_EXT on the chip; the test ROM with model_gaps = 1) and reports the rest as not run.
 *
 * Knobs: fault_point, fault_delay (the bench's), model_gaps.
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "hw/top/rstmgr_regs.h"  // Generated.
#include "sw/device/silicon_creator/lib/boot_data.h"
#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_msg.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define ROM_EXT_FAULT_PARAMS(X)   \
  X(fault_point, 0, 0, 1)         \
  X(fault_delay, 200, 0, 100000)  \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(ROM_EXT_FAULT_PARAMS)

enum {
  kMagic = 0x524f4d58,  // "ROMX"
  kBootLogPrimaryWord = 22,  // boot_log_t.primary_bl0_slot (offset 88)
};

/** The test's progress word: the first RRAM word of the OTTF non-volatile scratch area. */
static bool progress_region(void) {
  return eg2_pbf_rram_region(eg2_pbf_nv_scratch(), 1, true, true);
}

static bool progress_write(uint32_t phase, uint32_t old_primary) {
  uint32_t w[4] = {kMagic, phase, old_primary, 0};
  return eg2_pbf_rram_program(eg2_pbf_nv_scratch(), w, 4);
}

static const char *slot_name(uint32_t s) {
  return s == kBootSlotA ? "A" : s == kBootSlotB ? "B" : "?";
}

static bool boot_request(void) {
  uint32_t old = eg2_pbf_boot_log_word(kBootLogPrimaryWord);
  uint32_t req = old == kBootSlotB ? kBootSlotA : kBootSlotB;
  eg2_puts("PBF rom_ext_fault: primary slot ");
  eg2_puts(slot_name(old));
  eg2_puts(", requesting ");
  eg2_puts(slot_name(req));
  eg2_puts("\r\n");
  if (EG2_PARAM(fault_point) == 1) {
    eg2_pbf_gap("ownership activation",
                "needs a signed ownership block for the fake owner keys; the boot service runs");
  }
  bool ok = eg2_pbf_step("progress recorded", progress_write(1, old));
  boot_svc_msg_t msg;
  boot_svc_next_boot_bl0_slot_req_init(req, kBootSlotUnspecified, &msg.next_boot_bl0_slot_req);
  retention_sram_get()->creator.boot_svc_msg = msg;
  eg2_pbf_bench_step(1);  // the bench power-on resets the chip while ROM_EXT writes
  eg2_pbf_sw_reset();
  return ok;
}

static bool boot_after(uint32_t old) {
  uint32_t now = eg2_pbf_boot_log_word(kBootLogPrimaryWord);
  uint32_t req = old == kBootSlotB ? kBootSlotA : kBootSlotB;
  eg2_puts("PBF rom_ext_fault: primary slot now ");
  eg2_puts(slot_name(now));
  eg2_puts("\r\n");
  uint32_t reason = eg2_pbf_reset_reason();
  eg2_pbf_note("reset reason", reason);
  eg2_pbf_activity("power-on resets during the boot service",
                   (reason >> RSTMGR_RESET_INFO_POR_BIT) & 1);
  bool ok = eg2_pbf_check("primary slot old or new (1)", 1, now == old || now == req);
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("rom_ext_fault", EG2_PBF_INJECT);
  eg2_pbf_boot_stage_t stage = eg2_pbf_boot_stage();
  if (stage != kEg2PbfBootRomExt) {
    eg2_pbf_gap("boot service under a reset", "not run behind secure boot: no ROM_EXT");
    return eg2_pbf_check("boot stage (1 ROM_EXT, 0 the test ROM with model_gaps)",
                         EG2_PARAM(model_gaps) ? kEg2PbfBootTestRom : kEg2PbfBootRomExt,
                         stage) &&
           ok;
  }
  ok = eg2_pbf_step("progress region", progress_region()) && ok;
  volatile uint32_t *p = eg2_pbf_word(eg2_pbf_nv_scratch());
  if (p[0] != kMagic || p[1] == 0) {
    return boot_request() && ok;
  }
  ok = boot_after(p[2]) && ok;
  ok = eg2_pbf_step("progress reset", progress_write(0, 0)) && ok;
  return ok;
}
