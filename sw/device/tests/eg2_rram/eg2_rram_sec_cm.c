// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The RRAM controller's countermeasure tests (EG2 verification spec REQ_RRAM_08, REQ_RRAM_11,
 * REQ_RRAM_24; RFC RRAM Controller §4.5 pp. 11-13; rram_ctrl.hjson `countermeasures`,
 * STD_FAULT_STATUS, FAULT_STATUS and the alert list; theory_of_operation.md "Address-XOR",
 * "RRAM Read Integrity", the read-buffer state machine), run on the chip:
 *
 *   scenario 0  chip_sw_rram_sec_cm_mem_addr_infection (REQ_RRAM_08, sec_cm_mem_addr_infection):
 *               two RRAM words are written by the controller, then the bench swaps their two
 *               physical rows (data and ECC, so each row stays a valid codeword) by backdoor; a
 *               host read of the first must not return the relocated word silently ("A word
 *               relocated by a fault attack produces an ECC mismatch and is detected"): an error
 *               and an alert;
 *   scenario 1  chip_sw_rram_sec_cm_mem_readback (REQ_RRAM_11, sec_cm_mem_readback,
 *               sec_cm_phy_rd_buf_ctrl_integrity): the bench flips one bit of the macro's data
 *               on a shadow (verify) read; FAULT_STATUS.PHY_RD_INTG_ERR, fatal_err, and the
 *               read-buffer entry invalidated;
 *   scenario 2  chip_sw_rram_sec_cm_<cm> (REQ_RRAM_24, one run per countermeasure, `cm` below):
 *               the bench injects the fault the countermeasure guards against, the way the
 *               standard sec_cm tests do (a sparse FSM's state, one of a redundant counter's two
 *               copies, one copy of a duplicated arbiter) or forces the inconsistency a
 *               consistency check watches for; the countermeasure's STD_FAULT_STATUS or
 *               FAULT_STATUS bit and its fatal alert:
 *
 * | cm | countermeasure                   | where                          | bit, alert              |
 * |----|----------------------------------|--------------------------------|-------------------------|
 * | 0  | CTRL.FSM.SPARSE                  | lcmgr, otp, arb, rd, wr FSMs   | LCMGR/OTP/CTRL_FSM_ERR, |
 * |    |                                  |                                |   fatal_std_err         |
 * | 1  | PHY_WR.FSM.SPARSE                | rram_phy_wr FSM                | PHY_FSM_ERR, std        |
 * | 2  | CTR.REDUN                        | lcmgr, otp, rd/wr, phy_wr cnts | LCMGR/OTP/CTRL/PHY_CNT, |
 * |    |                                  |                                |   std                   |
 * | 3  | FIFO.CTR.REDUN                   | ctrl write FIFO; phy FIFOs     | CTRL/PHY_FIFO_ERR, std  |
 * | 4  | HOST_TL_LC_GATE.FSM.SPARSE       | the host TL-UL gate's FSM      | a fatal alert (no bit   |
 * | 5  | WR_TL_LC_GATE.FSM.SPARSE         | the write TL-UL gate's FSM     |   documented)           |
 * | 6  | PHY_ARBITER.CTRL.REDUN           | host and scramble arbiters     | PHY_ARB_ERR, std        |
 * | 7  | PHY_HOST_GRANT.CTRL.CONSISTENCY  | host grant on the info part.   | HOST_GNT_ERR, fatal_err |
 * | 8  | PHY_RSP.CTRL.CONSISTENCY         | a done for the other requester | SPURIOUS_DONE, fatal_err|
 * | 9  | PHY_RD_RSP.CTRL.INTEGRITY        | a read request with none asked | PHY_RD_CTRL_ERR, fatal  |
 *
 * Every one of these errors is in the local-escalation subset (MEM.CTRL.LOCAL_ESC: all of
 * STD_FAULT_STATUS and FAULT_STATUS), so RRAM stops answering, and this test runs from RRAM: the
 * checks after the fault are the bench's (through SBA and the alert handler, which stay
 * reachable), and a second boot checks the state a power-on reset leaves. The sequence is
 * env/seq_lib/chip_sw_eg2_rram_sec_cm_vseq.sv.
 *
 * Boot 0: the scratch region (pages 3584-3599, RD | WR | ECC, scrambled if `scramble`), its words
 * written and read back; `EG2 RRAM seccm ...` lines to the bench; then traffic (controller writes
 * and reads, host reads) for the bench's fault until RRAM stops. A boot 0 that finishes its
 * traffic prints `EG2 RRAM seccm traffic done` and waits for the bench's reset; for scenario 0 it
 * reports what its host read returned (`EG2 RRAM seccm swap ...`) and fails on a silent return.
 * Boot 1 (boot_phase 1): STD_FAULT_STATUS, FAULT_STATUS and ERR_CODE are 0, a controller write
 * and read and a host read of the scratch region work; the status sweep finds nothing set.
 *
 * Knobs:
 *   scenario     0 swap, 1 verify glitch, 2 fault injection
 *   cm           the countermeasure of scenario 2 (the table)
 *   scramble     the scratch region's scramble attribute
 *   boot_phase   written by the bench before its reset
 *   traffic      iterations of the traffic loop
 *   model_gaps   skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                    | Why                                       |
 * |------------------------------------------------|-------------------------------------------|
 * | the bench's fault, the wait for its reset;     | no bench in eg2sim; its rram_ctrl models |
 * |   boot 0 runs its traffic and passes           |   no countermeasure                      |
 *
 * Built with EG2_RRAM_SEC_CM_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define SEC_CM_PARAMS(X)      \
  X(scenario, 2, 0, 2)        \
  X(cm, 0, 0, 9)              \
  X(scramble, 0, 0, 1)        \
  X(boot_phase, 0, 0, 1)      \
  X(traffic, 4000, 1, 1000000) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(SEC_CM_PARAMS)

enum {
  kPolls = 200000,
  kFirstPage = kEg2RramScratchFirstPage,
  kPages = 16,
  kPageWords = kEg2RramPageBytes / 4,
  // The two words of the swap: the first RRAM word of the first and of the ninth page.
  kOffA = kFirstPage * kEg2RramPageBytes,
  kOffB = (kFirstPage + 8) * kEg2RramPageBytes,
};

static bool write_ok(const char *what, uint32_t off, const uint32_t *w, uint32_t n) {
  eg2_rram_result_t r;
  bool ok = eg2_chk_true(what, eg2_rram_write(kEg2RramPartData, off, w, n, &r));
  return eg2_chk_eq(what, 1, r.op_status) && ok;
}

static bool read_ok(const char *what, uint32_t off, uint32_t *w, uint32_t n) {
  eg2_rram_result_t r;
  bool ok = eg2_chk_true(what, eg2_rram_read(kEg2RramPartData, off, w, n, &r));
  return eg2_chk_eq(what, 1, r.op_status) && ok;
}

/** One page of the seed's pattern, written and read back by the controller and the host. */
static bool page_roundtrip(uint32_t page, uint32_t salt) {
  uint32_t w[kPageWords], r[kPageWords];
  for (uint32_t i = 0; i < kPageWords; ++i) {
    w[i] = eg2_rram_pattern(salt, i);
  }
  uint32_t off = page * kEg2RramPageBytes;
  bool ok = write_ok("page written", off, w, kPageWords);
  ok = read_ok("page read by the controller", off, r, kPageWords) && ok;
  uint32_t bad = 0;
  for (uint32_t i = 0; i < kPageWords; ++i) {
    bad += r[i] != w[i];
    bad += *eg2_rram_host(off + 4 * i) != w[i];
  }
  return eg2_chk_eq("page words read back (controller and host)", 0, bad) && ok;
}

/** Boot 1: what a power-on reset leaves after the fault. */
static bool boot1(void) {
  bool ok = true;
  ok = eg2_chk_eq("STD_FAULT_STATUS after the reset", 0,
                  eg2_rram_reg(RRAM_CTRL_STD_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("FAULT_STATUS after the reset", 0,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  ok = eg2_chk_eq("ERR_CODE after the reset", 0, eg2_rram_reg(RRAM_CTRL_ERR_CODE_REG_OFFSET)) &&
       ok;
  ok = page_roundtrip(kFirstPage + 1, 0x51) && ok;
  return ok;
}

/** Traffic for the bench's fault: controller writes and reads, host reads. */
static void traffic(uint32_t n) {
  uint32_t w[16], r[16];
  for (uint32_t it = 0; it < n; ++it) {
    uint32_t page = kFirstPage + 1 + eg2_rand_range(0, kPages - 2);
    uint32_t off = page * kEg2RramPageBytes + 64 * eg2_rand_range(0, 7);
    for (uint32_t i = 0; i < 16; ++i) {
      w[i] = eg2_rand();
    }
    eg2_rram_result_t res;
    if ((it & 3) == 0) {
      (void)eg2_rram_write(kEg2RramPartData, off, w, 16, &res);
    } else {
      (void)eg2_rram_read(kEg2RramPartData, off, r, 16, &res);
    }
    (void)*eg2_rram_host(off + 4 * eg2_rand_range(0, 15));
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  const uint32_t scenario = EG2_PARAM(scenario);
  const uint32_t phase = EG2_PARAM(boot_phase);
  eg2_chk_step("scenario", scenario);
  eg2_chk_step("countermeasure", EG2_PARAM(cm));
  eg2_chk_step("boot phase", phase);
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  eg2_rram_region_set(region, kFirstPage, kPages,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc |
                          (EG2_PARAM(scramble) ? kEg2RramAttrScr : 0));

  if (phase == 1) {
    return boot1() && eg2_chk_summary(8);
  }

  bool ok = page_roundtrip(kFirstPage + 2, 0x17);
  if (scenario == 0) {
    // Two RRAM words through the controller (which also invalidates any read-buffer entry of
    // their pages), never read by the host before the swap.
    uint32_t a[4], b[4];
    for (uint32_t i = 0; i < 4; ++i) {
      a[i] = eg2_rram_pattern(0xa, i);
      b[i] = eg2_rram_pattern(0xb, i);
    }
    ok = write_ok("word A written", kOffA, a, 4) && ok;
    ok = write_ok("word B written", kOffB, b, 4) && ok;
    if (gaps) {
      eg2_chk_skip("the bench's row swap");
    } else {
      LOG_INFO("EG2 RRAM seccm swap 0x%08x 0x%08x", kOffA, kOffB);
      eg2_chk_delay(200);
    }
    eg2_chk_trap_t t;
    uint32_t cause = eg2_chk_access(eg2_rram_host(kOffA), kEg2ChkLw, 0, &t);
    if (gaps) {
      ok = eg2_chk_eq("model: word A read by the host", a[0], t.value) && ok;
    } else if (cause == 0) {
      // Not detected as a data error: the relocated word came back.
      LOG_INFO("EG2 RRAM seccm swap silent 0x%08x", t.value);
      ok = eg2_chk_true("the relocated word was not returned silently", false) && ok;
    } else {
      LOG_INFO("EG2 RRAM seccm swap trapped 0x%08x 0x%08x", cause, t.mtval);
    }
  } else if (!gaps) {
    if (scenario == 1) {
      LOG_INFO("EG2 RRAM seccm verify arm");
    } else {
      LOG_INFO("EG2 RRAM seccm fi arm 0x%08x", EG2_PARAM(cm));
    }
    eg2_chk_delay(200);
  } else {
    eg2_chk_skip("the bench's fault");
  }
  traffic(EG2_PARAM(traffic));
  if (gaps) {
    return eg2_chk_summary(4) && ok;
  }
  // The fault stops RRAM, and with it this code: the bench resets the chip and the second boot
  // judges. Reaching here means RRAM still answers.
  LOG_INFO("EG2 RRAM seccm traffic done");
  eg2_chk_delay(2000000);
  ok = eg2_chk_true("the bench reset the chip after the fault", false) && ok;
  return eg2_chk_summary(4) && ok;
}
