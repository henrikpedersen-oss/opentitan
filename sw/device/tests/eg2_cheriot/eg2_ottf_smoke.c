// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_ottf_smoke (EG2 spec "Framework and mode switch"; REQ_STE_02).
 *
 * A CHERIoT build of OpenTitan's test framework runs an unchanged test source in CHERIoT mode: on
 * every boot it switches and locks CHERIoT mode before the test body, and it derives every device
 * pointer (mmio_region) from the memory root with bounds equal to that device's register window,
 * and every RAM pointer from the root with bounds equal to its region.
 *
 * An OTTF test (OTTF_DEFINE_TEST_CONFIG, test_main), built on OTTF's CHERIoT port
 * (test_framework/cheriot, the EG2 framework's start-up code underneath). The checks:
 *  - misa: E set, I clear (CHERIoT mode before the body);
 *  - the lock: a request of ePMP mode (CHERIOT_ENA MuBi4False, CHERIOT_LOCK MuBi4True) changes
 *    nothing;
 *  - `n_windows` of the bus windows of the generated table (eg2_tables.h, from the top's hjson),
 *    in `window_order` (address order, reverse, random): mmio_region_from_addr(base)'s capability
 *    is tagged, unsealed, based at the window's base with its exact length and the memory root's
 *    permissions (the capability model's root, from CHERIoT-Sail);
 *  - the stack pointer: tagged, bounded to [_stack_start, _stack_end).
 * The knobs are read through the EG2 parameter block (eg2_params.h), printed and range-checked
 * first.
 *
 * Built with EG2_OTTF_SMOKE_INJECT_FAIL, the first window's length is expected one byte longer: the
 * run must fail with `MISMATCH window length`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/eg2/eg2_params.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/lib/testing/eg2/eg2_tables.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"

OTTF_DEFINE_TEST_CONFIG();

#define OTTF_SMOKE_PARAMS(X) \
  X(n_windows, 0, 0, 255)    \
  X(window_order, 0, 0, 3)
EG2_TEST_PARAMS(OTTF_SMOKE_PARAMS);

typedef struct window {
  uint32_t base;
  uint32_t size;
} window_t;

#define EG2_WINDOW_ROW_(name, base, size) {(base), (size)},
static const window_t kWindows[] = {EG2_WINDOW_TABLE(EG2_WINDOW_ROW_)};
#undef EG2_WINDOW_ROW_

enum {
  kWindowCount = ARRAYSIZE(kWindows),
};
_Static_assert(kWindowCount % 7 != 0, "the random order steps by 7 through the windows");

static eg2_tally_t tally;

static void check_window(uint32_t i, bool first) {
  ++tally.planned;
  mmio_region_t r = mmio_region_from_addr(kWindows[i].base);
  void *c = (void *)r.base;
  uint32_t tag, base, len, perm, type;
  __asm__ volatile("cgettag %0, %1" : "=r"(tag) : "C"(c));
  __asm__ volatile("cgetbase %0, %1" : "=r"(base) : "C"(c));
  __asm__ volatile("cgetlen %0, %1" : "=r"(len) : "C"(c));
  __asm__ volatile("cgetperm %0, %1" : "=r"(perm) : "C"(c));
  __asm__ volatile("cgettype %0, %1" : "=r"(type) : "C"(c));
  uint32_t exp_len = kWindows[i].size;
#ifdef EG2_OTTF_SMOKE_INJECT_FAIL
  if (first) {
    exp_len += 1;
  }
#else
  (void)first;
#endif
  uint32_t failed = tally.failed;
  eg2_check(&tally, "window tag", i, tag, 1);
  eg2_check(&tally, "window base", i, base, kWindows[i].base);
  eg2_check(&tally, "window length", i, len, exp_len);
  eg2_check(&tally, "window permissions", i, perm, eg2_cap_perms(eg2_cap_root_mem(0)));
  eg2_check(&tally, "window object type", i, type, 0);
  if (tally.failed != failed && failed < kEg2MaxReports) {
    eg2_put_kv("  window", kWindows[i].base);
    eg2_put_kv(" size", kWindows[i].size);
    eg2_puts("\r\n");
  }
  ++tally.done;
}

bool test_main(void) {
  // OTTF leaves UART0's transmitter off on sim_dv (its log goes to the DV window); the EG2
  // console needs it, as in the framework's test_main.
  eg2_console_init();
  if (!eg2_params_begin()) {
    return false;
  }
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  eg2_check(&tally, "misa E,I", 0, misa & ((1u << 4) | (1u << 8)), 1u << 4);

  // The lock: a request of ePMP mode changes nothing.
  mmio_region_t cfg = mmio_region_from_addr(TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR);
  mmio_region_write32(cfg, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET, 0x9);
  mmio_region_write32(cfg, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET, 0x6);
  __asm__ volatile("fence.i");
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  eg2_check(&tally, "misa after an ePMP request", 0, misa & ((1u << 4) | (1u << 8)), 1u << 4);

  // n_windows: 0 draws (half the runs every window), anything above the count is every window.
  uint32_t n = EG2_PARAM(n_windows);
  if (n == 0) {
    n = eg2_pick(2) ? kWindowCount : eg2_rand_range(1, kWindowCount);
  } else if (n > kWindowCount) {
    n = kWindowCount;
  }
  uint32_t order = EG2_PARAM(window_order) != 0 ? EG2_PARAM(window_order) : eg2_rand_range(1, 3);
  LOG_INFO("EG2 ottf_smoke: %d of %d windows, order %d", (int)n, (int)kWindowCount, (int)order);
  uint32_t start = order == 3 ? eg2_pick(kWindowCount) : 0;
  bool first = true;
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t i = order == 1 ? k
                 : order == 2 ? kWindowCount - 1 - k
                              : (start + k * 7) % kWindowCount;  // 7 is prime to the count
    // A window at the base of an earlier one (sram_ctrl_meta.ram under cheriot_mem_sys.revbm, which
    // only the memory subsystem reaches) has no pointer of its own.
    bool alias = false;
    for (uint32_t j = 0; j < i; ++j) {
      alias = alias || kWindows[j].base == kWindows[i].base;
    }
    if (alias) {
      continue;
    }
    check_window(i, first);
    first = false;
  }

  // The stack.
  void *sp;
  __asm__ volatile("cmove %0, csp" : "=C"(sp));
  uint32_t sp_base, sp_len, sp_tag;
  __asm__ volatile("cgetbase %0, %1" : "=r"(sp_base) : "C"(sp));
  __asm__ volatile("cgetlen %0, %1" : "=r"(sp_len) : "C"(sp));
  __asm__ volatile("cgettag %0, %1" : "=r"(sp_tag) : "C"(sp));
  eg2_check(&tally, "stack tag", 0, sp_tag, 1);
  eg2_check(&tally, "stack base", 0, sp_base, eg2_sym_stack_start());
  eg2_check(&tally, "stack length", 0, sp_len, eg2_sym_stack_end() - eg2_sym_stack_start());

  eg2_tally_print("ottf_smoke", &tally);
  return eg2_tally_ok(&tally);
}
