// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_mem/eg2_chk.h"

static const char *area_ = "EG2";
static bool injected_;
static uint32_t checks_;
static uint32_t fails_;

void eg2_chk_area(const char *area) { area_ = area; }

static void head(void) {
  eg2_puts(area_);
  eg2_puts(" ");
}

void eg2_chk_step(const char *what, uint32_t value) {
  head();
  eg2_puts(what);
  eg2_puts(" 0x");
  eg2_puthex(value);
  eg2_puts("\r\n");
}

bool eg2_chk_eq(const char *what, uint32_t exp, uint32_t got) {
  if (kEg2ChkInjectFail && !injected_) {
    injected_ = true;
    exp ^= 1;
  }
  ++checks_;
  if (exp == got) {
    return true;
  }
  ++fails_;
  head();
  eg2_puts("FAIL ");
  eg2_puts(what);
  eg2_puts(" exp 0x");
  eg2_puthex(exp);
  eg2_puts(" got 0x");
  eg2_puthex(got);
  eg2_puts("\r\n");
  return false;
}

bool eg2_chk_true(const char *what, bool cond) {
  ++checks_;
  if (cond) {
    return true;
  }
  ++fails_;
  head();
  eg2_puts("FAIL ");
  eg2_puts(what);
  eg2_puts("\r\n");
  return false;
}

void eg2_chk_skip(const char *what) {
  head();
  eg2_puts("model_gaps skip ");
  eg2_puts(what);
  eg2_puts("\r\n");
}

bool eg2_chk_summary(uint32_t min_checks) {
  head();
  eg2_puts("summary checks ");
  eg2_putdec(checks_);
  eg2_puts(" failed ");
  eg2_putdec(fails_);
  eg2_puts("\r\n");
  if (checks_ < min_checks) {
    // A test that skipped its work fails (REQ_PBF_05).
    head();
    eg2_puts("FAIL too few checks, expected at least ");
    eg2_putdec(min_checks);
    eg2_puts("\r\n");
    return false;
  }
  return fails_ == 0;
}

void eg2_chk_delay(uint32_t cycles) {
  for (volatile uint32_t i = 0; i < cycles / 2; ++i) {
  }
}

bool eg2_chk_param(const char *name, uint32_t value, uint32_t min, uint32_t max) {
  // The framework's format (eg2_params.c), so the logs of every test read alike.
  eg2_puts("EG2 param ");
  eg2_puts(name);
  eg2_puts(" = ");
  eg2_putdec(value);
  eg2_puts(" (0x");
  eg2_puthex(value);
  eg2_puts(")\r\n");
  if (value < min || value > max) {
    eg2_puts("EG2 param ");
    eg2_puts(name);
    eg2_puts(" out of range [");
    eg2_putdec(min);
    eg2_puts(", ");
    eg2_putdec(max);
    eg2_puts("]\r\n");
    return false;
  }
  return true;
}

uint32_t eg2_chk_pick(const uint32_t *weights, uint32_t n) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < n; ++i) {
    total += weights[i];
  }
  if (total == 0) {
    return n;
  }
  uint32_t r = eg2_rand_range(0, total - 1);
  for (uint32_t i = 0; i < n; ++i) {
    if (r < weights[i]) {
      return i;
    }
    r -= weights[i];
  }
  return n - 1;
}

// The SRAM-call routine (eg2_chk_call.S): its first byte and its size.
extern const uint32_t eg2_chk_sram_call_start[];  // kEg2ChkSramCallBytes long
extern const char eg2_chk_stub_inc[];

#ifdef __CHERIOT__
#define CHK_ADDR(p) ((uint32_t)__builtin_cheri_address_get(p))
#else
#define CHK_ADDR(p) ((uint32_t)(uintptr_t)(p))
#endif

uint32_t eg2_chk_stub_inc_addr(void) { return CHK_ADDR(eg2_chk_stub_inc); }

bool eg2_chk_sram_call_install(uint32_t sram_addr) {
  if ((sram_addr & 0xff) != 0) {
    return false;
  }
  // Word copies: the routine is word aligned, and sub-word stores into SRAM no write has
  // initialised would meet its integrity bits uninitialised.
  const volatile uint32_t *src = eg2_chk_sram_call_start;
  volatile uint32_t *dst = eg2_chk_ptr(sram_addr);
  for (uint32_t i = 0; i < kEg2ChkSramCallBytes / 4; ++i) {
    dst[i] = src[i];
  }
  eg2_chk_fence_i();
  return true;
}

// eg2_chk_call.S
uint32_t eg2_chk_sram_call_asm(volatile uint32_t *reg, uint32_t value, uint32_t target,
                               uint32_t restore, eg2_chk_trap_t *out, uint32_t entry);

uint32_t eg2_chk_sram_call(uint32_t sram_addr, uint32_t reg, uint32_t value, uint32_t restore,
                           uint32_t target, eg2_chk_trap_t *out) {
  uint32_t entry = sram_addr + 64;  // the routine's entry, after its trap handler
  return eg2_chk_sram_call_asm(eg2_chk_ptr(reg), value, target, restore, out, entry);
}
