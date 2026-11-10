// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of cms_core.h: the documented behaviour, scenario by scenario, each followed by a wrong
// response the model must flag (a checker that cannot fail proves nothing). make test.

#include <cstdio>
#include <cstdlib>

#include "cms_core.h"

using namespace cms;

static int failures = 0;
#define EXPECT(cond, what)                                    \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); \
      failures++;                                             \
    }                                                         \
  } while (0)

static CoreTxn txn(uint8_t op, uint32_t addr, uint32_t wdata, bool tag, uint8_t ena = kMuBi4True) {
  CoreTxn t{};
  t.op = op; t.addr = addr; t.size = 2; t.mask = 0xf; t.wdata = wdata; t.tag = tag; t.ena = ena;
  return t;
}
// A capability with its base at `base` (exponent 0, base mantissa from the base), permissions p.
static uint32_t cap_meta(uint32_t base, uint32_t perms) { return perms << 25 | (base & 0x1ff); }
static const uint32_t kPermsRw = 0x1f, kPermsSealing = 0x07;

static void store_cap(Model &m, uint32_t a, uint32_t w0, uint32_t w1, bool tag) {
  CoreTxn t = txn(kPutFull, a, w0, tag);
  EXPECT(m.core(t) == 0, "cap store W0");
  t = txn(kPutFull, a + 4, w1, tag);
  EXPECT(m.core(t) == 0, "cap store W1");
}
// A capability load: returns the number of mismatches of both words given the tag the DUT returned.
static int load_cap(Model &m, uint32_t a, bool rtag, uint8_t ena = kMuBi4True) {
  // Mask 0: the data is not compared here (test_data does that), only the error and the tag.
  CoreTxn t0 = txn(kGet, a, 0, true, ena), t1 = txn(kGet, a + 4, 0, true, ena);
  t0.mask = t1.mask = 0;
  t0.rtag = t1.rtag = rtag;
  return m.core(t0) + m.core(t1);
}

static void test_tag_store_load() {
  Model m(Map::cms_small());
  uint32_t a = 0x10000100;
  store_cap(m, a, 0x10000200, cap_meta(0x10000200, kPermsRw), true);
  EXPECT(m.tag(a) == kTag1, "tag set by a capability store (REQ_TAG_02)");
  EXPECT(load_cap(m, a, true) == 0, "capability load returns tag 1 (REQ_TAG_01)");
  EXPECT(load_cap(m, a, false) > 0, "must fail: tag 0 where 1 was stored");
  // A plain byte store clears the tag (REQ_TAG_02, A5).
  CoreTxn b = txn(kPutPartial, a + 1, 0xff00, false);
  b.size = 0; b.mask = 0x2;
  EXPECT(m.core(b) == 0, "byte store");
  EXPECT(m.tag(a) == kTag0, "byte store clears the tag");
  EXPECT(load_cap(m, a, true) > 0, "must fail: tag 1 after a clearing store");
}

static void test_data() {
  Model m(Map::cms_small());
  CoreTxn w = txn(kPutFull, 0x10000040, 0xdeadbeef, false);
  EXPECT(m.core(w) == 0, "data store");
  CoreTxn r = txn(kGet, 0x10000040, 0, false);
  r.rdata = 0xdeadbeef;
  EXPECT(m.core(r) == 0, "data read back (REQ_TF_07)");
  r.rdata = 0xdeadbeee;
  EXPECT(m.core(r) == 1, "must fail: wrong data");
  CoreTxn e = txn(kGet, 0x20000000, 0, false);
  e.host_err = true; e.err = true;
  EXPECT(m.core(e) == 0, "the host's error passes (REQ_TF_12a)");
  e.err = false;
  EXPECT(m.core(e) == 1, "must fail: host error dropped");
}

static void test_epmp_mode() {
  Model m(Map::cms_small());
  uint32_t a = 0x10000300;
  store_cap(m, a, 0x10000200, cap_meta(0x10000200, kPermsRw), true);
  // In ePMP mode a capability load returns tag 0 and the tag store is not touched (REQ_CMS_06).
  EXPECT(load_cap(m, a, false, 0x9) == 0, "ePMP load: tag 0");
  EXPECT(load_cap(m, a, true, 0x9) > 0, "must fail: tag 1 in ePMP mode");
  CoreTxn w = txn(kPutFull, a, 1, false, 0x9);
  m.core(w);
  EXPECT(m.tag(a) == kTag1, "an ePMP store leaves the tag store alone");
}

static void test_nvm_cap_store() {
  Model m(Map::cms_small());
  uint32_t a = 0x30000040, w0 = 0x10000200, w1 = cap_meta(0x10000200, kPermsRw);
  m.backdoor_data(a, w0);
  m.backdoor_data(a + 4, w1);
  store_cap(m, a, w0, w1, true);  // match: W0 and W1 accepted
  EXPECT(m.tag(a) == kTag1, "verified capability store to the NVM sets the tag (REQ_WTRC_06)");
  EXPECT(m.alerts_expected() == 0, "no alert");
  // A mismatch: W0 matches, W1 does not: W1 errs, no tag, no alert (REQ_WTRC_08, 09).
  uint32_t b = 0x30000080;
  m.backdoor_data(b, w0);
  m.backdoor_data(b + 4, w1);
  CoreTxn t0 = txn(kPutFull, b, w0, true);
  EXPECT(m.core(t0) == 0, "W0 matches");
  CoreTxn t1 = txn(kPutFull, b + 4, w1 ^ 1, true);
  t1.err = true;
  EXPECT(m.core(t1) == 0, "W1 mismatch answered with an error");
  EXPECT(m.tag(b) == kTag0, "no tag after a mismatch");
  EXPECT(m.alerts_expected() == 0, "no alert for a mismatch");
  // Must fail: a mismatching W1 answered without error.
  CoreTxn t2 = txn(kPutFull, b, w0, true);
  m.core(t2);
  CoreTxn t3 = txn(kPutFull, b + 4, w1 ^ 1, true);
  EXPECT(m.core(t3) == 1, "must fail: mismatch accepted");
  // A partial word raises fatal_fault (REQ_WTRC_11).
  CoreTxn p = txn(kPutPartial, b, w0, true);
  p.mask = 0x3; p.err = true;
  m.core(p);
  EXPECT(m.alerts_expected() == 1, "partial capability store word: alert");
}

static void test_bitmap_window() {
  Model m(Map::cms_small());
  EXPECT(m.bitmap(false, kPutFull, 0x8, 2, 0xf, 0x5, kMuBi4True, false, 0) == 0, "bitmap write");
  EXPECT(m.bitmap(true, kGet, 0x8, 2, 0xf, 0, kMuBi4True, false, 0x5) == 0, "bitmap read by the core's TRVK");
  EXPECT(m.bitmap(false, kGet, 0x8, 2, 0xf, 0, 0x9, true, 0) == 0, "ePMP: error (REQ_META_04)");
  EXPECT(m.bitmap(false, kPutPartial, 0x8, 0, 0x1, 0, kMuBi4True, true, 0) == 0, "partial: error");
  EXPECT(m.bitmap(false, kGet, m.map().revbm_bytes(), 2, 0xf, 0, kMuBi4True, true, 0) == 0,
         "outside the bitmap: error (REQ_META_03)");
  EXPECT(m.bitmap(false, kGet, 0x8, 2, 0xf, 0, kMuBi4True, false, 0x4) == 1, "must fail: wrong word");
  EXPECT(m.bitmap(false, kGet, 0x8, 2, 0xf, 0, 0x9, false, 0) == 1, "must fail: no error in ePMP");
}

static void test_sweep() {
  Model m(Map::cms_small());
  uint32_t heap = 0x10000400, slot = 0x10000800, sealed = 0x10000808, kept = 0x10000810;
  store_cap(m, slot, heap, cap_meta(heap, kPermsRw), true);
  store_cap(m, sealed, heap, cap_meta(heap, kPermsSealing), true);
  store_cap(m, kept, 0x10000600, cap_meta(0x10000600, kPermsRw), true);
  uint32_t bit = (heap - 0x10000000) >> 3;
  EXPECT(m.bitmap(false, kPutFull, (bit / 32) * 4, 2, 0xf, 1u << (bit % 32), kMuBi4True, false, 0) == 0, "mark");
  m.csr(true, kTbreBase, slot, 0);
  m.csr(true, kTbreNum, 3, 0);
  m.csr(true, kIntrEnable, 1, 0);
  m.csr(true, kTbreStart, 1, 0);
  EXPECT(m.csr(false, kTbreRegwen, 0, 0) == 0, "REGWEN 0 while active (REQ_TBRE_07)");
  EXPECT(m.csr(false, kTbreEpoch, 0, 1) == 0, "epoch active (REQ_TBRE_09)");
  m.csr(true, kTbreNum, 99, 0);  // ignored while active (REQ_TBRE_05)
  m.sweep_end();
  EXPECT(m.tag(slot) == kTag0, "revoked capability cleared (REQ_TBRE_19)");
  EXPECT(m.tag(sealed) == kTag1, "sealing capability kept");
  EXPECT(m.tag(kept) == kTag1, "unrevoked capability kept (REQ_TBRE_20)");
  EXPECT(m.csr(false, kTbreNum, 0, 3) == 0, "NUM_CAPS kept its value");
  EXPECT(m.csr(false, kTbreEpoch, 0, 2) == 0, "epoch count 1 (REQ_TBRE_10)");
  EXPECT(m.intr_expected(), "tbre_done raised and enabled (REQ_TBRE_28, 31)");
  EXPECT(m.csr(false, kTbreEpoch, 0, 4) == 1, "must fail: wrong epoch");
  // An ignored start: NUM_CAPS 0 (REQ_TBRE_03).
  m.csr(true, kTbreNum, 0, 0);
  m.csr(true, kTbreStart, 1, 0);
  EXPECT(m.csr(false, kTbreStatus, 0, 1u << 8) == 0, "start_err");
  EXPECT(!m.sweep_active(), "no sweep");
  // The meta SRAM image: the cleared tag.
  uint32_t v, c;
  m.meta_word(m.map().tag_word_off(m.map().granule(slot)), v, c);
  EXPECT(!(v >> m.map().tag_bit(m.map().granule(slot)) & 1), "meta word has the tag cleared");
}

static void test_sweep_watch() {
  Model m(Map::cms_small());
  uint32_t heap = 0x10000400, slot = 0x10000800;
  store_cap(m, slot, heap, cap_meta(heap, kPermsRw), true);
  uint32_t bit = (heap - 0x10000000) >> 3;
  m.bitmap(false, kPutFull, (bit / 32) * 4, 2, 0xf, 1u << (bit % 32), kMuBi4True, false, 0);
  m.csr(true, kTbreBase, slot, 0);
  m.csr(true, kTbreNum, 1, 0);
  m.csr(true, kTbreStart, 1, 0);
  // The core rewrites the capability during the sweep: the sweep must not clear it (REQ_TBRE_24, 39).
  store_cap(m, slot, 0x10000600, cap_meta(0x10000600, kPermsRw), true);
  m.sweep_end();
  EXPECT(m.tag(slot) == kTag1, "a capability the core rewrote keeps its tag");
}

static void test_reset_mid_sweep() {
  Model m(Map::cms_small());
  uint32_t heap = 0x10000400, slot = 0x10000800, sealed = 0x10000808;
  store_cap(m, slot, heap, cap_meta(heap, kPermsRw), true);
  store_cap(m, sealed, heap, cap_meta(heap, kPermsSealing), true);
  uint32_t bit = (heap - 0x10000000) >> 3;
  m.bitmap(false, kPutFull, (bit / 32) * 4, 2, 0xf, 1u << (bit % 32), kMuBi4True, false, 0);
  m.csr(true, kTbreBase, slot, 0);
  m.csr(true, kTbreNum, 2, 0);
  m.csr(true, kTbreStart, 1, 0);
  // A reset cuts the sweep short: the revoked capability's tag may or may not be cleared.
  m.dut_reset();
  EXPECT(!m.sweep_active(), "no sweep after a reset");
  EXPECT(m.tag(slot) == kTagAny, "a tag the cut sweep may have cleared is open (REQ_CMS_15)");
  EXPECT(m.tag(sealed) == kTag1, "a sealing capability keeps its tag");
  EXPECT(load_cap(m, slot, false) == 0 && load_cap(m, slot, true) == 0 && m.tag(slot) == kTagAny,
         "either tag may be loaded, and the tag stays open: the model takes no value from the DUT");
  EXPECT(load_cap(m, sealed, false) > 0, "must fail: tag 0 for the sealing capability");
}

static void test_sticky_tag() {
  Model m(Map::cms_small());
  uint32_t a = 0x10000100;
  store_cap(m, a, 0x10000200, cap_meta(0x10000200, kPermsRw), true);
  CoreTxn w0 = txn(kGet, a, 0, true);
  w0.mask = 0; w0.rtag = true;
  EXPECT(m.core(w0) == 0, "capability W0 with tag 1");
  // The next unaligned read, at any address and not a capability's W1, presents the captured tag.
  CoreTxn h = txn(kGet, 0x3000007c, 0, false);
  h.mask = 0; h.rtag = true;
  EXPECT(m.core(h) == 0, "the captured tag on the next unaligned read (theory_of_operation)");
  h.rtag = true;
  EXPECT(m.core(h) == 1, "must fail: the captured tag is cleared after one unaligned response");
}

int main() {
  test_tag_store_load();
  test_data();
  test_epmp_mode();
  test_nvm_cap_store();
  test_bitmap_window();
  test_sweep();
  test_sweep_watch();
  test_reset_mid_sweep();
  test_sticky_tag();
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
