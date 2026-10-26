// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_connectivity (EG2 verification spec, after chip_sw_eg2_mem_test; REQ_PER_01,
 * REQ_PER_02, REQ_PER_03, REQ_SEC_19, REQ_STE_11): every peripheral and module of the top is
 * present, answers at its address and is connected to the blocks software observes it through, in
 * either execution mode. One line per block, `CONN <block> PASS|FAIL <checks>`, a line per
 * connection between blocks, and a summary.
 *
 * Per register block (the generated table eg2_regmem_tables.h, from the top's and the IPs' hjson),
 * selected by `blocks_lo`/`blocks_hi` (a bit per block, in table order) and visited in `order`:
 *   - present: a register with a defined reset value no hardware updates reads it (the one with
 *     the most predicted one bits), with no bus error;
 *   - answers: a plain rw register written with its rw bits inverted reads back, then restored;
 *   - bounds: a read one word past the window faults: through the window's own capability a
 *     bounds fault (CHERIoT mode), through an unbounded pointer a bus error unless the crossbar's
 *     map gives the address to another window;
 *   - interrupt: each interrupt, with only its source enabled at rv_plic (priority 1, threshold
 *     0), is pending there after INTR_TEST (or INTR_ENABLE, for a status interrupt already high),
 *     is claimed with its source ID and, completed with the interrupt disabled and cleared, is no
 *     longer pending; rv_timer's also sets mip.MTIP;
 *   - alert: each alert raised through ALERT_TEST (the status module, eg2_status, owns the alert
 *     handler) sets its cause bit at the alert handler, and no other cause bit.
 * Between blocks, where software can observe the connection without pins: otp_ctrl to lc_ctrl
 * (the OTP image's life-cycle state), the AST to sensor_ctrl, clkmgr's hints and the transactional
 * blocks' idle (CLK_HINTS_STATUS), otp_ctrl's key to sram_ctrl, EDN0 to rv_core_ibex (random
 * data), rstmgr's software resets on their blocks (a register written, the block reset, the
 * register back at its reset value), aon_timer's wake-up request at pwrmgr (WAKE_STATUS, after
 * the enable mask), and last the ALERT_TEST lock (a 0 in its REGWEN bit stops ALERT_TEST).
 *
 * `model_gaps` (default 0) skips what the EG2 SystemC model does not model (eg2sim runs only).
 *
 * Built with `EG2_REGMEM_INJECT_FAIL`, the first interrupt is expected at the wrong rv_plic
 * source ID: the run must fail with `CONN <block> FAIL interrupt`.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/aon_timer_regs.h"      // Generated.
#include "hw/top/clkmgr_regs.h"         // Generated.
#include "hw/top/lc_ctrl_regs.h"        // Generated.
#include "hw/top/pwrmgr_regs.h"         // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "hw/top/rv_core_ibex_regs.h"   // Generated.
#include "hw/top/sensor_ctrl_regs.h"    // Generated.
#include "hw/top/sram_ctrl_regs.h"      // Generated.
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem_tables.h"

EG2_TEST_CONFIG();

#define CONNECTIVITY_PARAMS(X)              \
  X(blocks_lo, 0xffffffffu, 0, 0xffffffffu) \
  X(blocks_hi, 0xffffffffu, 0, 0xffffffffu) \
  X(order, 0, 0, 2)                         \
  X(alert_polls, 10000, 100, 1000000)       \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(CONNECTIVITY_PARAMS)

enum {
  kNone = 0xffff,
  kPlicPolls = 100,
  kLinkPolls = 10000,
  kMipMtip = 1u << 7,
};
// ALERT_TEST's in-register write enable (reggen: rw0c, reset 1).
static const uint32_t kAlertTestRegwen = 1u << 31;

typedef struct ctx {
  bool gaps;
  bool injected;
  uint32_t checks;
  uint32_t fails;
  uint32_t gap_skips;
  const char *where;  // the block or connection under test
} ctx_t;

static const char *name_of(uint32_t off) { return &kEg2RmNames[off]; }

// `CONN <where> FAIL <check> <what>: got .. expected ..` for a mismatch in `mask`.
static bool check(ctx_t *c, const char *chk, const char *what, uint32_t got, uint32_t exp,
                  uint32_t mask) {
  ++c->checks;
  if (((got ^ exp) & mask) == 0) {
    return true;
  }
  ++c->fails;
  eg2_puts("CONN ");
  eg2_puts(c->where);
  eg2_puts(" FAIL ");
  eg2_puts(chk);
  eg2_puts(" ");
  eg2_puts(what);
  eg2_rm_hex(": got ", got);
  eg2_rm_hex(" expected ", exp);
  eg2_rm_hex(" mask ", mask);
  eg2_puts("\r\n");
  return false;
}

static uint32_t plic_rd(uint32_t off) { return eg2_rm_rd32(TOP_EARLGREY_RV_PLIC_BASE_ADDR + off); }
static void plic_wr(uint32_t off, uint32_t v) {
  eg2_rm_wr32(TOP_EARLGREY_RV_PLIC_BASE_ADDR + off, v);
}
static uint32_t plic_pending(uint32_t id) {
  return (plic_rd(EG2_RM_PLIC_IP_0 + 4 * (id / 32)) >> (id % 32)) & 1;
}

static uint32_t mip(void) {
  uint32_t v;
  __asm__ volatile("csrr %0, mip" : "=r"(v));
  return v;
}

static uint32_t poll(uint32_t addr, uint32_t mask, uint32_t want) {
  uint32_t v = 0;
  for (int i = 0; i < kLinkPolls; ++i) {
    v = eg2_rm_rd32(addr);
    if ((v & mask) == want) {
      break;
    }
  }
  return v;
}

// The size of the bus window at `base` (eg2_tables.h's window table), 0 if none.
static uint32_t window_size(uint32_t base) {
#define EG2_RM_SIZE_(name, b, s) \
  if (base == (uint32_t)(b)) {   \
    return (uint32_t)(s);        \
  }
  EG2_WINDOW_TABLE(EG2_RM_SIZE_)
#undef EG2_RM_SIZE_
  return 0;
}

// Whether the crossbar maps `addr` to a window. sram_ctrl_meta.ram is not on the crossbar
// (cheriot_mem_sys.revbm, at the same base, is the software's view of it).
static bool mapped(uint32_t addr) {
#define EG2_RM_MAPPED_(name, b, s)                                      \
  if (!((uint32_t)(b) == TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR &&   \
        (uint32_t)(s) == TOP_EARLGREY_SRAM_CTRL_META_RAM_SIZE_BYTES) && \
      addr - (uint32_t)(b) < (uint32_t)(s)) {                           \
    return true;                                                        \
  }
  EG2_WINDOW_TABLE(EG2_RM_MAPPED_)
#undef EG2_RM_MAPPED_
  return false;
}

// An unbounded pointer to a bus address (the memory root's in CHERIoT mode).
static volatile void *root_ptr(uint32_t addr) {
#ifdef __CHERIOT__
  void *root;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(root));
  return (volatile void *)__builtin_cheri_address_set(root, addr);
#else
  return (volatile void *)(uintptr_t)addr;
#endif
}

// One interrupt of block b; true if every check passed.
static bool one_intr(ctx_t *c, const eg2_rm_block_t *b, const eg2_rm_intr_t *it) {
  uint32_t fails0 = c->fails;
  uint32_t id = it->plic_id;
  uint32_t bit = 1u << it->bit;
  uint32_t ie_off = EG2_RM_PLIC_IE0_0 + 4 * (id / 32);
  uint32_t prio_off = EG2_RM_PLIC_PRIO_0 + 4 * id;
  uint32_t old_ie = plic_rd(ie_off), old_prio = plic_rd(prio_off);
  uint32_t old_thr = plic_rd(EG2_RM_PLIC_THRESHOLD0);
  bool has_en = b->intr_enable != kNone;
  uint32_t old_en = has_en ? eg2_rm_rd32(b->base + b->intr_enable) : 0;

  if (has_en) {
    eg2_rm_wr32(b->base + b->intr_enable, 0);
  }
  eg2_rm_wr32(b->base + b->intr_state, bit);  // an event interrupt left from before
  // Only this source enabled, so a claim can only return it; anything still pending is claimed.
  plic_wr(ie_off, 1u << (id % 32));
  plic_wr(prio_off, 1);
  plic_wr(EG2_RM_PLIC_THRESHOLD0, 0);
  for (int i = 0; i < 4 && plic_pending(id); ++i) {
    plic_wr(EG2_RM_PLIC_CC0, plic_rd(EG2_RM_PLIC_CC0));
  }

  if (has_en) {
    eg2_rm_wr32(b->base + b->intr_enable, bit);
  }
  bool high = (eg2_rm_rd32(b->base + b->intr_state) & bit) != 0;
  if (!(it->status && high)) {
    eg2_rm_wr32(b->base + b->intr_test, bit);
  }
  uint32_t pend = 0;
  for (int i = 0; i < kPlicPolls && !pend; ++i) {
    pend = plic_pending(id);
  }
  check(c, "interrupt", "pending at rv_plic", pend, 1, 1);
  if (b->base == TOP_EARLGREY_RV_TIMER_BASE_ADDR) {
    check(c, "interrupt", "rv_timer at the core (mip.MTIP)", mip() & kMipMtip, kMipMtip,
          kMipMtip);
  }
  uint32_t claimed = plic_rd(EG2_RM_PLIC_CC0);
  uint32_t want = id;
#ifdef EG2_REGMEM_INJECT_FAIL
  if (!c->injected) {
    c->injected = true;
    want = id + 1;  // the wrong source ID
  }
#endif
  check(c, "interrupt", "claimed source ID", claimed, want, 0xffffffffu);
  if (has_en) {
    eg2_rm_wr32(b->base + b->intr_enable, 0);
  }
  eg2_rm_wr32(b->base + b->intr_state, bit);
  plic_wr(EG2_RM_PLIC_CC0, claimed);
  // (Without INTR_ENABLE, as aon_timer, the line follows INTR_STATE, cleared above.)
  check(c, "interrupt", "pending after completion, disabled and cleared", plic_pending(id), 0, 1);

  plic_wr(prio_off, old_prio);
  plic_wr(ie_off, old_ie);
  plic_wr(EG2_RM_PLIC_THRESHOLD0, old_thr);
  if (has_en) {
    eg2_rm_wr32(b->base + b->intr_enable, old_en);
  }
  return c->fails == fails0;
}

// One alert: raised through ALERT_TEST, its cause set and no other; true if so.
static bool one_alert(ctx_t *c, size_t alert) {
  uint32_t fails0 = c->fails;
  eg2_status_expect_alert(alert);
  eg2_status_raise_alert(alert);
  check(c, "alert", eg2_status_alert_name(alert),
        eg2_status_wait_alert(alert, EG2_PARAM(alert_polls)), 1, 1);
  for (size_t k = 0; k < kEg2AlertCount; ++k) {
    if (k != alert) {
      uint32_t cause = eg2_rm_rd32(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR +
                                   ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4 * (uint32_t)k);
      // Another cause set as well: name it.
      check(c, "alert", eg2_status_alert_name(k), cause, 0, 1);
    }
  }
  eg2_status_clear_expected();
  return c->fails == fails0;
}

static void one_block(ctx_t *c, const eg2_rm_block_t *b) {
  c->where = name_of(b->name);
  uint32_t fails0 = c->fails;
  eg2_rm_out_t out;
  uint32_t nint = 0, nalert = 0;

  // Present: a known reset value, with no bus error.
  uint32_t f = c->fails;
  const eg2_rm_reg_t *pr = &kEg2RmRegs[b->probe_rd != kNone ? b->probe_rd : b->first];
  uint32_t cause = eg2_rm_probe(eg2_rm_ptr(b->base + pr->offset), kEg2RmLw, 0, &out);
  check(c, "present", "read faulted (mcause)", cause, 0, 0xffffffffu);
  if (b->probe_rd != kNone) {
    check(c, "present", name_of(pr->name), out.value, pr->resval, kEg2RmMasks[pr->masks].known);
  }
  bool present = c->fails == f;

  // Answers: a write reads back.
  f = c->fails;
  bool answers = false;
  if (b->probe_wr != kNone) {
    const eg2_rm_reg_t *r = &kEg2RmRegs[b->probe_wr];
    const eg2_rm_masks_t *m = &kEg2RmMasks[r->masks];
    uint32_t addr = b->base + r->offset;
    bool open = true;
    if (r->regwen != kNone) {
      uint32_t g = eg2_rm_rd32(b->base + kEg2RmRegs[r->regwen].offset);
      open = (r->flags & kEg2RmWenMubi) ? (g & 0xf) == 0x6 : (g & 1) != 0;
    }
    if (open) {
      uint32_t orig = eg2_rm_rd32(addr);
      uint32_t bits = m->rw & ~m->mubi;
      uint32_t v = (orig & ~m->neutral0) ^ bits;
      int n = (r->flags & kEg2RmShadowed) ? 2 : 1;  // a shadowed register takes two writes
      for (int i = 0; i < n; ++i) {
        eg2_rm_wr32(addr, v);
      }
      check(c, "answers", name_of(r->name), eg2_rm_rd32(addr), v, bits);
      for (int i = 0; i < n; ++i) {
        eg2_rm_wr32(addr, orig & ~m->neutral0);
      }
      check(c, "answers", "restored", eg2_rm_rd32(addr), orig, bits);
      answers = c->fails == f;
    }
  }

  // Bounds: one word past the window.
  f = c->fails;
  uint32_t size = window_size(b->base);
  uint32_t past = b->base + size;
  if (size == 0) {
    check(c, "bounds", "window not in the window table", 0, 1, 1);
  } else {
#ifdef __CHERIOT__
    cause = eg2_rm_probe((volatile uint8_t *)eg2_rm_ptr(b->base) + size, kEg2RmLw, 0, &out);
    check(c, "bounds", "through the window's capability (mcause)", cause, kEg2RmCauseCheri,
          0xffffffffu);
#endif
    bool next = mapped(past);
    cause = eg2_rm_probe(root_ptr(past), kEg2RmLw, 0, &out);
    check(c, "bounds", next ? "the next window's read (mcause)" : "unmapped read (mcause)", cause,
          next ? 0 : (uint32_t)kEg2RmCauseLoadFault, 0xffffffffu);
  }
  bool bounds = c->fails == f;

  for (uint32_t i = 0; i < b->nintr; ++i) {
    nint += one_intr(c, b, &kEg2RmIntrs[b->intr0 + i]);
  }
  for (uint32_t i = 0; i < b->nalert; ++i) {
    nalert += one_alert(c, b->alert0 + i);
  }

  eg2_puts("CONN ");
  eg2_puts(c->where);
  eg2_puts(c->fails == fails0 ? " PASS" : " FAIL");
  eg2_puts(present ? " present" : " present?");
  eg2_puts(b->probe_wr == kNone ? "" : answers ? " answers" : " answers?");
  eg2_puts(bounds ? " bounds" : " bounds?");
  eg2_puts(" interrupts ");
  eg2_putdec(nint);
  eg2_puts("/");
  eg2_putdec(b->nintr);
  eg2_puts(" alerts ");
  eg2_putdec(nalert);
  eg2_puts("/");
  eg2_putdec(b->nalert);
  eg2_puts("\r\n");
}

static void link_begin(ctx_t *c, const char *what, uint32_t *f) {
  c->where = what;
  *f = c->fails;
}
static void link_end(ctx_t *c, uint32_t f) {
  eg2_puts("CONN link ");
  eg2_puts(c->where);
  eg2_puts(c->fails == f ? " PASS\r\n" : " FAIL\r\n");
}

// rstmgr's software reset of block b: a register written, the block reset, the register back at
// its reset value.
static void sw_reset(ctx_t *c, const eg2_rm_block_t *b) {
  const eg2_rm_reg_t *r = &kEg2RmRegs[b->probe_wr];
  const eg2_rm_masks_t *m = &kEg2RmMasks[r->masks];
  uint32_t addr = b->base + r->offset;
  uint32_t bits = m->rw & ~m->mubi;
  uint32_t v = (r->resval & ~m->neutral0) ^ bits;
  int n = (r->flags & kEg2RmShadowed) ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    eg2_rm_wr32(addr, v);
  }
  uint32_t ctrl =
      TOP_EARLGREY_RSTMGR_BASE_ADDR + RSTMGR_SW_RST_CTRL_N_0_REG_OFFSET + 4u * b->sw_rst;
  eg2_rm_wr32(ctrl, 0);  // in reset (active low); the block is not accessed meanwhile
  eg2_rm_wr32(ctrl, 1);
  check(c, "link", name_of(b->name), eg2_rm_rd32(addr), r->resval, bits);
}

static void links(ctx_t *c) {
  uint32_t f;

  link_begin(c, "otp_ctrl -> lc_ctrl (LC_STATE RMA, STATUS.READY)", &f);
  check(c, "link", "LC_STATE",
        eg2_rm_rd32(TOP_EARLGREY_LC_CTRL_REGS_BASE_ADDR + LC_CTRL_LC_STATE_REG_OFFSET),
        LC_CTRL_LC_STATE_STATE_VALUE_RMA, 0xffffffffu);
  check(c, "link", "STATUS.READY",
        eg2_rm_rd32(TOP_EARLGREY_LC_CTRL_REGS_BASE_ADDR + LC_CTRL_STATUS_REG_OFFSET),
        1u << LC_CTRL_STATUS_READY_BIT, 1u << LC_CTRL_STATUS_READY_BIT);
  link_end(c, f);

  link_begin(c, "ast -> sensor_ctrl (STATUS.AST_INIT_DONE)", &f);
  check(c, "link", "STATUS.AST_INIT_DONE",
        eg2_rm_rd32(TOP_EARLGREY_SENSOR_CTRL_BASE_ADDR + SENSOR_CTRL_STATUS_REG_OFFSET),
        1u << SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT, 1u << SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT);
  link_end(c, f);

  // With its hint cleared an idle block's clock stops, and CLK_HINTS_STATUS says so.
  link_begin(c, "clkmgr <-> aes, hmac, kmac, otbn (CLK_HINTS, CLK_HINTS_STATUS)", &f);
  uint32_t hints_reg = TOP_EARLGREY_CLKMGR_BASE_ADDR + CLKMGR_CLK_HINTS_REG_OFFSET;
  uint32_t status_reg = TOP_EARLGREY_CLKMGR_BASE_ADDR + CLKMGR_CLK_HINTS_STATUS_REG_OFFSET;
  uint32_t hints = eg2_rm_rd32(hints_reg);
  for (uint32_t bit = 0; bit < 4; ++bit) {
    uint32_t m = 1u << bit;
    eg2_rm_wr32(hints_reg, hints & ~m);
    check(c, "link", "clock stopped (hint 0, block idle)", poll(status_reg, m, 0), 0, m);
    eg2_rm_wr32(hints_reg, hints);
    check(c, "link", "clock running (hint 1)", poll(status_reg, m, m), m, m);
  }
  link_end(c, f);

  // A key renewal gets a valid key (the secondary SRAM's contents are lost; nothing uses them).
  link_begin(c, "otp_ctrl -> sram_ctrl_sec (RENEW_SCR_KEY, STATUS.SCR_KEY_VALID)", &f);
  uint32_t valid = 1u << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT;
  eg2_rm_wr32(TOP_EARLGREY_SRAM_CTRL_SEC_REGS_BASE_ADDR + SRAM_CTRL_CTRL_REG_OFFSET,
              1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  check(c, "link", "STATUS.SCR_KEY_VALID",
        poll(TOP_EARLGREY_SRAM_CTRL_SEC_REGS_BASE_ADDR + SRAM_CTRL_STATUS_REG_OFFSET, valid,
             valid),
        valid, valid);
  link_end(c, f);

  // Random data arrives (the test ROM started the entropy complex, EDN0 in boot mode).
  link_begin(c, "edn0 -> rv_core_ibex (RND_STATUS, RND_DATA)", &f);
  uint32_t rnd_st = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR + RV_CORE_IBEX_RND_STATUS_REG_OFFSET;
  uint32_t rnd = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR + RV_CORE_IBEX_RND_DATA_REG_OFFSET;
  uint32_t ok = 1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT;
  check(c, "link", "RND_STATUS.RND_DATA_VALID", poll(rnd_st, ok, ok), ok, ok);
  uint32_t r1 = eg2_rm_rd32(rnd);
  (void)poll(rnd_st, ok, ok);
  uint32_t r2 = eg2_rm_rd32(rnd);
  check(c, "link", "two random words differ", r1 != r2, 1, 1);
  link_end(c, f);

  // rstmgr's software resets: each on its block.
  link_begin(c, "rstmgr -> software-reset blocks (SW_RST_CTRL_N)", &f);
  if (c->gaps) {
    ++c->gap_skips;  // the model keeps SW_RST_CTRL_N as storage only
  } else {
    for (uint32_t k = 0; k < EG2_RM_BLOCK_COUNT; ++k) {
      const eg2_rm_block_t *b = &kEg2RmBlocks[k];
      if (b->sw_rst != 0xff && b->probe_wr != kNone) {
        sw_reset(c, b);
      }
    }
  }
  link_end(c, f);

  // aon_timer's wake-up request reaches pwrmgr: WAKE_STATUS shows it only while enabled
  // ("all current wake requests post enable mask") and drops when the request is cleared.
  link_begin(c, "aon_timer -> pwrmgr (wake-up request, WAKE_STATUS)", &f);
  uint32_t pw = TOP_EARLGREY_PWRMGR_BASE_ADDR, aon = TOP_EARLGREY_AON_TIMER_BASE_ADDR;
  uint32_t wk = 1u << PWRMGR_PARAM_AON_TIMER_WKUP_REQ_IDX;
  uint32_t old_en = eg2_rm_rd32(pw + PWRMGR_WAKEUP_EN_REG_OFFSET);
  eg2_rm_wr32(pw + PWRMGR_WAKEUP_EN_REG_OFFSET, old_en & ~wk);
  eg2_rm_wr32(pw + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)poll(pw + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1, 0);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 1);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_CTRL_REG_OFFSET, 1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  check(c, "link", "aon_timer WKUP_CAUSE", poll(aon + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 1, 1), 1,
        1);
  if (c->gaps) {
    ++c->gap_skips;  // the model shows wake-up requests in WAKE_STATUS before the enable mask
  } else {
    check(c, "link", "WAKE_STATUS with the wake-up disabled",
          eg2_rm_rd32(pw + PWRMGR_WAKE_STATUS_REG_OFFSET), 0, wk);
  }
  eg2_rm_wr32(pw + PWRMGR_WAKEUP_EN_REG_OFFSET, old_en | wk);
  eg2_rm_wr32(pw + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)poll(pw + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1, 0);
  check(c, "link", "WAKE_STATUS with the wake-up enabled",
        poll(pw + PWRMGR_WAKE_STATUS_REG_OFFSET, wk, wk), wk, wk);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  eg2_rm_wr32(aon + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  eg2_rm_wr32(aon + AON_TIMER_INTR_STATE_REG_OFFSET,
              1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  if (c->gaps) {
    ++c->gap_skips;  // the model keeps a wake-up request in WAKE_STATUS after it is cleared
  } else {
    check(c, "link", "WAKE_STATUS after the request is cleared",
          poll(pw + PWRMGR_WAKE_STATUS_REG_OFFSET, wk, 0), 0, wk);
  }
  eg2_rm_wr32(pw + PWRMGR_WAKEUP_EN_REG_OFFSET, old_en);
  eg2_rm_wr32(pw + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  (void)poll(pw + PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1, 0);
  link_end(c, f);

  // The ALERT_TEST lock: once its REGWEN bit is 0, ALERT_TEST no longer raises the alert. Last,
  // as it cannot be undone; on one alert, drawn from the seed.
  size_t alert = eg2_rand_range(0, kEg2AlertCount - 1);
  link_begin(c, "ALERT_TEST lock", &f);
  if (c->gaps) {
    ++c->gap_skips;  // the model raises the alert whatever the lock
  } else {
    for (uint32_t k = 0; k < EG2_RM_BLOCK_COUNT; ++k) {
      const eg2_rm_block_t *b = &kEg2RmBlocks[k];
      if (b->nalert != 0 && alert >= b->alert0 && alert < (size_t)b->alert0 + b->nalert) {
        uint32_t at = b->base + b->alert_test;
        uint32_t bit = 1u << (alert - b->alert0);
        eg2_rm_wr32(at, 0);                       // the lock
        eg2_rm_wr32(at, bit | kAlertTestRegwen);  // a 1 does not reopen it
        check(c, "link", "ALERT_TEST lock bit after a 0", eg2_rm_rd32(at), 0, kAlertTestRegwen);
        check(c, "link", eg2_status_alert_name(alert),
              eg2_status_wait_alert(alert, EG2_PARAM(alert_polls)), 0, 1);
      }
    }
  }
  link_end(c, f);
}

bool eg2_test_main(void) {
  eg2_rm_irq_off();
  if (!eg2_rm_params_begin()) {
    return false;
  }
  ctx_t c = {0};
  c.gaps = EG2_PARAM(model_gaps) != 0;

  // The blocks, in `order`: the top's, reversed, or drawn from the seed.
  uint8_t ord[EG2_RM_BLOCK_COUNT];
  for (uint32_t i = 0; i < EG2_RM_BLOCK_COUNT; ++i) {
    ord[i] = (uint8_t)(EG2_PARAM(order) == 1 ? EG2_RM_BLOCK_COUNT - 1 - i : i);
  }
  if (EG2_PARAM(order) == 2) {
    for (uint32_t i = EG2_RM_BLOCK_COUNT; i > 1; --i) {
      uint32_t j = eg2_rand_range(0, i - 1);
      uint8_t t = ord[i - 1];
      ord[i - 1] = ord[j];
      ord[j] = t;
    }
  }
  uint32_t nblocks = 0, failed = 0, nint = 0, nalert = 0;
  for (uint32_t i = 0; i < EG2_RM_BLOCK_COUNT; ++i) {
    uint32_t k = ord[i];
    uint32_t sel = k < 32 ? EG2_PARAM(blocks_lo) >> k : EG2_PARAM(blocks_hi) >> (k - 32);
    if (!(sel & 1)) {
      continue;
    }
    uint32_t f = c.fails;
    one_block(&c, &kEg2RmBlocks[k]);
    ++nblocks;
    failed += c.fails != f;
    nint += kEg2RmBlocks[k].nintr;
    nalert += kEg2RmBlocks[k].nalert;
  }
  links(&c);
  eg2_puts("CONN summary: ");
  eg2_putdec(nblocks);
  eg2_puts(" blocks, ");
  eg2_putdec(failed);
  eg2_puts(" failed, ");
  eg2_putdec(nint);
  eg2_puts(" interrupts, ");
  eg2_putdec(nalert);
  eg2_puts(" alerts, ");
  eg2_putdec(c.checks);
  eg2_puts(" checks, ");
  eg2_putdec(c.fails);
  eg2_puts(" failures; model-gap skips ");
  eg2_putdec(c.gap_skips);
  eg2_puts("\r\n");
  return c.fails == 0 && c.checks > 0;
}
