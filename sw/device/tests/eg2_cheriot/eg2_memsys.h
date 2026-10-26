// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_MEMSYS_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_MEMSYS_H_

/**
 * @file
 * @brief `eg2_memsys`: what the memory-subsystem and core tests of the CHERIoT suite share (EG2
 * spec "Memory subsystem in CHERIoT mode", "Revocation", "Mode switch and handover").
 *
 *  - The meta SRAM map (cheriot_mem_sys doc/theory_of_operation.md "Meta SRAM Address Map") and
 *    the tagged regions.
 *  - Capability store and load at an address, with the tag the load returned (CHERIoT mode).
 *  - The revocation bitmap window and the revocation engine (TBRE) registers.
 *  - The RRAM controller's program path (rram_ctrl doc/programmers_guide.md).
 *  - The SRAM controllers' INIT and key renewal (sram_ctrl doc/theory_of_operation.md).
 *  - A boot record in retention SRAM for tests that cross a reset, the reset reason the test ROM
 *    left there, and the resets software can cause (software, watchdog bite, escalation,
 *    deep-sleep wake-up).
 *  - Escalation through alert class A to a reset, and rstmgr's crash dump.
 *  - Bench steps: what the chip bench does for the test (an SBA access, a fault), requested on the
 *    console as `EG2 BENCH <n> <op> <a> <b>` and acknowledged by the bench writing <n> into
 *    `kEg2BenchAck` (chip_sw_eg2_rand_vseq). Without a bench (the SystemC model, `model_gaps`) a
 *    step is announced and skipped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------ The map ------------------------------------------

enum {
  // The revocable heap: main and sec SRAM, one bitmap bit per 8 bytes (MainSramBaseAddr,
  // MainSramTopAddr of cheriot_mem_sys.hjson).
  kEg2MsHeapBase = 0x10000000u,
  kEg2MsHeapTop = 0x10030000u,
  kEg2MsNvmBase = 0x30000000u,
  kEg2MsNvmTop = 0x30200000u,
  // The end of the RRAM the host window reaches: data pages 0-4089. The OTP integrity page (4090,
  // 0x1FF400) and the OTP pages (4091-4095) are reachable through the OTP hardware interface only
  // (rram_ctrl README "Address Map"), so a sweep or read beyond here gets error responses.
  kEg2MsNvmHostTop = 0x301FF400u,
  // The meta SRAM, at the address of the revocation bitmap window (theory_of_operation.md).
  kEg2MsMetaBase = 0x11000000u,
  kEg2MsMetaSize = 0x9800u,
  kEg2MsRevbmSize = 0xc00u,
  kEg2MsNvmTagOffset = 0x0c00u,
  kEg2MsSramTagOffset = 0x8c00u,
  // Untagged memories (REQ_CMS_10): ROM, retention SRAM, debug memory.
  kEg2MsRomBase = 0x00040000u,
  kEg2MsRetBase = 0x40600000u,
  kEg2MsDbgMemBase = 0x00010000u,
  kEg2MsDbgMemSize = 0x1000u,
  kEg2MsGranule = 8,
  // The RRAM area the tests program (the image runs from the start of RRAM): from 1 MiB on, 64
  // pages of 512 bytes.
  kEg2MsRramArea = 0x30100000u,
  kEg2MsRramAreaSize = 0x8000u,
  kEg2MsRramPage = 512,
  // tbre_done's PLIC source.
  kEg2MsTbreIrq = 189,
};

/** The meta SRAM byte offset of the tag of the granule at `addr`, 0xffffffff if untagged. */
uint32_t eg2_ms_tag_offset(uint32_t addr);

/** True if `addr` lies in main SRAM, sec SRAM or RRAM, the memories with tags. */
bool eg2_ms_tagged(uint32_t addr);

/** The revocation bitmap's bit for the granule at `addr` (heap only): word index and bit. */
bool eg2_ms_revbm_bit(uint32_t addr, uint32_t *word, uint32_t *bit);

// --------------------------------- Capabilities (CHERIoT) -------------------------------------

#ifdef __CHERIOT__

/**
 * A data capability from the memory root over [base, base + len), every memory permission (LD,
 * SD, MC, SL, LG, LM, GL), as CSetBounds rounds it.
 */
void *eg2_ms_data_cap(uint32_t base, uint32_t len);

/** An authority over [addr, addr + 8) with LD, SD and MC, for one capability access. */
void *eg2_ms_auth(uint32_t addr);

/** CSC `cap` to `addr`. */
void eg2_ms_csc(uint32_t addr, void *cap);

/** CLC from `addr`. */
void *eg2_ms_clc(uint32_t addr);

/** CLC from `addr`, CGetTag of the result. */
uint32_t eg2_ms_tag(uint32_t addr);

/** CGetTag, CGetBase, CGetAddr of a capability register. */
uint32_t eg2_ms_cap_tag(void *c);
uint32_t eg2_ms_cap_base(void *c);
uint32_t eg2_ms_cap_addr(void *c);
uint32_t eg2_ms_cap_high(void *c);

/** A sealing capability (permit-seal from the sealing root): never revoked (theory_of_operation). */
void *eg2_ms_sealing_cap(void);

/**
 * The capability stored at a slot, as a test plans it: tagged, based at `base` (a granule of the
 * heap or elsewhere), length `len`; sealed with object type 9 when `sealed`.
 */
void *eg2_ms_object_cap(uint32_t base, uint32_t len, bool sealed);

/** The revocation bitmap's word `i` through the revbm window (CHERIoT mode only). */
uint32_t eg2_ms_revbm_read(uint32_t i);
void eg2_ms_revbm_write(uint32_t i, uint32_t value);
/** Sets (or clears) the revocation bit of the granule at `addr`. */
void eg2_ms_revbm_mark(uint32_t addr, bool revoked);
/** Clears the whole bitmap. */
void eg2_ms_revbm_clear(void);

#endif  // __CHERIOT__

/** Writes a 32-bit word / reads one at an address (a root-derived capability in CHERIoT mode). */
void eg2_ms_write32(uint32_t addr, uint32_t value);
uint32_t eg2_ms_read32(uint32_t addr);

// ------------------------------------- Revocation engine --------------------------------------

typedef struct eg2_ms_sweep {
  uint32_t epoch_before;
  uint32_t epoch_after;
  uint32_t status;     // TBRE_STATUS when the sweep was over
  uint32_t polls;      // reads until it was
  bool started;        // the start was taken (no start_err)
  bool done;           // busy cleared within the poll limit
} eg2_ms_sweep_t;

/** Starts a sweep; `out->started` is false if the engine refused it (start_err). */
void eg2_ms_tbre_start(uint32_t base, uint32_t n_caps, eg2_ms_sweep_t *out);
/** Polls TBRE_STATUS.busy until clear, at most `max_polls` reads. */
void eg2_ms_tbre_wait(eg2_ms_sweep_t *out, uint32_t max_polls);
uint32_t eg2_ms_tbre_epoch(void);
uint32_t eg2_ms_tbre_status(void);
/** Clears TBRE_STATUS's sticky errors (rw1c) and tbre_done. */
void eg2_ms_tbre_clear(void);

// -------------------------------------- PLIC and timers ---------------------------------------

/** Enables PLIC source `id` (priority 1, threshold 0) for the core's external interrupt. */
void eg2_ms_plic_enable(uint32_t id, bool enable);
/** The PLIC's pending bit of `id`. */
bool eg2_ms_plic_pending(uint32_t id);
/** Claims; returns the source ID (0 if none). */
uint32_t eg2_ms_plic_claim(void);
void eg2_ms_plic_complete(uint32_t id);

// ------------------------------------------ RRAM ---------------------------------------------

/**
 * Lets the controller program and read the data partition (DEFAULT_REGION: read, write, ECC and
 * scrambling as the test ROM left them) and makes sure its initialisation is done.
 */
bool eg2_ms_rram_setup(void);
/**
 * Programs `n` words (a multiple of 4) at host address `addr` (16-byte aligned) through the
 * controller. Returns false on OP_STATUS.err; `err_code` gets ERR_CODE.
 */
bool eg2_ms_rram_program(uint32_t addr, const uint32_t *words, uint32_t n, uint32_t *err_code);

// ------------------------------------------ SRAM ---------------------------------------------

/**
 * sram_ctrl CTRL with RENEW_SCR_KEY and/or INIT, then waits for INIT_DONE (if INIT) or
 * SCR_KEY_VALID. Returns false if the controller reported INIT_ERROR or did not finish.
 */
bool eg2_ms_sram_ctrl(eg2_dev_id_t regs, bool renew_key, bool init);

// ----------------------------------- Boot record and resets -----------------------------------

/** The test ROM's reset reason of this boot (retention SRAM creator.reset_reasons). */
uint32_t eg2_ms_reset_reason(void);

enum {
  kEg2MsResetPor = 1u << 0,
  kEg2MsResetLowPowerExit = 1u << 1,
  kEg2MsResetSw = 1u << 2,
  kEg2MsResetWatchdog = 1u << 4,
  kEg2MsResetEscalation = 1u << 6,
  kEg2MsResetNdm = 1u << 7,
};

/** A test's record across resets: in retention SRAM, the 256 bytes below the test KiB. */
typedef struct eg2_ms_rec {
  uint32_t magic;
  uint32_t boot;      // boots so far (1 on the first)
  uint32_t phase;     // the test's own
  uint32_t data[61];  // the test's own
} eg2_ms_rec_t;

enum {
  kEg2MsRecAddr = 0x40601b00u,
  kEg2MsRecMagic = 0xe62c4ec0u,
};

/**
 * The record. On a power-on reset (or a record without its magic) it is zeroed and boot = 1;
 * otherwise boot counts up. Call once per boot.
 */
eg2_ms_rec_t *eg2_ms_rec_boot(void);
eg2_ms_rec_t *eg2_ms_rec(void);

/** The resets software can cause. */
typedef enum eg2_ms_reset_kind {
  kEg2MsResetKindPor = 0,       // the bench (power-on)
  kEg2MsResetKindSw = 1,        // rstmgr RESET_REQ
  kEg2MsResetKindNdm = 2,       // the bench (rv_dm ndmreset over JTAG)
  kEg2MsResetKindEscalation = 3,  // an alert in class A, escalation signal 3
  kEg2MsResetKindDeepSleep = 4,   // pwrmgr deep sleep, aon_timer wake-up
  kEg2MsResetKindWatchdog = 5,    // aon_timer watchdog bite
  kEg2MsResetKindCount = 6,
} eg2_ms_reset_kind_t;

const char *eg2_ms_reset_name(eg2_ms_reset_kind_t kind);

/** The reset reason bit each kind leaves. */
uint32_t eg2_ms_reset_expected(eg2_ms_reset_kind_t kind);

/**
 * Causes the reset; returns only if it did not happen (the caller fails the test). POR and NDM
 * are bench steps; without a bench (`bench` false) they are replaced by a software reset.
 */
void eg2_ms_reset(eg2_ms_reset_kind_t kind, bool bench);

/**
 * Class A: `alert` escalates on its first occurrence, straight to escalation signal 3 (the
 * escalation reset), with the crash dump latched in phase 0 and captured by rstmgr.
 */
void eg2_ms_escalation_arm(size_t alert);

/** rstmgr's alert crash dump: the cause bit of `alert`. */
bool eg2_ms_crashdump_cause(size_t alert);

// ------------------------------------------ Bench --------------------------------------------

/**
 * A bench step: prints `EG2 BENCH <n> <op> <a> <b>` and waits until the bench has written n into
 * kEg2BenchAck (polls bounded by `max_polls`). With `bench` false only the line is printed
 * (`EG2 BENCH <n> <op> ... skipped (no bench)`). Returns false if the bench did not answer.
 */
bool eg2_ms_bench(const char *op, uint32_t a, uint32_t b, bool bench);

/** What the bench answered in the step's second word (kEg2BenchAck[1]). */
uint32_t eg2_ms_bench_result(void);

/** Writes the DV status word (rv_core_ibex DV_SIM_WINDOW[0]). */
void eg2_ms_dv_status(uint32_t status);

// ------------------------------------------ Misc ---------------------------------------------

/** Busy-waits about `n` loop iterations. */
void eg2_ms_spin(uint32_t n);

/** Prints `EG2 <what>: <v>`. */
void eg2_ms_note(const char *what, uint32_t v);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_MEMSYS_H_
