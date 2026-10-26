// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_boot_handover_protection and _cheriot (EG2 verification spec REQ_BOOT_10, "Secure
 * boot in RISC-V mode", "Twins of the RISC-V tests"): each stage hands over with execution limited
 * to what it verified. From the owner image, behind the full secure-boot chain (sim_dv_rom_ext):
 *
 *   path 0 fetch    outside the owner image's code: in ROM_EXT's slot and in the owner slot past
 *                   the image. RISC-V mode: an instruction access fault (ROM_EXT's ePMP: only the
 *                   owner's code TOR region is executable, secure boot "ROM_EXT" step 7).
 *                   CHERIoT mode: through a capability the owner derives for its own code (bounded
 *                   to [_text_start, _text_end)): a CHERI exception (bounds). The same fetch
 *                   through the executable root is reported, not checked (REQ_SEC_26 note).
 *   path 1 store    into ROM_EXT's slot through the host window: a store access fault (the RRAM
 *                   host window takes reads only).
 *   path 2 rram_ctrl write into ROM_EXT's slot and into a Creator info page through the
 *                   controller: ERR_CODE.mp_err (ROM_EXT protects its own slot read-only and locks
 *                   the Creator pages: rom_ext.c rom_ext_nvm_protect_self,
 *                   nvm_ctrl_creator_info_pages_lockdown; secure boot "ROM_EXT": the Silicon
 *                   Creator regions are not writable). The Creator pages are the real info pages
 *                   (CreatorSecret, FactoryId, AttestationKeySeeds) and the emulated ones
 *                   (BootData0/1, CreatorReserved0). A write that would be accepted writes the
 *                   word's own value back, so it changes nothing.
 *   path 3 re-enable a locked region: MP_REGION_CFG_0 (ROM_EXT's slot A) and INFO_PAGE_CFG_5 with
 *                   write enabled: they read back unchanged and their REGWEN reads 0.
 *   path 4 SBA      by the bench (chip_sw_eg2_boot_handover_protection_vseq): system bus writes
 *                   into ROM_EXT's slot and into MP_REGION_CFG_0 in a debug-enabled state; the
 *                   bench checks the bus error, the register, and ROM_EXT's word by backdoor, and
 *                   writes its verdict into `kEg2BenchVerdict`.
 * `paths` selects them (bits 0-4); the signed image keeps its defaults (all).
 *
 * Built with EG2_BOOT_HANDOVER_PROTECTION_INJECT_FAIL the store into ROM_EXT's slot is expected
 * to complete: the run must fail with `EG2CHK FAIL host store into ROM_EXT's slot`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define HANDOVER_PARAMS(X) X(paths, 31, 1, 31)
EG2_RM_PARAMS(HANDOVER_PARAMS)

enum {
  kRram = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
  kRomExtSlotA = 0x0,
  kRomExtSize = 0x10000,
  // A word ROM_EXT's image does not reach (zero-filled by the assembly): its slot's last word.
  kRomExtTail = kRomExtSlotA + kRomExtSize - 16,
  // The owner slot past this image, below the emulated info pages at slot A's end.
  kOwnerTail = 0xd0000,
  kPage = RRAM_CTRL_PARAM_BYTES_PER_PAGE,
  kEmulBaseB = RRAM_CTRL_PARAM_NUM_DATA_PAGES - 0x8000 / RRAM_CTRL_PARAM_BYTES_PER_PAGE,
  kEmulBaseA = RRAM_CTRL_PARAM_NUM_DATA_PAGES / 2 - 0x8000 / RRAM_CTRL_PARAM_BYTES_PER_PAGE,
  kSbaReady = 0x41425353,  // "SSBA"
  kBenchDone = 0x454e4f44,  // "DONE"
};

/** For the bench: kSbaReady when the software's checks are done and the SBA path may start. */
volatile uint32_t kEg2SbaReady;
/** From the bench: kBenchDone, then 0 (pass) or the number of failed bench checks. */
volatile uint32_t kEg2BenchDone;
volatile uint32_t kEg2BenchVerdict;

static uint32_t text_start(void) {
  uint32_t a;
  __asm__("lui %0, %%hi(_text_start)\n addi %0, %0, %%lo(_text_start)" : "=r"(a));
  return a;
}

static uint32_t text_end(void) {
  uint32_t a;
  __asm__("lui %0, %%hi(_text_end)\n addi %0, %0, %%lo(_text_end)" : "=r"(a));
  return a;
}

static bool fetch(const char *what, uint32_t addr) {
  eg2_boot_out_t out;
#ifdef __CHERIOT__
  uint32_t lo = text_start();
  uint32_t cause = eg2_boot_probe_cap(eg2_boot_code_cap(lo, text_end() - lo, addr), kEg2BootFetch,
                                      0, &out);
  bool ok = eg2_boot_check(what, kEg2CauseCheri, cause);
  // The same fetch through the executable root: what the hardware alone allows (REQ_SEC_26).
  uint32_t root = eg2_boot_probe(addr, kEg2BootFetch, 0, &out);
  eg2_boot_note("  through the executable root, mcause", root);
  return ok;
#else
  return eg2_boot_check(what, kEg2CauseFetchFault, eg2_boot_probe(addr, kEg2BootFetch, 0, &out));
#endif
}

/** A controller write of the words already there; must end with mp_err. */
static bool ctrl_write(const char *what, uint32_t partition, uint32_t addr) {
  uint32_t words[4] = {0, 0, 0, 0};
  eg2_rram_res_t r = eg2_rram_op(kEg2RramOpRead, partition, addr, 4, words);
  eg2_boot_note("  read before the write, ERR_CODE", r.err_code);
  r = eg2_rram_op(kEg2RramOpWrite, partition, addr, 4, words);
  return eg2_boot_check(what, kEg2RramErrMp, r.err_code & kEg2RramErrMp);
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA) && ok;
  eg2_boot_irq_off();
  int recov = eg2_status_alert_find("rram_ctrl_recov_err");
  if (recov >= 0) {
    eg2_status_expect_alert((size_t)recov);
  }
  uint32_t paths = EG2_PARAM(paths);
  eg2_boot_note("owner code start", text_start());
  eg2_boot_note("owner code end", text_end());

  if (paths & 1) {
    ok = fetch("fetch in ROM_EXT's slot refused", kRram + kRomExtTail) && ok;
    ok = fetch("fetch in the owner slot past the image refused", kRram + kOwnerTail) && ok;
  }
  if (paths & 2) {
    eg2_boot_out_t out;
    uint32_t before = eg2_boot_rd32(kRram + kRomExtSlotA + 0x400);
    uint32_t cause = eg2_boot_probe(kRram + kRomExtSlotA + 0x400, kEg2BootStore, ~before, &out);
#ifdef EG2_BOOT_HANDOVER_PROTECTION_INJECT_FAIL
    ok = eg2_boot_check("host store into ROM_EXT's slot", kEg2CauseNone, cause) && ok;
#else
    ok = eg2_boot_check("host store into ROM_EXT's slot", kEg2CauseStoreFault, cause) && ok;
#endif
    ok = eg2_boot_check("ROM_EXT's word unchanged", before,
                        eg2_boot_rd32(kRram + kRomExtSlotA + 0x400)) &&
         ok;
  }
  if (paths & 4) {
    ok = ctrl_write("controller write into ROM_EXT's slot: mp_err", kEg2RramPartData,
                    kRomExtSlotA + 0x400) &&
         ok;
    ok = ctrl_write("controller write into CreatorSecret (info 5): mp_err", kEg2RramPartInfo,
                    5 * kPage) &&
         ok;
    ok = ctrl_write("controller write into FactoryId (info 0): mp_err", kEg2RramPartInfo, 0) &&
         ok;
    ok = ctrl_write("controller write into AttestationKeySeeds (info 1): mp_err",
                    kEg2RramPartInfo, 1 * kPage) &&
         ok;
    ok = ctrl_write("controller write into BootData0 (emulated): mp_err", kEg2RramPartData,
                    (kEmulBaseB + 32) * kPage) &&
         ok;
    ok = ctrl_write("controller write into BootData1 (emulated): mp_err", kEg2RramPartData,
                    (kEmulBaseB + 36) * kPage) &&
         ok;
    ok = ctrl_write("controller write into CreatorReserved0 (emulated): mp_err", kEg2RramPartData,
                    kEmulBaseA * kPage) &&
         ok;
  }
  if (paths & 8) {
    uint32_t cfg = eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET);
    ok = eg2_boot_check("REGION_CFG_REGWEN_0 locked", 0,
                        eg2_rram_reg(RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET)) &&
         ok;
    eg2_rram_reg_wr(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET, 0x66666);  // every field MuBi4True
    ok = eg2_boot_check("MP_REGION_CFG_0 unchanged after the re-enable", cfg,
                        eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET)) &&
         ok;
    uint32_t info = eg2_rram_reg(RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * 5);
    ok = eg2_boot_check("INFO_REGWEN_5 locked", 0,
                        eg2_rram_reg(RRAM_CTRL_INFO_REGWEN_0_REG_OFFSET + 4 * 5)) &&
         ok;
    eg2_rram_info_cfg(5, true, true, true, true);
    ok = eg2_boot_check("INFO_PAGE_CFG_5 unchanged after the re-enable", info,
                        eg2_rram_reg(RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * 5)) &&
         ok;
    ok = ctrl_write("controller write into ROM_EXT's slot after the re-enable: mp_err",
                    kEg2RramPartData, kRomExtSlotA + 0x400) &&
         ok;
  }
  if (paths & 16) {
    kEg2BenchDone = 0;
    kEg2SbaReady = kSbaReady;
    uint32_t polls = 0;
    while (kEg2BenchDone != kBenchDone && polls < 2000000) {
      ++polls;
    }
    ok = eg2_boot_check("bench's SBA path done", kBenchDone, kEg2BenchDone) && ok;
    ok = eg2_boot_check("bench's SBA checks", 0, kEg2BenchVerdict) && ok;
  }
  return eg2_boot_summary() && ok;
}
