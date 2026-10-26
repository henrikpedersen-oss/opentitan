// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

#include <stddef.h>

// The assembly's offsets (eg2_probe.S).
_Static_assert(offsetof(eg2_trap_rec_t, recover) == 0, "REC_RECOVER");
_Static_assert(offsetof(eg2_trap_rec_t, save1) == 8, "REC_SAVE1");
_Static_assert(offsetof(eg2_trap_rec_t, save2) == 16, "REC_SAVE2");
_Static_assert(offsetof(eg2_trap_rec_t, mepcc) == 24, "REC_MEPCC");
_Static_assert(offsetof(eg2_trap_rec_t, cap) == 32, "REC_CAP");
_Static_assert(offsetof(eg2_trap_rec_t, prev_tcc) == 40, "REC_PREV_TCC");
_Static_assert(offsetof(eg2_trap_rec_t, armed) == 48, "REC_ARMED");
_Static_assert(offsetof(eg2_trap_rec_t, mcause) == 52, "REC_MCAUSE");
_Static_assert(offsetof(eg2_trap_rec_t, mtval) == 56, "REC_MTVAL");
_Static_assert(offsetof(eg2_trap_rec_t, mstatus) == 60, "REC_MSTATUS");
_Static_assert(offsetof(eg2_trap_rec_t, count) == 64, "REC_COUNT");
_Static_assert(offsetof(eg2_trap_rec_t, value) == 68, "REC_VALUE");
_Static_assert(offsetof(eg2_trap_rec_t, value2) == 72, "REC_VALUE2");
_Static_assert(offsetof(eg2_trap_rec_t, mepcc_hi) == 76, "REC_MEPCC_HI");
_Static_assert(offsetof(eg2_trap_rec_t, mepcc_tag) == 80, "REC_MEPCC_TAG");
_Static_assert(offsetof(eg2_trap_rec_t, irq_count) == 84, "REC_IRQ");
_Static_assert(offsetof(eg2_trap_rec_t, irq_mepc) == 96, "REC_IRQ_MEPC");

#undef eg2_trap_rec
static eg2_trap_rec_t rec_storage __attribute__((aligned(8)));

__attribute__((noinline)) eg2_trap_rec_t *eg2_rec(void) {
  eg2_trap_rec_t *r = &rec_storage;
#ifdef __CHERIOT__
  __asm__ volatile("" : "+C"(r));  // opaque: callers address fields from the pointer
#else
  __asm__ volatile("" : "+r"(r));
#endif
  return r;
}
#define eg2_trap_rec (*eg2_rec())

extern char eg2_probe_trap[];

#ifdef __CHERIOT__

// MScratchC as the test found it: the sealing root (EG2 spec REQ_SOC_07).
static void *saved_mscratchc;

void eg2_probe_begin(void) {
  void *tcc;
  void *scratch;
  __asm__ volatile("cspecialr %0, mtcc" : "=C"(tcc));
  __asm__ volatile("cspecialr %0, mscratchc" : "=C"(scratch));
  saved_mscratchc = scratch;
  eg2_trap_rec.prev_tcc.p = tcc;
  eg2_trap_rec.armed = 0;
  void *rec = &eg2_trap_rec;
  __asm__ volatile("cspecialw mscratchc, %0" : : "C"(rec) : "memory");
  // The handler from the executable root MTCC holds: a 64-byte aligned address, so legal.
  void *handler =
      __builtin_cheri_address_set(tcc, __builtin_cheri_address_get(eg2_probe_trap));
  __asm__ volatile("cspecialw mtcc, %0" : : "C"(handler) : "memory");
}

void eg2_probe_end(void) {
  void *tcc = eg2_trap_rec.prev_tcc.p;
  void *scratch = saved_mscratchc;
  __asm__ volatile("cspecialw mtcc, %0" : : "C"(tcc) : "memory");
  __asm__ volatile("cspecialw mscratchc, %0" : : "C"(scratch) : "memory");
}

#else  // RISC-V mode

static uint32_t saved_mscratch;

void eg2_probe_begin(void) {
  uint32_t tvec;
  uint32_t scratch;
  __asm__ volatile("csrr %0, mtvec" : "=r"(tvec));
  __asm__ volatile("csrr %0, mscratch" : "=r"(scratch));
  saved_mscratch = scratch;
  eg2_trap_rec.prev_tcc.pad = tvec;
  eg2_trap_rec.armed = 0;
  uint32_t rec = (uint32_t)(uintptr_t)&eg2_trap_rec;
  __asm__ volatile("csrw mscratch, %0" : : "r"(rec) : "memory");
  // Vectored mode, the only one Ibex has: the handler is a 256-byte aligned table (eg2_probe.S).
  uint32_t handler = (uint32_t)(uintptr_t)eg2_probe_trap | 1u;
  __asm__ volatile("csrw mtvec, %0" : : "r"(handler) : "memory");
}

void eg2_probe_end(void) {
  uint32_t tvec = (uint32_t)eg2_trap_rec.prev_tcc.pad;
  __asm__ volatile("csrw mtvec, %0" : : "r"(tvec) : "memory");
  __asm__ volatile("csrw mscratch, %0" : : "r"(saved_mscratch) : "memory");
}

#endif  // __CHERIOT__
