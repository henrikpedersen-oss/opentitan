// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Map clang's CHERI permission macros onto CHERIoT's permission bits.
//
// The CHERIoT compiler predefines __CHERI_CAP_PERMISSION_*__ with the generic (128-bit CHERI)
// numbering, which is not CHERIoT's. __builtin_cheri_perms_and/get use the architectural bits, so
// a mask built from the macros selects the wrong permissions:
//
//   macro                     clang value   bit   what that bit is on CHERIoT
//   PERMIT_EXECUTE                 2         1    LoadGlobal
//   PERMIT_LOAD                    4         2    Store
//   PERMIT_STORE                   8         3    LoadMutable
//   PERMIT_LOAD_CAPABILITY        16         4    StoreLocal
//   PERMIT_STORE_CAPABILITY       32         5    Load
//
// With them, clang_purecap_input's "read-only" capability kept Store and clang_purecap_output's
// "write-only" one kept Load, so the core correctly let the access through and the tests failed;
// clang_purecap_init's "not executable" check tested LoadGlobal, which a pointer to a global has.
// Found 2026-09-29 by comparing `clang -dM` for the CHERIoT target with the RTOS's cheri.h.
//
// The values below are cheri.h's CheriPermission* bit numbers. CHERIoT has one bit (MC) for both
// loading and storing capabilities, so LOAD_CAPABILITY and STORE_CAPABILITY are the same bit.
// PERMIT_INVOKE has no CHERIoT equivalent and is left undefined, so a test that uses it fails to
// compile instead of silently testing another permission.
//
// Include before any use of the macros: cheri_c_test_framework.h includes it for the upstream
// tests, and the wrappers that use the macros directly include it themselves.

#pragma once

#undef __CHERI_CAP_PERMISSION_GLOBAL__
#undef __CHERI_CAP_PERMISSION_PERMIT_EXECUTE__
#undef __CHERI_CAP_PERMISSION_PERMIT_LOAD__
#undef __CHERI_CAP_PERMISSION_PERMIT_STORE__
#undef __CHERI_CAP_PERMISSION_PERMIT_LOAD_CAPABILITY__
#undef __CHERI_CAP_PERMISSION_PERMIT_STORE_CAPABILITY__
#undef __CHERI_CAP_PERMISSION_PERMIT_STORE_LOCAL__
#undef __CHERI_CAP_PERMISSION_PERMIT_SEAL__
#undef __CHERI_CAP_PERMISSION_PERMIT_UNSEAL__
#undef __CHERI_CAP_PERMISSION_PERMIT_INVOKE__
#undef __CHERI_CAP_PERMISSION_ACCESS_SYSTEM_REGISTERS__

#define __CHERI_CAP_PERMISSION_GLOBAL__                  (1U << 0)  // GL
#define __CHERI_CAP_PERMISSION_PERMIT_STORE__            (1U << 2)  // SD
#define __CHERI_CAP_PERMISSION_PERMIT_STORE_LOCAL__      (1U << 4)  // SL
#define __CHERI_CAP_PERMISSION_PERMIT_LOAD__             (1U << 5)  // LD
#define __CHERI_CAP_PERMISSION_PERMIT_LOAD_CAPABILITY__  (1U << 6)  // MC
#define __CHERI_CAP_PERMISSION_PERMIT_STORE_CAPABILITY__ (1U << 6)  // MC
#define __CHERI_CAP_PERMISSION_ACCESS_SYSTEM_REGISTERS__ (1U << 7)  // SR
#define __CHERI_CAP_PERMISSION_PERMIT_EXECUTE__          (1U << 8)  // EX
#define __CHERI_CAP_PERMISSION_PERMIT_UNSEAL__           (1U << 9)  // US
#define __CHERI_CAP_PERMISSION_PERMIT_SEAL__             (1U << 10) // SE
