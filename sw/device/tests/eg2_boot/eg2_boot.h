// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_BOOT_EG2_BOOT_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_BOOT_EG2_BOOT_H_

/**
 * @file
 * @brief What the secure-boot, security, life-cycle and ownership tests share (eg2_boot, eg2_sec).
 *
 * EG2 verification spec: REQ_BOOT, REQ_SEC. One source per test, built for RISC-V mode (OTTF) and
 * CHERIoT mode; nothing here names the mode except the capability probes.
 *
 *  - Checks: `eg2_boot_check` / `eg2_boot_expect` print one line per check, `EG2CHK ok <what>` or
 *    `EG2CHK FAIL <what> exp 0x... got 0x...`, so every `_inject_fail` build fails with a stated
 *    `EG2CHK FAIL <what>` line, and the count of checks made is printed at the end
 *    (`eg2_boot_summary`): a test that checked nothing fails (REQ_PBF_05).
 *  - Probes: one access or fetch that may fault, with its own trap handler (`mtvec` in RISC-V mode,
 *    256-byte aligned as Ibex's vectored `mtvec` requires; MTCC in CHERIoT mode). The result is
 *    `mcause`, or 0 without a trap. Interrupts must be off.
 *  - The retention SRAM as OpenTitan's silicon creator defines it (`retention_sram.h`): the boot
 *    services message, the boot log ROM_EXT leaves, and the owner area, where a test keeps its
 *    step across the resets it causes.
 *  - The RRAM controller's software path (CONTROL / ADDR / FIFOs, rram_ctrl programmer's guide)
 *    and the key manager's (keymgr_dpe programmer's guide), with the outcome as the documents
 *    define it: OP_STATUS / ERR_CODE.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/base/cheriot_mmio.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------------------------------------
// Checks

/** Compares `got` with `exp`; prints one `EG2CHK` line; returns whether they are equal. */
bool eg2_boot_check(const char *what, uint32_t exp, uint32_t got);

/** Checks a condition; prints one `EG2CHK` line with `got`. */
bool eg2_boot_expect(const char *what, bool cond, uint32_t got);

/** Prints `EG2 <what> 0x<value>` (information, not a check). */
void eg2_boot_note(const char *what, uint32_t value);

/** Prints the number of checks made and failed; false if any failed or none was made. */
bool eg2_boot_summary(void);

// ------------------------------------------------------------------------------------------------
// Probes

/** What a probe returns besides the trap cause. */
typedef struct eg2_boot_out {
  uint32_t value;  // the loaded value (0 if the load trapped)
  uint32_t mtval;  // mtval of the trap (0 without one)
} eg2_boot_out_t;

/** Probe operations. */
typedef enum eg2_boot_op {
  kEg2BootLoad = 0,   // lw
  kEg2BootStore = 1,  // sw
  kEg2BootFetch = 2,  // a jump to the address (RISC-V: jalr; CHERIoT: cjalr through `target`)
} eg2_boot_op_t;

/** Trap causes (RISC-V privileged spec; CHERIoT ISA for the CHERI exception). */
enum {
  kEg2CauseNone = 0,
  kEg2CauseFetchFault = 1,
  kEg2CauseIllegal = 2,
  kEg2CauseLoadFault = 5,
  kEg2CauseStoreFault = 7,
  kEg2CauseCheri = 0x1c,
};

/**
 * One load, store or fetch at `addr` that may fault. Returns 0, or `mcause` if it trapped.
 *
 * A fetch jumps to `addr`; the word there must be 0 (an illegal instruction in both modes), so a
 * fetch that the hardware allows traps with cause 2 at once instead of running code. In CHERIoT
 * mode the jump goes through a capability derived from PCC (the executable root the start-up code
 * left), with `addr` as its address; loads and stores through the window capability
 * (`OT_MMIO_ADDR_TO_PTR`). Interrupts must be disabled.
 */
uint32_t eg2_boot_probe(uint32_t addr, eg2_boot_op_t op, uint32_t wdata, eg2_boot_out_t *out);

#ifdef __CHERIOT__
/**
 * As `eg2_boot_probe`, through the capability `target` the caller derived: a load (op 0), store
 * (op 1) or jump (op 2).
 */
uint32_t eg2_boot_probe_cap(void *target, eg2_boot_op_t op, uint32_t wdata, eg2_boot_out_t *out);

/** A capability from the executable root (PCC) at `addr`, bounded to [base, base + len). */
void *eg2_boot_code_cap(uint32_t base, uint32_t len, uint32_t addr);
#endif

/** Disables interrupts (mstatus.MIE): the probes' handlers take none. */
static inline void eg2_boot_irq_off(void) { __asm__ volatile("csrci mstatus, 8"); }

/** Reads and writes a 32-bit word at a bus address (bounded to its window in CHERIoT mode). */
static inline uint32_t eg2_boot_rd32(uint32_t addr) {
  return *(volatile uint32_t *)OT_MMIO_ADDR_TO_PTR(addr);
}
static inline void eg2_boot_wr32(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *)OT_MMIO_ADDR_TO_PTR(addr) = value;
}

// ------------------------------------------------------------------------------------------------
// Retention SRAM, boot log, the test's own state across resets

/** The retention SRAM (a capability to its window in CHERIoT mode). */
volatile retention_sram_t *eg2_boot_retram(void);

/**
 * The test's state in the owner area of the retention SRAM, which ROM and ROM_EXT leave alone
 * except at power-on (retention_sram.h). `magic` marks it as the test's.
 */
typedef struct eg2_boot_state {
  uint32_t magic;
  uint32_t step;
  uint32_t boots;
  uint32_t fails;
  uint32_t data[60];
} eg2_boot_state_t;

enum { kEg2BootStateMagic = 0x45473242 };  // "EG2B"

/** The state; set to step 0 (and `magic`) on the first boot. Counts this boot. */
volatile eg2_boot_state_t *eg2_boot_state(void);

/** Requests a software reset from rstmgr (RESET_REQ = MuBi4True) and waits for it. */
void eg2_boot_sw_reset(void) __attribute__((noreturn));

/** rstmgr RESET_INFO. */
uint32_t eg2_boot_reset_info(void);

/**
 * Checks the boot log ROM_EXT left in the retention SRAM: its identifier, and the slots that
 * booted. Prints every field. Returns whether the checks held.
 */
bool eg2_boot_log_check(uint32_t exp_rom_ext_slot, uint32_t exp_bl0_slot);

/** The boot-log identifier (`BLOG`, boot_log.h) and the boot slots (boot_data.h). */
enum {
  kEg2BootLogId = 0x474f4c42,
  kEg2BootSlotA = 0x5f5f4141,
  kEg2BootSlotB = 0x42425f5f,
};

// ------------------------------------------------------------------------------------------------
// RRAM controller (software path)

/** A controller operation's outcome. */
typedef struct eg2_rram_res {
  uint32_t op_status;  // OP_STATUS: bit 0 done, bit 1 err
  uint32_t err_code;   // ERR_CODE: op_err, mp_err, rd_err, wr_err
} eg2_rram_res_t;

enum {
  kEg2RramOpRead = 0,
  kEg2RramOpWrite = 1,
  kEg2RramPartData = 0,
  kEg2RramPartInfo = 1,
  kEg2RramErrOp = 1u << 0,
  kEg2RramErrMp = 1u << 1,
};

/**
 * Reads (op 0) or writes (op 1) `nwords` bus words at byte address `addr` of `partition` through
 * the controller (rram_ctrl programmer's guide, "Issuing a Controller Read/Write"); `data` in or
 * out. Waits for OP_STATUS.done. Clears OP_STATUS and ERR_CODE after reading them. A write's
 * `nwords` must be a multiple of 4.
 */
eg2_rram_res_t eg2_rram_op(uint32_t op, uint32_t partition, uint32_t addr, uint32_t nwords,
                           uint32_t *data);

/** The rram_ctrl register at `offset`. */
uint32_t eg2_rram_reg(uint32_t offset);
void eg2_rram_reg_wr(uint32_t offset, uint32_t value);

/**
 * INFO_PAGE_CFG_<page>: enabled, with read, write, scrambling and ECC as given (MuBi4 each).
 * Returns the value written; the caller reads it back to see whether a lock kept it.
 */
uint32_t eg2_rram_info_cfg(uint32_t page, bool rd, bool wr, bool scramble, bool ecc);

// ------------------------------------------------------------------------------------------------
// Key manager (keymgr_dpe)

/** keymgr_dpe's working state (WORKING_STATE). */
uint32_t eg2_kmgr_state(void);

/** Slot `i`'s metadata: METADATA_LOW (max key version) and METADATA_HIGH (valid, stage, policy). */
uint32_t eg2_kmgr_meta_lo(uint32_t slot);
uint32_t eg2_kmgr_meta_hi(uint32_t slot);

/**
 * Starts operation `op` (CONTROL_SHADOWED.OPERATION) with the given selectors, waits, and
 * returns OP_STATUS (2 DONE_SUCCESS, 3 DONE_ERROR). ERR_CODE is returned in `*err` and cleared.
 */
uint32_t eg2_kmgr_op(uint32_t op, uint32_t dest, uint32_t src, uint32_t dst, bool sw_binding_only,
                     uint32_t *err);

/**
 * A DICE chain from the UDS (keymgr_dpe theory of operation, NumBootStages = 3):
 *  - the UDS into `slot`: the first advance in Reset, Load Root Key once Available;
 *  - advance in place with SW_BINDING `binding` (parent stage 0: SW_CDI_INPUT, device identifier,
 *    health state measurement, ROM digest, hardware revision seed) -> stage 1;
 *  - a versioned SW key from it with `salt`, version 0 -> `key1` (share0 ^ share1);
 *  - advance in place with `binding` (parent stage 1: SW_CDI_INPUT, creator seed) -> stage 2;
 *  - a versioned SW key with `salt` -> `key2`.
 * Returns whether every operation completed with DONE_SUCCESS (each is checked and printed).
 */
bool eg2_kmgr_chain(uint32_t slot, const uint32_t binding[8], const uint32_t salt[8],
                    uint32_t key1[8], uint32_t key2[8]);

/** Prints `EG2 <what> <8 words>`. */
void eg2_boot_note_key(const char *what, const uint32_t key[8]);

/** Whether two 8-word keys are equal. */
bool eg2_boot_key_eq(const uint32_t a[8], const volatile uint32_t b[8]);

/** Reads the 8 words of SW_SHARE0_OUTPUT and SW_SHARE1_OUTPUT (each read clears them). */
void eg2_kmgr_sw_output(uint32_t share0[8], uint32_t share1[8]);

/** keymgr_dpe's register at `offset`; a shadowed register is written twice. */
uint32_t eg2_kmgr_reg(uint32_t offset);
void eg2_kmgr_reg_wr(uint32_t offset, uint32_t value);
void eg2_kmgr_reg_wr_shadowed(uint32_t offset, uint32_t value);

enum {
  kEg2KmgrReset = 0,
  kEg2KmgrAvailable = 1,
  kEg2KmgrDisabled = 2,
  kEg2KmgrInvalid = 3,
  kEg2KmgrOpAdvance = 0,
  kEg2KmgrOpErase = 1,
  kEg2KmgrOpGenSw = 2,
  kEg2KmgrOpGenHw = 3,
  kEg2KmgrOpDisable = 4,
  kEg2KmgrOpLoadRoot = 5,
  kEg2KmgrDone = 2,
  kEg2KmgrDoneError = 3,
  kEg2KmgrSlots = 4,
};

// ------------------------------------------------------------------------------------------------
// OTP direct access interface (otp_ctrl programmer's guide, "Direct Access Interface")

enum {
  kEg2OtpDaiRead = 1u << 0,
  kEg2OtpDaiWrite = 1u << 1,
  kEg2OtpDaiDigest = 1u << 2,
  kEg2OtpErrNone = 0,
  kEg2OtpErrAccess = 5,
};

/**
 * One DAI command (`cmd`) at OTP byte address `addr`, with write data `wdata[2]` (a write) or
 * read data into `rdata[2]` (a read; zeroed first). Waits for DAI_IDLE. Returns the DAI's
 * ERR_CODE entry (0 NO_ERROR, 5 ACCESS_ERROR, ...); `*regwen_locked` (if given) tells whether
 * DIRECT_ACCESS_REGWEN was 0, in which case the command could not be issued and 0xff is returned.
 */
uint32_t eg2_otp_dai(uint32_t cmd, uint32_t addr, const uint32_t wdata[2], uint32_t rdata[2]);

/** The two digest words of a partition, from its DIGEST_0/1 registers. */
uint64_t eg2_otp_digest(uint32_t digest0_offset);

/** Declares the DAI error interrupt as expected and clears the DAI's error code with a good read. */
void eg2_otp_dai_clear_error(void);

// ------------------------------------------------------------------------------------------------
// Life cycle

/** lc_ctrl LC_STATE (the 5-bit state repeated six times) and LC_TRANSITION_CNT. */
uint32_t eg2_lc_state(void);
uint32_t eg2_lc_count(void);

/** The decoded life-cycle state values (lc_ctrl.hjson LC_STATE). */
enum {
  kEg2LcRaw = 0,
  kEg2LcTestUnlocked0 = 1,
  kEg2LcTestUnlocked7 = 15,
  kEg2LcDev = 16,
  kEg2LcProd = 17,
  kEg2LcProdEnd = 18,
  kEg2LcRma = 19,
  kEg2LcScrap = 20,
  kEg2LcInvalid = 23,
};

/** The 5-bit state of LC_STATE (all six copies must agree, else kEg2LcInvalid). */
uint32_t eg2_lc_state_decoded(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_BOOT_EG2_BOOT_H_
