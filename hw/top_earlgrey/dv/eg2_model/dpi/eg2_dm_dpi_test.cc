// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the debug module model through its DPI functions (no simulator):
//   g++ -std=c++17 -I.. eg2_dm_dpi.cc eg2_dm_dpi_test.cc -o eg2_dm_dpi_test && ./eg2_dm_dpi_test
// EG2_DM_TEST_BREAK=1 inverts one expectation: the run must then fail.

#include <cstdio>
#include <cstdlib>

extern "C" {
void *eg2_dm_create();
void eg2_dm_release(void *h);
void eg2_dm_reset(void *h);
void eg2_dm_set_enables(void *h, int dmi, int full);
void eg2_dm_set_unavailable(void *h, int u);
void eg2_dm_set_cheriot(void *h, int c);
int eg2_dm_dmi_read(void *h, unsigned int addr, unsigned int *data, unsigned int *mask);
int eg2_dm_dmi_write(void *h, unsigned int addr, unsigned int data);
int eg2_dm_debug_req(void *h);
int eg2_dm_ndmreset(void *h);
void eg2_dm_mem_write(void *h, unsigned int off, unsigned int data, unsigned int be);
unsigned int eg2_dm_mem_read(void *h, unsigned int off, unsigned int *mask);
int eg2_dm_sba_take(void *h, int *write, unsigned int *addr, unsigned int *size,
                    unsigned int *wdata);
void eg2_dm_sba_complete(void *h, unsigned int rdata, int error);
}

namespace {

int failures = 0, checks = 0;

void check(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

unsigned rd(void *h, unsigned addr, unsigned *mask = nullptr) {
  unsigned v = 0, m = 0;
  eg2_dm_dmi_read(h, addr, &v, &m);
  if (mask) *mask = m;
  return v;
}

unsigned field(unsigned v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

}  // namespace

int main() {
  const bool brk = std::getenv("EG2_DM_TEST_BREAK") != nullptr;
  void *h = eg2_dm_create();
  eg2_dm_set_enables(h, 1, 1);

  // Activation: the first write sets only dmactive (haltreq in it is dropped).
  eg2_dm_dmi_write(h, 0x10, 0x80000001u);
  check(rd(h, 0x10) == 1u, "dmcontrol reads dmactive only after activation");
  check(!eg2_dm_debug_req(h), "no debug_req from the activating write");
  unsigned s = rd(h, 0x11);
  check(field(s, 3, 0) == 2 && field(s, 7, 7) == 1, "dmstatus version 2, authenticated");
  check(field(s, 19, 18) == 3, "dmstatus anyhavereset/allhavereset after reset");
  check(rd(h, 0x12) == ((2u << 20) | (1u << 16) | (2u << 12) | 0x380u), "hartinfo");
  check(rd(h, 0x16) == ((8u << 24) | 2u), "abstractcs progbufsize 8, datacount 2");

  // Halt: debug_req, then the core's write of Halted.
  eg2_dm_dmi_write(h, 0x10, 0x80000001u);
  check(eg2_dm_debug_req(h) == 1, "haltreq drives debug_req");
  eg2_dm_mem_write(h, 0x100, 0, 0xf);
  s = rd(h, 0x11);
  check(field(s, 9, 8) == 3 && field(s, 11, 10) == 0, "halted after the core writes Halted");
  eg2_dm_dmi_write(h, 0x10, 0x10000001u);  // haltreq 0, ackhavereset
  check(!eg2_dm_debug_req(h) && field(rd(h, 0x11), 19, 18) == 0, "haltreq 0, havereset acked");

  // An abstract command: busy, the go flag, the core's Going and Halted.
  eg2_dm_dmi_write(h, 0x04, 0x12345678u);
  eg2_dm_dmi_write(h, 0x17, (2u << 20) | (1u << 17) | (1u << 16) | 0x1008u);  // write x8
  unsigned m = 0;
  check(field(rd(h, 0x16), 12, 12) == 1, "abstractcs.busy while the command runs");
  check(eg2_dm_mem_read(h, 0x400, &m) == 1u && m == ~0u, "flags: go set for the core");
  check(eg2_dm_mem_read(h, 0x380, &m) == 0x12345678u, "the core reads data0 from the memory");
  eg2_dm_mem_read(h, 0x300, &m);
  check(m == 0, "WhereTo is not predicted");
  rd(h, 0x04, &m);
  check(m == 0, "data0 read while busy is not predicted");
  check(field(rd(h, 0x16), 10, 8) == 1, "cmderr busy after a data0 access while busy");
  eg2_dm_mem_write(h, 0x100, 0, 0xf);
  check(field(rd(h, 0x16), 12, 12) == 1 && eg2_dm_mem_read(h, 0x400, &m) == 1u,
        "Halted before Going: the command stays busy, go stays set");
  eg2_dm_mem_write(h, 0x108, 0, 0xf);
  check(eg2_dm_mem_read(h, 0x400, &m) == 0u, "Going clears go");
  eg2_dm_mem_write(h, 0x100, 0, 0xf);
  check(field(rd(h, 0x16), 12, 12) == 0, "busy clear once the core is back (Halted)");
  eg2_dm_dmi_write(h, 0x17, 0);
  check(field(rd(h, 0x16), 12, 12) == 0, "no command while cmderr is set");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);
  check(field(rd(h, 0x16), 10, 8) == 0, "cmderr W1C");

  // An exception in the command, then unsupported types and sizes.
  eg2_dm_dmi_write(h, 0x17, (2u << 20) | (1u << 17) | 0x1000u);
  eg2_dm_mem_write(h, 0x108, 0, 0xf);
  eg2_dm_mem_write(h, 0x118, 0, 0xf);
  eg2_dm_mem_write(h, 0x100, 0, 0xf);
  check(field(rd(h, 0x16), 10, 8) == 3, "cmderr exception from the core's Exception write");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);
  eg2_dm_dmi_write(h, 0x17, 1u << 24);
  check(field(rd(h, 0x16), 10, 8) == 2, "Quick Access: cmderr not supported");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);
  eg2_dm_dmi_write(h, 0x17, (3u << 20) | (1u << 17) | 0x1000u);
  check(field(rd(h, 0x16), 10, 8) == 2, "64-bit access: cmderr not supported");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);

  // CHERIoT mode (rv_dm "CHERIoT Mode"): aarsize 3 for a GPR or an SCR; other CSR numbers rejected.
  eg2_dm_set_cheriot(h, 1);
  eg2_dm_dmi_write(h, 0x17, (3u << 20) | (1u << 17) | 0x100au);  // read ca0 (x10) as a capability
  check(field(rd(h, 0x16), 12, 12) == 1 && field(rd(h, 0x16), 10, 8) == 0,
        "CHERIoT: a GPR capability read runs");
  eg2_dm_mem_write(h, 0x108, 0, 0xf);
  eg2_dm_mem_write(h, 0x380, 0x20000010u, 0xf);
  eg2_dm_mem_write(h, 0x384, 0x1f0a0003u, 0xf);
  eg2_dm_mem_write(h, 0x100, 0, 0xf);
  check(rd(h, 0x04) == 0x20000010u && rd(h, 0x05) == 0x1f0a0003u,
        "CHERIoT: address in data0, metadata in data1");
  eg2_dm_dmi_write(h, 0x17, (3u << 20) | (1u << 17) | 0x1cu);  // SCR 28 (mtcc)
  check(field(rd(h, 0x16), 10, 8) == 0, "CHERIoT: an SCR (regno 0x1c) is accepted");
  eg2_dm_mem_write(h, 0x108, 0, 0xf);
  eg2_dm_mem_write(h, 0x100, 0, 0xf);
  eg2_dm_dmi_write(h, 0x17, (3u << 20) | (1u << 17) | 0x7b1u);  // dpc as aarsize 3
  check(field(rd(h, 0x16), 10, 8) == 2, "CHERIoT: aarsize 3 of CSR 0x7b1 is not supported");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);
  eg2_dm_set_cheriot(h, 0);
  eg2_dm_dmi_write(h, 0x17, (3u << 20) | (1u << 17) | 0x100au);
  check(field(rd(h, 0x16), 10, 8) == 2, "RV32I mode: aarsize 3 is not supported");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);

  // Resume: the resume flag, the core's Resuming, resumeack.
  eg2_dm_dmi_write(h, 0x10, 0x40000001u);
  check(eg2_dm_mem_read(h, 0x400, &m) == 2u, "flags: resume set for the core");
  check(field(rd(h, 0x11), 17, 16) == 0, "no resumeack before the core resumes");
  eg2_dm_mem_write(h, 0x110, 0, 0xf);
  s = rd(h, 0x11);
  check(field(s, 17, 16) == 3 && field(s, 11, 10) == 3 && field(s, 9, 8) == 0,
        "running with resumeack after Resuming");
  eg2_dm_dmi_write(h, 0x17, (2u << 20) | (1u << 17) | 0x1000u);
  check(field(rd(h, 0x16), 10, 8) == 4, "a command on a running hart: cmderr halt/resume");
  eg2_dm_dmi_write(h, 0x16, 7u << 8);

  // System bus: a 32-bit read on address write, sbbusy until the response, sbbusyerror.
  eg2_dm_dmi_write(h, 0x38, (1u << 20) | (2u << 17) | (1u << 16));
  eg2_dm_dmi_write(h, 0x39, 0x10000000u);
  int w = 0;
  unsigned a = 0, sz = 0, wd = 0;
  check(eg2_dm_sba_take(h, &w, &a, &sz, &wd) == 1 && !w && a == 0x10000000u && sz == 4,
        "sbreadonaddr starts a 4-byte read");
  check(field(rd(h, 0x38), 21, 21) == 1, "sbbusy while the bus has not answered");
  eg2_dm_dmi_write(h, 0x39, 0x20000000u);
  check(field(rd(h, 0x38), 22, 22) == 1, "sbbusyerror: sbaddress0 written while sbbusy");
  eg2_dm_sba_complete(h, 0xcafef00du, 0);
  check(rd(h, 0x3c) == 0xcafef00du, "sbdata0 holds the read data");
  check(rd(h, 0x39) == 0x10000004u, "sbautoincrement after the access");
  eg2_dm_dmi_write(h, 0x38, (1u << 22) | (2u << 17));
  check(field(rd(h, 0x38), 22, 22) == 0, "sbbusyerror W1C");
  eg2_dm_dmi_write(h, 0x39, 0x10000002u);
  eg2_dm_dmi_write(h, 0x3c, 1);
  check(field(rd(h, 0x38), 14, 12) == 3 && !eg2_dm_sba_take(h, &w, &a, &sz, &wd),
        "a misaligned 32-bit access: sberror 3, no bus request");
  eg2_dm_dmi_write(h, 0x38, (7u << 12) | (0u << 17));
  eg2_dm_dmi_write(h, 0x39, 0x10000003u);
  eg2_dm_dmi_write(h, 0x3c, 0x1ffu);
  check(eg2_dm_sba_take(h, &w, &a, &sz, &wd) == 1 && w && sz == 1 && wd == 0xffu,
        "an 8-byte-lane write carries the low byte only");
  eg2_dm_sba_complete(h, 0, 1);
  check(field(rd(h, 0x38), 14, 12) == 2, "a bus error response: sberror 2");

  // ndmreset, and life cycle.
  eg2_dm_dmi_write(h, 0x10, 0x10000001u);  // ackhavereset first
  eg2_dm_dmi_write(h, 0x10, 0x3u);
  check(eg2_dm_ndmreset(h) == 1, "ndmreset_o follows dmcontrol.ndmreset");
  check(field(rd(h, 0x11), 19, 18) == 3, "ndmreset resets the hart: dmstatus havereset");
  eg2_dm_set_enables(h, 1, 0);
  check(eg2_dm_ndmreset(h) == 0, "ndmreset_o gated while not fully enabled");
  eg2_dm_set_enables(h, 0, 0);
  unsigned v = 1;
  check(eg2_dm_dmi_read(h, 0x11, &v, &m) == 0 && v == 0 && m == 0, "gated DMI read");
  eg2_dm_set_enables(h, 1, 1);
  eg2_dm_set_unavailable(h, 1);
  s = rd(h, 0x11);
  check(field(s, 13, 12) == 3 && field(s, 11, 10) == 0, "unavailable: anyunavail, not running");

  // dmactive 0 clears the module, the data and the program buffer.
  eg2_dm_dmi_write(h, 0x20, 0xabcdu);
  eg2_dm_dmi_write(h, 0x10, 0);
  eg2_dm_dmi_write(h, 0x10, 1);
  check(rd(h, 0x20) == 0 && rd(h, 0x38) == ((1u << 29) | (2u << 17) | (32u << 5) | 7u),
        "dmactive 0 resets the program buffer and sbcs");
  check((rd(h, 0x10) == 1u) != brk, "dmcontrol after re-activation");

  eg2_dm_release(h);
  std::printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
