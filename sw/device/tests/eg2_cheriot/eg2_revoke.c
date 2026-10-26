// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

#include <stddef.h>

#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

bool eg2_rv_revocable(uint32_t base) { return base >= kEg2MsHeapBase && base < kEg2MsHeapTop; }

bool eg2_rv_sealing(eg2_cap_t c) {
  return (eg2_cap_perms(c) & (kEg2PermSe | kEg2PermUs | kEg2PermU0)) != 0;
}

bool eg2_rv_revokes(eg2_cap_t c, bool revbm) {
  return c.tag && !eg2_rv_sealing(c) && eg2_rv_revocable(eg2_cap_base(c)) && revbm;
}

eg2_cap_t eg2_rv_expect(eg2_cap_t auth, eg2_cap_t mem, bool revbm) {
  eg2_cap_t r = eg2_cap_clc_result(auth, mem);
  if (eg2_rv_revokes(r, revbm)) {
    r.tag = false;
  }
  return r;
}

// --------------------------------------- Bench steps -----------------------------------------

bool eg2_rv_count_revbm(uint32_t lo, uint32_t hi, bool bench) {
  return eg2_ms_bench("count_revbm", lo, hi, bench);
}

bool eg2_rv_count_begin(uint32_t lo, uint32_t hi, bool bench) {
  return eg2_ms_bench("count_begin", lo, hi, bench);
}

bool eg2_rv_count_end(eg2_rv_counts_t *out, bool bench) {
  out->core = 0;
  out->xbar = 0;
  out->bitmap = 0;
  out->valid = false;
  if (!eg2_ms_bench("count_end", 0, 0, bench)) {
    return false;
  }
  if (bench) {
    // {bitmap[29:20], xbar[19:10], core[9:0]} (chip_sw_eg2_revocation_bench_vseq).
    uint32_t r = eg2_ms_bench_result();
    out->core = r & 0x3ff;
    out->xbar = (r >> 10) & 0x3ff;
    out->bitmap = (r >> 20) & 0x3ff;
    out->valid = true;
  }
  return true;
}

bool eg2_rv_delays(bool on, bool bench) { return eg2_ms_bench("rsp_delay", on ? 1 : 0, 0, bench); }

bool eg2_rv_sba_noise(bool on, bool bench) {
  return eg2_ms_bench("sba_noise", on ? 1 : 0, 0, bench);
}

bool eg2_rv_tbre_watch_begin(uint32_t lo, uint32_t hi, bool bench) {
  return eg2_ms_bench("tbre_watch_begin", lo, hi, bench);
}

bool eg2_rv_tbre_watch_end(uint32_t *inside, uint32_t *outside, bool bench) {
  *inside = 0;
  *outside = 0;
  if (!eg2_ms_bench("tbre_watch_end", 0, 0, bench)) {
    return false;
  }
  if (bench) {
    *inside = eg2_ms_bench_result();
  }
  if (!eg2_ms_bench("tbre_watch_end", 1, 0, bench)) {
    return false;
  }
  if (bench) {
    *outside = eg2_ms_bench_result();
  }
  return true;
}

// ---------------------------------------- Op windows -----------------------------------------

// kEg2BenchAck (eg2_memsys.c).
extern volatile const uint32_t kEg2BenchAck[2];

uint32_t eg2_rv_ack_addr(void) { return EG2_ADDR(kEg2BenchAck); }

void eg2_rv_ops_init(eg2_rv_ops_t *o, uint32_t base, uint32_t max_ops) {
  o->base = base;
  o->n = 0;
  o->max = max_ops;
  o->outs = 0;
  o->full = false;
}

uint32_t eg2_rv_op(eg2_rv_ops_t *o, eg2_rv_op_kind_t kind, eg2_rv_auth_t auth, uint32_t addr,
                   uint32_t value) {
  if (o->n >= o->max) {
    o->full = true;
    return 0xffffffffu;
  }
  uint32_t a = o->base + 12 * o->n;
  eg2_ms_write32(a, (uint32_t)kind | ((uint32_t)auth << 8));
  eg2_ms_write32(a + 4, addr);
  eg2_ms_write32(a + 8, value);
  ++o->n;
  if (kind == kEg2RvOpSw || kind == kEg2RvOpSpin || kind == kEg2RvOpWfi) {
    return 0xffffffffu;
  }
  return o->outs++;
}

void eg2_rv_op_revbm(eg2_rv_ops_t *o, uint32_t word, uint32_t value) {
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthRevbm, kEg2MsMetaBase + 4 * word, value);
}

void eg2_rv_op_sweep(eg2_rv_ops_t *o, uint32_t base, uint32_t n_caps) {
  uint32_t regs = TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR;
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthRegs, regs + CHERIOT_MEM_SYS_TBRE_BASE_ADDR_REG_OFFSET,
                  base);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthRegs, regs + CHERIOT_MEM_SYS_TBRE_NUM_CAPS_REG_OFFSET,
                  n_caps);
  (void)eg2_rv_op(o, kEg2RvOpSw, kEg2RvAuthRegs, regs + CHERIOT_MEM_SYS_TBRE_START_REG_OFFSET, 1);
}

uint32_t eg2_rv_op_sweep_wait(eg2_rv_ops_t *o) {
  return eg2_rv_op(o, kEg2RvOpPollClear, kEg2RvAuthRegs,
                   TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR +
                       CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET,
                   1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT);
}

#ifdef __CHERIOT__
// eg2_revoke.S.
extern void eg2_rv_run(const uint32_t *ops, uint32_t n, void *const *auths, uint32_t *out);

void eg2_rv_ops_run(const eg2_rv_ops_t *o, uint32_t out) {
  void *auths[kEg2RvAuthCount];
  for (uint32_t i = 0; i < kEg2RvAuthCount; ++i) {
    auths[i] = NULL;
  }
  auths[kEg2RvAuthHeap] = eg2_mem(kEg2MsHeapBase, kEg2MsHeapTop - kEg2MsHeapBase);
  auths[kEg2RvAuthRevbm] = eg2_mem(kEg2MsMetaBase, kEg2MsRevbmSize);
  auths[kEg2RvAuthRegs] = (void *)eg2_dev(kEg2DevCheriotMemSysRegs);
  auths[kEg2RvAuthAck] = eg2_mem(eg2_rv_ack_addr(), 8);
  auths[kEg2RvAuthPlic] = (void *)eg2_dev(kEg2DevRvPlic);
  auths[kEg2RvAuthTimer] = (void *)eg2_dev(kEg2DevRvTimer);
  auths[kEg2RvAuthRstmgr] = (void *)eg2_dev(kEg2DevRstmgr);
  auths[kEg2RvAuthIbex] = (void *)eg2_dev(kEg2DevRvCoreIbexCfg);
  auths[kEg2RvAuthPwrmgr] = (void *)eg2_dev(kEg2DevPwrmgr);
  auths[kEg2RvAuthAon] = (void *)eg2_dev(kEg2DevAonTimer);
  eg2_rv_run(eg2_mem(o->base, 12 * o->n + 12), o->n, auths, eg2_mem(out, 4 * o->outs + 4));
}
#else
void eg2_rv_ops_run(const eg2_rv_ops_t *o, uint32_t out) {
  (void)o;
  (void)out;
}
#endif

// ------------------------- Bench steps of the later corner cases ----------------------------

bool eg2_rv_race_toggle_begin(uint32_t word_addr, uint32_t bit, bool bench) {
  return eg2_ms_bench("race_toggle_begin", word_addr, bit, bench);
}

bool eg2_rv_race_toggle_end(uint32_t *reads, uint32_t *seen, bool bench) {
  *reads = 0;
  *seen = 0;
  if (!eg2_ms_bench("race_toggle_end", 0, 0, bench)) {
    return false;
  }
  if (bench) {
    *reads = eg2_ms_bench_result();
  }
  if (!eg2_ms_bench("race_toggle_end", 1, 0, bench)) {
    return false;
  }
  if (bench) {
    *seen = eg2_ms_bench_result();
  }
  return true;
}

bool eg2_rv_tbre_hold_arm(uint32_t addr, uint32_t release_addr, bool bench) {
  return eg2_ms_bench("tbre_hold_arm", addr, release_addr, bench);
}

bool eg2_rv_tbre_hold_end(uint32_t *cycles, bool bench) {
  *cycles = 0;
  if (!eg2_ms_bench("tbre_hold_end", 0, 0, bench)) {
    return false;
  }
  if (bench) {
    *cycles = eg2_ms_bench_result();
  }
  return true;
}

bool eg2_rv_snoop_watch_begin(bool bench) { return eg2_ms_bench("snoop_watch", 1, 0, bench); }

bool eg2_rv_snoop_watch_end(uint32_t *hits, bool bench) {
  *hits = 0;
  if (!eg2_ms_bench("snoop_watch", 0, 0, bench)) {
    return false;
  }
  if (bench) {
    *hits = eg2_ms_bench_result();
  }
  return true;
}
