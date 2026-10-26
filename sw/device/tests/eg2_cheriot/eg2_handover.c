// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_handover (EG2 spec "Mode switch and handover"; REQ_SOC_07, REQ_BOOT_03).
 *
 * After the switch, PCC is the executable root and execution continues at the next PC; MTCC and
 * MEPCC are the executable root with the address of mtvec and mepc; MTDC and MScratchC are the
 * memory and sealing roots; every general-purpose register's tag is 0, whatever ePMP mode left in
 * x16-x31; at every switch point (RFC CHERIoT Ibex "Handover to CHERIoT Mode Software").
 *
 * The switch and the capture are in eg2_handover.S (RISC-V mode, then the first CHERIoT-mode
 * instructions); this checks what was captured, against the capability model's roots
 * (CHERIoT-Sail, eg2_cap_model.h):
 *  - x1-x15: tag 0 and the value RISC-V mode left (the fill table);
 *  - PCC: the executable root at the address of the capture code's AUIPCC;
 *  - MTCC, MEPCC: the executable root at mtvec's and mepc's values (as the core held them after
 *    the RISC-V-mode writes);
 *  - MTDC: the memory root (its address is the capture area, set by the capture code);
 *  - MScratchC: the sealing root (at the address it had);
 *  - misa: E set, I clear (REQ_SOC_03).
 * No alert: the framework's final status sweep. The bench checks every instruction from the one
 * after the lock store (the scoreboard's handover, decision 2026-10-23).
 *
 * Differences from the spec's stimulus: the registers are filled by assembly, not compiled C (the
 * image is a CHERIoT image, whose C cannot run before the switch); boot_mode is the dvsim entry's
 * (the `_secure_boot` entry), not a knob.
 *
 * Built with EG2_HANDOVER_INJECT_FAIL, x1 is expected tagged: the run must fail with
 * `MISMATCH x tag #1`.
 */

#include "hw/top/rv_timer_regs.h"
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_handover_params.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"

EG2_LATE_TEST_PARAMS(EG2_HANDOVER_PARAMS);

extern char eg2_handover_pcc_at[];

static eg2_tally_t tally;

static const char *switch_point_name(uint32_t sp) {
  switch (sp) {
    case 0:
      return "a store (before the lock)";
    case 1:
      return "WFI (after the lock)";
    case 2:
      return "a taken branch (after the lock)";
    case 3:
      return "a CSR write (after the lock)";
    default:
      return "a load (before the lock)";
  }
}

/** Compares a captured register with the model's capability: tag, high word, address. */
static void check_cap(const char *what, uint32_t idx, eg2_cap_t got, eg2_cap_t exp,
                      bool check_addr) {
  uint32_t failed = tally.failed;
  eg2_check(&tally, what, idx, got.tag ? 1u : 0u, exp.tag ? 1u : 0u);
  eg2_check(&tally, what, idx, got.hi, exp.hi);
  if (check_addr) {
    eg2_check(&tally, what, idx, got.addr, exp.addr);
  }
  if (tally.failed != failed && failed < kEg2MaxReports) {
    eg2_cap_print("hw   ", got);
    eg2_cap_print("model", exp);
  }
}

bool eg2_test_main(void) {
  // The WFI switch point's timer: off, and its interrupt cleared.
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
  __asm__ volatile("csrc mie, %0" : : "r"(0x80u));

  uint32_t sp = eg2_late_word(EG2_LATE_INFO + EG2_HANDOVER_INFO_SWITCH_POINT);
  uint32_t fill = eg2_late_word(EG2_LATE_INFO + EG2_HANDOVER_INFO_FILL);
  uint32_t mtvec = eg2_late_word(EG2_LATE_INFO + EG2_HANDOVER_INFO_MTVEC);
  uint32_t mepc = eg2_late_word(EG2_LATE_INFO + EG2_HANDOVER_INFO_MEPC);
  eg2_puts("EG2 handover: switch point ");
  eg2_puts(switch_point_name(sp));
  eg2_put_kv(", fill class", fill);
  eg2_put_kv(", mtvec", mtvec);
  eg2_put_kv(", mepc", mepc);
  eg2_put_kv(", cpuctrlsts", eg2_late_word(EG2_LATE_INFO + EG2_HANDOVER_INFO_CPUCTRL));
  eg2_puts("\r\n");

  // x1-x15: integers, tag 0, the values RISC-V mode left.
  for (uint32_t i = 1; i < 16; ++i) {
    ++tally.planned;
    eg2_cap_t got = eg2_hw_read(eg2_late_cap(EG2_LATE_X(i)));
    uint32_t exp_tag = 0;
#ifdef EG2_HANDOVER_INJECT_FAIL
    if (i == 1) {
      exp_tag = 1;
    }
#endif
    eg2_check(&tally, "x tag", i, got.tag ? 1u : 0u, exp_tag);
    eg2_check(&tally, "x value", i, got.addr, eg2_late_word(EG2_LATE_FILL + 4 * i));
    ++tally.done;
  }

  // PCC: the executable root, at the capture code's AUIPCC.
  ++tally.planned;
  uint32_t pcc_at = EG2_ADDR(eg2_handover_pcc_at);
  check_cap("PCC", 0, eg2_hw_read(eg2_late_cap(EG2_LATE_PCC)), eg2_cap_root_exe(pcc_at), true);
  ++tally.done;

  // MTCC and MEPCC: the executable root at mtvec's and mepc's values. MTCC's address bits [1:0]
  // read 0 (core REQ_SCR_02: CHERIoT has direct mode only), so a vectored mtvec (MODE 01, as the
  // fill sets) shows without its mode bit (F39, fixed 2026-10-25).
  ++tally.planned;
  check_cap("MTCC", 0, eg2_hw_read(eg2_late_cap(EG2_LATE_MTCC)), eg2_cap_root_exe(mtvec & ~3u),
            true);
  ++tally.done;
  ++tally.planned;
  check_cap("MEPCC", 0, eg2_hw_read(eg2_late_cap(EG2_LATE_MEPCC)), eg2_cap_root_exe(mepc), true);
  ++tally.done;

  // MTDC: the memory root (the capture code moved its address to the capture area).
  ++tally.planned;
  check_cap("MTDC", 0, eg2_hw_read(eg2_late_cap(EG2_LATE_MTDC)),
            eg2_cap_root_mem(eg2_late_area()), true);
  ++tally.done;

  // MScratchC: the sealing root, wherever its address was.
  ++tally.planned;
  eg2_cap_t sc = eg2_hw_read(eg2_late_cap(EG2_LATE_MSCRATCHC));
  check_cap("MScratchC", 0, sc, eg2_cap_root_seal(sc.addr), false);
  ++tally.done;

  // misa: CHERIoT mode (E set, I clear).
  ++tally.planned;
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  eg2_check(&tally, "misa E,I", 0, misa & ((1u << 4) | (1u << 8)), 1u << 4);
  ++tally.done;

  eg2_tally_print("handover", &tally);
  return eg2_tally_ok(&tally);
}
