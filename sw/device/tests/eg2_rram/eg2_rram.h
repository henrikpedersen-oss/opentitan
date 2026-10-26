// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_RRAM_EG2_RRAM_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_RRAM_EG2_RRAM_H_

/**
 * @file
 * @brief What the RRAM tests share (eg2_top_verification spec REQ_RRAM, "RRAM controller in
 * RISC-V mode"; hw/ip/rram_ctrl: README "Address Map", doc/programmers_guide.md).
 *
 * Registers through the device table (`kEg2DevRramCtrlCore`: a bounded capability in CHERIoT
 * mode), the host window through `eg2_chk_ptr`. Every expected value comes from the documents:
 * the layout (512-byte pages of 32 RRAM words, 4096 data pages, the OTP integrity page 4090 and
 * the OTP pages 4091-4095, 8 info pages), the MuBi4 attributes of MP_REGION_CFG, DEFAULT_REGION
 * and INFO_PAGE_CFG, and the controller operations (16-byte aligned writes of whole RRAM words,
 * at most 1024 bus words).
 *
 * Scratch pages: the tests write only pages kEg2RramScratchFirstPage..LastPage (the top of the
 * data partition below the OTP region), far above the test image at the bottom of RRAM and the
 * 1 MiB slot A of the secure-boot images.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  kEg2RramHostBase = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
  kEg2RramHostSize = TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES,
  kEg2RramPrimBase = TOP_EARLGREY_RRAM_MACRO_PRIM_BASE_ADDR,
  kEg2RramPageBytes = 512,
  kEg2RramWordBytes = 16,  // one RRAM word: four bus words
  kEg2RramDataPages = 4096,
  kEg2RramInfoPages = 8,
  kEg2RramOtpIntgPage = 4090,
  kEg2RramOtpFirstPage = 4091,
  kEg2RramOtpStartAddr = 0x1FF600,  // README "Address Map": OtpStartAddr
  kEg2RramOtpIntgAddr = 0x1FF400,   // OtpIntgStartAddr
  kEg2RramScratchFirstPage = 3584,
  kEg2RramScratchLastPage = 4089,
  kEg2RramMaxOpWords = 1024,
  kEg2RramFifoDepth = 16,
  kEg2RramExecMagic = 0xa26a38f7,  // programmers_guide.md "Code Execution from RRAM"
  kEg2RramRegions = 10,
  kEg2RramMubiTrue = 0x6,
  kEg2RramMubiFalse = 0x9,
};

/** Region attributes (MP_REGION_CFG, DEFAULT_REGION, INFO_PAGE_CFG): each MuBi4True when set. */
enum {
  kEg2RramAttrRd = 1u << 0,
  kEg2RramAttrWr = 1u << 1,
  kEg2RramAttrScr = 1u << 2,
  kEg2RramAttrEcc = 1u << 3,
  kEg2RramAttrAll = 0xf,
};

/** Controller operations (CONTROL.OP) and partitions (CONTROL.PARTITION). */
enum {
  kEg2RramOpRead = 0,
  kEg2RramOpWrite = 1,
  kEg2RramOpRewrite = 2,
  kEg2RramPartData = 0,
  kEg2RramPartInfo = 1,
};

/** rram_ctrl.core registers. */
uint32_t eg2_rram_reg(uint32_t offset);
void eg2_rram_reg_wr(uint32_t offset, uint32_t value);

/** The host window: a pointer to byte offset `offset` of the data partition. */
volatile uint32_t *eg2_rram_host(uint32_t offset);
/** The host address of `page`, `word` (bus word). */
static inline uint32_t eg2_rram_addr(uint32_t page, uint32_t word) {
  return page * kEg2RramPageBytes + word * 4;
}

/**
 * Starts the controller's initialisation if it has not run (INIT.VAL) and waits for
 * STATUS.init_done and keys_valid. Returns false after `polls` reads.
 */
bool eg2_rram_wait_init(uint32_t polls);

/** MP_REGION_CFG_<i> with EN = MuBi4True and `attrs`. */
uint32_t eg2_rram_region_cfg(uint32_t attrs);
/** DEFAULT_REGION with `attrs`. */
uint32_t eg2_rram_default_cfg(uint32_t attrs);
/** A region's attributes back from its MP_REGION_CFG / DEFAULT_REGION / INFO_PAGE_CFG value. */
uint32_t eg2_rram_cfg_attrs(uint32_t cfg, bool is_default);

/**
 * The highest-numbered data region that is disabled and whose REGION_CFG_REGWEN is still open, or
 * kEg2RramRegions if none: the test's own region, never one the boot stages configured.
 */
uint32_t eg2_rram_free_region(void);
/** Configures data region `idx` over [base_page, base_page + pages) with `attrs`, enabled. */
void eg2_rram_region_set(uint32_t idx, uint32_t base_page, uint32_t pages, uint32_t attrs);
/** Disables data region `idx`. */
void eg2_rram_region_off(uint32_t idx);
/** INFO_PAGE_CFG_<page> with EN = MuBi4True and `attrs`. */
void eg2_rram_info_set(uint32_t page, uint32_t attrs);

/** What a controller operation ended with. */
typedef struct eg2_rram_result {
  uint32_t op_status;  // OP_STATUS (done, err)
  uint32_t err_code;   // ERR_CODE, read before it is cleared
  uint32_t err_addr;   // ERR_ADDR
  uint32_t words;      // words pushed or popped
} eg2_rram_result_t;

/** Starts an operation (CONTROL with START), after clearing OP_STATUS. Does not wait. */
void eg2_rram_start(uint32_t op, uint32_t part, uint32_t byte_addr, uint32_t words);

/** Pushes one word into wr_fifo when it has room. False after `polls` reads. */
bool eg2_rram_push(uint32_t word, uint32_t polls);
/** Pops one word from rd_fifo when it holds one. False after `polls` reads. */
bool eg2_rram_pop(uint32_t *word, uint32_t polls);

/**
 * Waits for OP_STATUS.done, then records OP_STATUS, ERR_CODE and ERR_ADDR, clears ERR_CODE (rw1c)
 * and OP_STATUS, and the op_done interrupt. False if it never finished.
 */
bool eg2_rram_wait_done(eg2_rram_result_t *r, uint32_t polls);

/** A whole write: start, push `words` words, wait. False on a timeout (not on an error). */
bool eg2_rram_write(uint32_t part, uint32_t byte_addr, const uint32_t *data, uint32_t words,
                    eg2_rram_result_t *r);
/** A whole read into `data`. */
bool eg2_rram_read(uint32_t part, uint32_t byte_addr, uint32_t *data, uint32_t words,
                   eg2_rram_result_t *r);
/** A Rewrite of the RRAM word at `byte_addr`. */
bool eg2_rram_rewrite(uint32_t part, uint32_t byte_addr, eg2_rram_result_t *r);

/** EXEC: MuBi-like magic value 0xa26a38f7 enables fetches. */
static inline void eg2_rram_exec_set(uint32_t value) {
  eg2_rram_reg_wr(RRAM_CTRL_EXEC_REG_OFFSET, value);
}

/** Clears every RRAM interrupt the tests raise (INTR_STATE, the event-type ones). */
void eg2_rram_clear_intrs(void);

// ---- Code in RRAM (the host-path tests; added by the group-A tests' author) ----

/** RV32 encodings the tests place in RRAM: the same words in both modes. */
static inline uint32_t eg2_rram_insn_addi_a0(int32_t imm) {
  return ((uint32_t)imm & 0xfff) << 20 | 0x00050513u;  // addi a0, a0, imm
}
enum {
  kEg2RramInsnCJrRa = 0x8082,  // c.jr ra (c.cjr cra in CHERIoT mode): the return
  kEg2RramInsnCNop = 0x0001,   // c.nop
  kEg2RramInsnCUnimp = 0x0000, // c.unimp: an illegal instruction where no code belongs
};

/**
 * Fills `nwords` words with c.unimp and places the 32-bit instructions `insns[0..n)` from byte
 * offset `off` (even), then c.jr ra. Returns the byte offset after the c.jr; 0 if they do not
 * fit.
 */
uint32_t eg2_rram_code_block(uint32_t *words, uint32_t nwords, uint32_t off, const uint32_t *insns,
                             uint32_t n);

/** Turns the instruction cache on or off (cpuctrlsts.icache_enable); returns the old setting. */
bool eg2_rram_icache_set(bool on);

/** A word of a seed-determined pattern: the same in every boot of a run. */
static inline uint32_t eg2_rram_pattern(uint32_t salt, uint32_t i) {
  uint32_t x = eg2_seed() ^ salt ^ (i * 0x9e3779b9u);
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_RRAM_EG2_RRAM_H_
