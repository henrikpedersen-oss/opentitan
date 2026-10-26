// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_CAP_MODEL_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_CAP_MODEL_H_

/**
 * @file
 * @brief The CHERIoT capability encoding and instruction semantics, as a model inside the tests.
 *
 * The tests of the CHERIoT suite (EG2 verification spec, "CHERIoT Test Suite") compute every
 * expected tag, bound, permission set and trap cause from the architecture, never from what the
 * hardware returned. This is that architecture, written from CHERIoT-Sail (src/cheri_cap_common.sail,
 * cheri_insts.sail, cheri_addr_checks.sail, cheri_regs.sail), the executable form of the CHERIoT ISA
 * specification:
 *   - a register value is the 64-bit in-memory form and the tag (`eg2_cap_t`); every instruction
 *     result is normalised through the encoding, as Sail's `wC` does, so the model holds exactly
 *     what `CGetHigh`, `CGetAddr` and `CGetTag` return;
 *   - bounds decode (`getCapBoundsBits`), `setCapBounds`, `setCapBoundsRoundDown`, the
 *     representability rule (an address change keeps the tag only if the decoded bounds stay the
 *     same), the compressed permission formats, sealing and the data-access checks in Sail's order.
 *
 * The model is plain integer C, so it builds in both execution modes; only the CHERIoT build runs
 * it against the hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Architectural permission bits, as `CGetPerm` returns them (Sail `getCapPerms`). */
enum {
  kEg2PermGl = 1u << 0,   // Global
  kEg2PermLg = 1u << 1,   // Load global
  kEg2PermSd = 1u << 2,   // Store
  kEg2PermLm = 1u << 3,   // Load mutable
  kEg2PermSl = 1u << 4,   // Store local
  kEg2PermLd = 1u << 5,   // Load
  kEg2PermMc = 1u << 6,   // Load and store capabilities
  kEg2PermSr = 1u << 7,   // Access system registers
  kEg2PermEx = 1u << 8,   // Execute
  kEg2PermUs = 1u << 9,   // Unseal
  kEg2PermSe = 1u << 10,  // Seal
  kEg2PermU0 = 1u << 11,  // User permission 0
  kEg2PermAll = 0xfffu,
};

/** The capability exception causes in `mtval[4:0]` (Sail `CapExCode`; the eight Sail raises). */
enum {
  kEg2CauseBounds = 0x01,
  kEg2CauseTag = 0x02,
  kEg2CauseSeal = 0x03,
  kEg2CauseEx = 0x11,
  kEg2CauseLd = 0x12,
  kEg2CauseSd = 0x13,
  kEg2CauseSc = 0x15,
  kEg2CauseSr = 0x18,
};

/** `mcause` values. */
enum {
  kEg2McauseFetchFault = 1,
  kEg2McauseIllegal = 2,
  kEg2McauseBreakpoint = 3,
  kEg2McauseLoadMisaligned = 4,
  kEg2McauseLoadFault = 5,
  kEg2McauseStoreMisaligned = 6,
  kEg2McauseStoreFault = 7,
  kEg2McauseEcallU = 8,
  kEg2McauseEcallM = 11,
  kEg2McauseCheri = 28,
};

/** Object types (Sail `otype_sentry*`). */
enum {
  kEg2OtypeUnsealed = 0,
  kEg2OtypeSentry = 1,     // forward, interrupts inherited
  kEg2OtypeSentryId = 2,   // forward, interrupts disabled
  kEg2OtypeSentryIe = 3,   // forward, interrupts enabled
  kEg2OtypeSentryBid = 4,  // backward, interrupts disabled
  kEg2OtypeSentryBie = 5,  // backward, interrupts enabled
};

/** The `mtval` of a capability exception: register index (bit 5 set for a special one), cause. */
static inline uint32_t eg2_cap_mtval(uint32_t reg6, uint32_t cause) { return reg6 << 5 | cause; }
/** The register index of PCC in `mtval` (Sail `handle_cheri_pcc_exception`). */
enum { kEg2RegPcc = 0x20 };

/** A capability register's value: the high word of the in-memory form, the address, the tag. */
typedef struct eg2_cap {
  uint32_t hi;
  uint32_t addr;
  bool tag;
} eg2_cap_t;

/** The decoded form (Sail `Capability`): permissions expanded, E expanded, otype expanded. */
typedef struct eg2_capx {
  bool tag;
  uint32_t perms;     // architectural, kEg2Perm*
  uint32_t reserved;  // 1 bit
  uint32_t e;         // 0-14 or 24
  uint32_t b, t;      // 9-bit mantissas
  uint32_t otype;     // 4 bits
  uint32_t addr;
} eg2_capx_t;

eg2_capx_t eg2_cap_decode(eg2_cap_t c);
eg2_cap_t eg2_cap_encode(const eg2_capx_t *x);

/** Decoded bounds; `top` is 33 bits (2^32 for the roots). */
uint32_t eg2_cap_base(eg2_cap_t c);
uint64_t eg2_cap_top(eg2_cap_t c);
/** What `CGetLen` and `CGetTop` return (saturated at 2^32 - 1). */
uint32_t eg2_cap_len32(eg2_cap_t c);
uint32_t eg2_cap_top32(eg2_cap_t c);
/** What `CGetPerm` and `CGetType` return. */
uint32_t eg2_cap_perms(eg2_cap_t c);
uint32_t eg2_cap_otype(eg2_cap_t c);
uint32_t eg2_cap_exp(eg2_cap_t c);
bool eg2_cap_is_sealed(eg2_cap_t c);
/** Sail `inCapBounds`: base <= addr and addr + size <= top. */
bool eg2_cap_in_bounds(eg2_cap_t c, uint32_t addr, uint32_t size);
/** The first address above the representable window of `c`: base + 2^(E+9) (or 2^32 for E 24). */
uint64_t eg2_cap_repr_limit(eg2_cap_t c);

/** The roots (Sail `root_cap_mem`, `root_cap_exe`, `root_cap_seal`) at address `addr`. */
eg2_cap_t eg2_cap_root_mem(uint32_t addr);
eg2_cap_t eg2_cap_root_exe(uint32_t addr);
eg2_cap_t eg2_cap_root_seal(uint32_t addr);
/** The null capability with address `addr`: what an integer write leaves in a register. */
eg2_cap_t eg2_cap_null(uint32_t addr);

/** The bounds-setting instructions. */
typedef enum eg2_bounds_kind {
  kEg2BoundsInexact = 0,  // CSetBounds, CSetBoundsImm
  kEg2BoundsExact = 1,    // CSetBoundsExact
  kEg2BoundsRoundDown = 2,  // CSetBoundsRoundDown
} eg2_bounds_kind_t;

/** Each returns the result register as the instruction writes it. */
eg2_cap_t eg2_cap_set_addr(eg2_cap_t c, uint32_t addr);      // CSetAddr
eg2_cap_t eg2_cap_inc_addr(eg2_cap_t c, uint32_t delta);     // CIncAddr, CIncAddrImm
eg2_cap_t eg2_cap_set_bounds(eg2_cap_t c, uint32_t len, eg2_bounds_kind_t kind);
eg2_cap_t eg2_cap_and_perm(eg2_cap_t c, uint32_t mask);      // CAndPerm
eg2_cap_t eg2_cap_seal(eg2_cap_t c, eg2_cap_t auth);         // CSeal
eg2_cap_t eg2_cap_unseal(eg2_cap_t c, eg2_cap_t auth);       // CUnseal
eg2_cap_t eg2_cap_set_high(eg2_cap_t c, uint32_t hi);        // CSetHigh
eg2_cap_t eg2_cap_clear_tag(eg2_cap_t c);                    // CClearTag
uint32_t eg2_cap_test_subset(eg2_cap_t a, eg2_cap_t b);      // CTestSubset a, b
uint32_t eg2_cap_equal_exact(eg2_cap_t a, eg2_cap_t b);      // CSetEqualExact
uint32_t eg2_cap_crrl(uint32_t len);                         // CRRL
uint32_t eg2_cap_cram(uint32_t len);                         // CRAM
/** AUIPCC executed at `pc` under `pcc` (unsealed). */
eg2_cap_t eg2_cap_auipcc(eg2_cap_t pcc, uint32_t pc, uint32_t imm20);
/** AUICGP with CGP = `cgp`. */
eg2_cap_t eg2_cap_auicgp(eg2_cap_t cgp, uint32_t imm20);
/** The link capability of CJAL/CJALR at PCC `pcc`; `to_ra` seals it as a backward sentry. */
eg2_cap_t eg2_cap_link(eg2_cap_t pcc, uint32_t next_pc, bool to_ra, bool mie);
/** The legalisation of a CSpecialRW write to MTCC (`scr` 28) or MEPCC (31). */
eg2_cap_t eg2_cap_scr_legalise(uint32_t scr, eg2_cap_t c);

/** The access an instruction makes. */
typedef enum eg2_access {
  kEg2AccessLoad = 0,
  kEg2AccessStore = 1,
  kEg2AccessLoadCap = 2,
  kEg2AccessStoreCap = 3,
} eg2_access_t;

/**
 * The capability checks of a data access through `auth`, in Sail's order: 0 if they pass, else the
 * capability cause. A capability access that passes them but is misaligned returns -1 (mcause 4
 * for CLC, 6 for CSC). `stored_tag` is the tag of the capability a CSC stores.
 */
int eg2_cap_check_access(eg2_cap_t auth, uint32_t addr, uint32_t width, eg2_access_t access,
                         bool stored_tag);

/** The capability CLC writes, from the memory's bits and tag, through `auth` (Sail LoadCapImm). */
eg2_cap_t eg2_cap_clc_result(eg2_cap_t auth, eg2_cap_t mem);
/** The capability CSC writes to memory through `auth` (Sail StoreCapImm). */
eg2_cap_t eg2_cap_csc_stored(eg2_cap_t auth, eg2_cap_t value);

/** The CJALR checks (Sail CJALR): 0, or the capability cause raised for `cs1`. */
int eg2_cap_check_cjalr(eg2_cap_t target, uint32_t imm, bool cd_is_ra, bool cd_is_zero,
                        bool cs1_is_ra);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_CAP_MODEL_H_
