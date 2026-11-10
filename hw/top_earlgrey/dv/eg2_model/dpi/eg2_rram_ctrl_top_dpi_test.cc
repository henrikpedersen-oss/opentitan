// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the rram_ctrl top-level model (ip/eg2_rram_ctrl_top_core.h): each documented rule
// the model composes, checked on a small scenario, and no objection of a unit core to any step.
//   g++ -std=c++17 -O1 -Wall -I. dpi/eg2_rram_ctrl_top_dpi_test.cc -o /tmp/rram_top_selftest
//   /tmp/rram_top_selftest                         (must print "PASS ..." and exit 0)
//   EG2_RRAM_TOP_TEST_BREAK=1 /tmp/rram_top_selftest  (one expectation inverted: must exit 1)

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "ip/eg2_rram_ctrl_top_core.h"

using eg2_model::RramCtrlTop;

static int fails = 0, checks = 0;
static bool brk = std::getenv("EG2_RRAM_TOP_TEST_BREAK") != nullptr;
static void check(bool ok, const char *what) {
  ++checks;
  if (brk && checks == 5) ok = !ok;  // the red run: one expectation inverted
  if (!ok) {
    ++fails;
    std::printf("FAIL %s\n", what);
  }
}

static std::map<uint64_t, uint32_t> mem;
static std::map<uint64_t, bool> bad;  // words whose read is an uncorrectable ECC error
using Phys = RramCtrlTop::Phys;

static uint32_t ctl(unsigned op, unsigned part, unsigned num) {
  return 1u | (op << 4) | (part << 8) | (num << 16);
}

int main() {
  RramCtrlTop t;
  t.mem_rd = [](unsigned part, uint32_t a, uint32_t *w, const Phys &) {
    uint64_t k = (uint64_t(part) << 32) | a;
    if (bad.count(k)) return RramCtrlTop::kRdUncorr;
    auto it = mem.find(k);
    if (it == mem.end()) return RramCtrlTop::kRdUnknown;
    *w = it->second;
    return RramCtrlTop::kRdOk;
  };
  t.mem_wr = [](unsigned part, uint32_t a, uint32_t w, const Phys &, bool) {
    mem[(uint64_t(part) << 32) | a] = w;
  };

  // Before INIT: op_err ("rejected by the arbiter").
  RramCtrlTop::Ev e = t.start(ctl(0, 0, 3), 0x1000);
  check(e.done && e.err == RramCtrlTop::kOpErr && e.recov, "read before INIT: op_err, recov_err");
  check(t.rd_fifo.empty(), "read before INIT: nothing in the read FIFO");

  // INIT without provisioning: keys requested, no seed read, init_done.
  t.init(RramCtrlTop::kLcOff);
  check(t.init_done() && t.keys_valid(), "INIT: init_done, keys_valid");
  std::vector<uint8_t> sd;
  check(!t.seed(0, &sd) && !t.seed(1, &sd), "INIT without lc_seed_hw_rd_en: seeds invalid");

  // A region over pages 8-15 with RD, WR, SCR, ECC (MuBi4True = 6); default: nothing allowed.
  t.mp.region[0] = 8u | (7u << 12);
  t.mp.region_cfg[0] = 0x66666;
  uint32_t base = 8 * 512;
  e = t.start(ctl(1, 0, 7), base);
  check(!e.done && t.write_running() && !t.regwen, "write: running, CTRL_REGWEN low");
  for (uint32_t i = 0; i < 8; ++i) e = t.push(0x11110000u + i);
  check(e.done && e.err == 0 && t.regwen, "write of 8 words: done without error");
  e = t.start(ctl(0, 0, 7), base + 2);  // ADDR truncated to a bus word
  bool same = t.rd_fifo.size() == 8;
  for (uint32_t i = 0; same && i < 8; ++i) same = t.rd_fifo[i] == 0x11110000u + i && t.rd_known[i];
  check(e.done && e.err == 0 && same, "read back: the words written, in order");
  uint32_t w;
  while (t.pop(&w)) {}

  // Write alignment: rejected without access.
  size_t before = mem.size();
  e = t.start(ctl(1, 0, 5), base);
  check(e.done && e.err == RramCtrlTop::kOpErr && mem.size() == before, "write of 6 words: op_err, no access");
  e = t.start(ctl(1, 0, 7), base + 4);
  check(e.done && e.err == RramCtrlTop::kOpErr, "unaligned write: op_err");
  e = t.start(ctl(2, 0, 0), base + 8);
  check(e.done && e.err == RramCtrlTop::kOpErr, "unaligned Rewrite: op_err");

  // A read running from the region into page 16 (default region: denied): mp_err at the first
  // word of page 16, ERR_ADDR there, all ones from there on, the full count delivered.
  uint32_t a16 = 16 * 512 - 8;
  e = t.start(ctl(0, 0, 5), a16);
  check(e.done && e.err == RramCtrlTop::kMpErr, "read into a denied page: mp_err");
  check(t.err_addr == 16 * 512, "ERR_ADDR: the first failing word");
  check(t.rd_fifo.size() == 6 && t.rd_fifo[2] == 0xffffffffu && t.rd_fifo[5] == 0xffffffffu,
        "all ones from the failing word on, six words");
  t.fifo_clr(false, true);
  check(t.rd_fifo.empty(), "FIFO_CLR.RD empties the read FIFO");

  // The OTP pages: never reachable by software or the host.
  e = t.start(ctl(0, 0, 3), 4090 * 512);
  check(e.err == RramCtrlTop::kMpErr, "controller read of an OTP page: mp_err");
  t.fifo_clr(false, true);
  check(!t.host_read_ok(4090 * 512) && !t.host_read_ok(4095 * 512 + 508), "host read of an OTP page: error");
  check(t.host_read_ok(base) && !t.host_read_ok(16 * 512), "host read: the region's RD decides");

  // Rewrite: the RRAM word unchanged, nothing to software.
  e = t.start(ctl(2, 0, 0), base);
  check(e.done && e.err == 0 && t.rd_fifo.empty() && mem[base] == 0x11110000u, "Rewrite: done, data unchanged");
  t.mp.region_cfg[0] = 0x66606;  // RD only
  e = t.start(ctl(2, 0, 0), base);
  check(e.done && e.err == RramCtrlTop::kMpErr && t.err_addr == base, "Rewrite without WR: mp_err");
  t.mp.region_cfg[0] = 0x66666;

  // A write denied in its middle: the first denied word gives ERR_ADDR; nothing written there.
  t.mp.region[0] = 8u | (0u << 12);  // page 8 only
  e = t.start(ctl(1, 0, 255), 8 * 512 + 256);  // 256 words: page 8's second half, page 9 denied
  for (uint32_t i = 0; i < 256; ++i) e = t.push(0xa0000000u + i);
  check(e.done && e.err == RramCtrlTop::kMpErr && t.err_addr == 9 * 512, "write into a denied page: mp_err at its first word");
  check(mem.find(9 * 512) == mem.end(), "the denied word is not written");
  check((t.err_code & 0xf) == (RramCtrlTop::kOpErr | RramCtrlTop::kMpErr), "ERR_CODE gathers op_err and mp_err (rw1c)");

  // An uncorrectable word in the middle of a read: rd_err, ERR_ADDR there, all ones from it on.
  t.mp.region[0] = 8u | (7u << 12);
  bad[base + 8] = true;
  e = t.start(ctl(0, 0, 3), base);
  check(e.done && e.err == RramCtrlTop::kRdErr && t.err_addr == base + 8 &&
            t.rd_fifo.size() == 4 && t.rd_fifo[1] == 0x11110001u && t.rd_fifo[2] == 0xffffffffu,
        "read of an uncorrectable word: rd_err, ERR_ADDR, all ones from it on");
  t.fifo_clr(false, true);
  bad.clear();

  // The composition never broke a unit rule.
  for (const std::string &f : t.findings()) std::printf("finding: %s\n", f.c_str());
  check(t.findings().empty(), "no unit core objected to a step of the model");

  // A second model, provisioned: the seeds read twice, valid; disabled afterwards: invalid.
  RramCtrlTop p;
  p.mem_rd = t.mem_rd;
  p.mem_wr = t.mem_wr;
  for (uint32_t i = 0; i < 8; ++i) {
    mem[(1ull << 32) | (5 * 512 + 4 * i)] = 0xc0de0000u + i;
    mem[(1ull << 32) | (6 * 512 + 4 * i)] = 0x0e1e0000u + i;
  }
  p.init(RramCtrlTop::kLcOn);
  check(p.init_done() && p.seed(0, &sd) && sd[0] == 0x00 && sd[2] == 0xde && sd[3] == 0xc0, "provisioned INIT: creator seed valid, its bytes");
  check(p.seed(1, &sd) && sd[3] == 0x0e, "provisioned INIT: owner seed valid");
  p.dis = 0x96;  // SW_DIS any value but MuBi4False
  check(!p.seed(0, &sd) && p.disabled(), "after DIS.SW_DIS: seeds invalid, disabled");
  e = p.start(ctl(0, 0, 0), base);
  check(e.done && e.err == RramCtrlTop::kOpErr && !p.host_read_ok(base), "disabled: op_err (model choice), host read error");
  check(!p.rma([] { return uint8_t(0x5a); }), "RMA refused after a disable");

  // A third: RMA wipe from StWait, rma_ack; refused a second time; software op_err afterwards.
  RramCtrlTop r;
  std::map<uint64_t, uint32_t> m2;
  r.mem_rd = [&m2](unsigned part, uint32_t a, uint32_t *w, const Phys &) {
    auto it = m2.find((uint64_t(part) << 32) | a);
    if (it == m2.end()) return RramCtrlTop::kRdUnknown;
    *w = it->second;
    return RramCtrlTop::kRdOk;
  };
  r.mem_wr = [&m2](unsigned part, uint32_t a, uint32_t w, const Phys &, bool) {
    m2[(uint64_t(part) << 32) | a] = w;
  };
  r.init(RramCtrlTop::kLcOff);
  uint8_t x = 1;
  check(r.rma([&x] { x = uint8_t(x * 5 + 1); return x; }), "RMA from StWait: wipe complete, rma_ack");
  check(m2.size() == 4090u * 128 + 3 * 128 && m2.find(4090ull * 512) == m2.end(), "RMA wipes data pages 0-4089 and info 5-7, never OTP");
  check(!r.rma([] { return uint8_t(0); }), "RMA a second time: refused");
  e = r.start(ctl(0, 0, 0), 0);
  check(e.err == RramCtrlTop::kOpErr && r.rma_sw_dis(), "after RMA entry: software op_err (rma_sw_dis)");
  check(p.findings().empty() && r.findings().empty(), "no unit core objected (provisioned, RMA)");

  std::printf("%s %d/%d checks\n", fails ? "FAIL" : "PASS", checks - fails, checks);
  return fails ? 1 : 0;
}
