// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_epmp_equivalents and _cheriot (EG2 verification spec REQ_SEC_26, "Security in
 * RISC-V mode"): CHERIoT mode weakens no protection OpenTitan gets from the ePMP. Behind the full
 * secure-boot chain (sim_dv_rom_ext). For each ePMP rule ROM_EXT leaves for the owner image
 * (rom_ext.c rom_ext_init and rom_ext_boot), its violation, and the cause each mode must give:
 *
 *   rule                                   RISC-V (ePMP)  CHERIoT (the mechanism that replaces it)
 *   0 ROM_EXT's code not executable        fetch fault 1  capability bounds: a code capability
 *                                                          derived for the owner's code, CHERI 0x1c
 *   1 the owner's code not writable        store fault 7  the code capability has no store
 *                                                          permission (PCC-derived), CHERI 0x1c
 *   2 the RRAM not writable from the host  store fault 7  the RRAM host window (rram_ctrl takes
 *                                                          reads only), store fault 7
 *   3 the sec SRAM not executable (entry 5 fetch fault 1  sram_ctrl: the sec SRAM takes no
 *     locked RW)                                           instruction fetch, fetch fault 1
 *   4 the main SRAM not executable         fetch fault 1  sram_ctrl EXEC (as the owner
 *                                                          configuration set it; if EXEC is on, the
 *                                                          rule is not in force in either mode)
 *   5 devices not executable (MMIO RW)     fetch fault 1  the devices refuse fetches (TL-UL
 *                                                          instruction type), fetch fault 1
 * In CHERIoT mode the fetches of rules 3-5 go through the executable root (what the hardware alone
 * enforces); rule 0's fetch through the root is reported (the root is the owner loader's; see the
 * spec note on REQ_SEC_26). `rules` selects them (bits 0-5).
 *
 * Built with EG2_SEC_EPMP_EQUIVALENTS_INJECT_FAIL the device fetch is expected to be allowed: the
 * run must fail with `EG2CHK FAIL rule 5`.
 */

#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define EQUIV_PARAMS(X) X(rules, 63, 1, 63)
EG2_RM_PARAMS(EQUIV_PARAMS)

enum {
  kRram = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
  kRomExtTail = 0x10000 - 16,
};

/** A zero word in main SRAM to fetch from. */
volatile uint32_t kEg2ZeroWord[4];

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

static uint32_t own_code_cap_probe(uint32_t addr, eg2_boot_op_t op) {
  eg2_boot_out_t out;
#ifdef __CHERIOT__
  uint32_t lo = text_start();
  return eg2_boot_probe_cap(eg2_boot_code_cap(lo, text_end() - lo, addr), op, 0, &out);
#else
  return eg2_boot_probe(addr, op, 0, &out);
#endif
}

static uint32_t root_probe(uint32_t addr, eg2_boot_op_t op, uint32_t wdata) {
  eg2_boot_out_t out;
  return eg2_boot_probe(addr, op, wdata, &out);
}

#ifdef __CHERIOT__
static const uint32_t kExpCode = kEg2CauseCheri;
#else
static const uint32_t kExpCode = kEg2CauseFetchFault;
#endif

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA) && ok;
  eg2_boot_irq_off();
  uint32_t rules = EG2_PARAM(rules);

  if (rules & 1) {
    ok = eg2_boot_check("rule 0: fetch ROM_EXT's code", kExpCode,
                        own_code_cap_probe(kRram + kRomExtTail, kEg2BootFetch)) &&
         ok;
#ifdef __CHERIOT__
    eg2_boot_note("  rule 0 through the executable root, mcause",
                  root_probe(kRram + kRomExtTail, kEg2BootFetch, 0));
#endif
  }
  if (rules & 2) {
    uint32_t addr = text_start() + 0x100;
#ifdef __CHERIOT__
    ok = eg2_boot_check("rule 1: store into the owner's code", kEg2CauseCheri,
                        own_code_cap_probe(addr, kEg2BootStore)) &&
         ok;
#else
    ok = eg2_boot_check("rule 1: store into the owner's code", kEg2CauseStoreFault,
                        root_probe(addr, kEg2BootStore, 0)) &&
         ok;
#endif
  }
  if (rules & 4) {
    ok = eg2_boot_check("rule 2: host store into the RRAM", kEg2CauseStoreFault,
                        root_probe(kRram + 0x400, kEg2BootStore, 0)) &&
         ok;
  }
  if (rules & 8) {
    uint32_t addr = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 0x100;
    // The word is made 0, so an allowed fetch traps as an illegal instruction.
    eg2_boot_out_t out;
    (void)eg2_boot_probe(addr, kEg2BootStore, 0, &out);
    ok = eg2_boot_check("rule 3: fetch from the sec SRAM", kEg2CauseFetchFault,
                        root_probe(addr, kEg2BootFetch, 0)) &&
         ok;
  }
  if (rules & 16) {
    uint32_t exec = eg2_dev_read32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET) & 0xf;
    eg2_boot_note("main SRAM EXEC (owner configuration)", exec);
    kEg2ZeroWord[0] = 0;
    uint32_t addr = 0;
#ifdef __CHERIOT__
    addr = __builtin_cheri_address_get((void *)kEg2ZeroWord);
#else
    addr = (uint32_t)(uintptr_t)kEg2ZeroWord;
#endif
    uint32_t cause = root_probe(addr, kEg2BootFetch, 0);
    if (exec == kMultiBitBool4True) {
      eg2_boot_note("  rule 4 not in force (EXEC on), fetch mcause", cause);
    } else {
      ok = eg2_boot_check("rule 4: fetch from the main SRAM", kEg2CauseFetchFault, cause) && ok;
    }
  }
  if (rules & 32) {
    uint32_t cause = root_probe(TOP_EARLGREY_UART0_BASE_ADDR + 0x10, kEg2BootFetch, 0);
#ifdef EG2_SEC_EPMP_EQUIVALENTS_INJECT_FAIL
    ok = eg2_boot_expect("rule 5: fetch from a device", cause != kEg2CauseFetchFault, cause) && ok;
#else
    ok = eg2_boot_check("rule 5: fetch from a device", kEg2CauseFetchFault, cause) && ok;
#endif
  }
  return eg2_boot_summary() && ok;
}
