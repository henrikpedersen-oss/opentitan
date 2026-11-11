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
  EXPECT(load_cap(m, slot, false) == 0 && load_cap(m, slot, false) == 0 && m.tag(slot) == kTagAny,
         "an open tag remains stable without resolving the independent prediction");
  EXPECT(load_cap(m, slot, true) > 0,
         "must fail: an open tag changed without any mutation between reads");
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

// Open tags still obey the documented relations. Exercise both initial choices, mutations,
// intervening writes, and the split predict/check/update interface used by the UVM bench.
static void test_open_tag_relations() {
  constexpr uint32_t a = 0x10000100;
  for (bool initial : {false, true}) {
    Model m(Map::cms_small());
    m.open_tag(a);
    CoreReq w0{kGet, a, 2, 0, 0, true, kMuBi4True, false, false};
    CoreReq w1 = w0; w1.addr += 4;
    EXPECT(m.predict_core(w0).rtag == kTagAny, "open W0 allows either value independently");
    EXPECT(m.check_core_tag(w0, false, initial) == 0, "first observation permits either tag");
    m.update_core(w0);
    EXPECT(m.predict_core(w1).rtag == kTagAny, "observation does not resolve predicted capture");
    EXPECT(m.check_core_tag(w1, false, !initial) > 0,
           "must fail: W1 differs from W0 although each is individually allowed");
    m.update_core(w1);
    EXPECT(m.check_core_tag(w0, false, initial) == 0, "repeated open read stays stable");
    m.update_core(w0);
    EXPECT(m.check_core_tag(w0, false, !initial) > 0,
           "must fail: repeated open read changes without a mutation");
    // Starting a new uncertain interval permits a different memory value, but does not change
    // the already captured response. An intervening write must not change that capture either.
    m.open_tag(a);
    CoreReq store{kPutFull, a + 8, 2, 15, 0, false, kMuBi4True, false, false};
    m.check_core_tag(store, false, false); m.update_core(store);
    EXPECT(m.check_core_tag(w1, false, !initial) == 0, "write preserves last captured response");
    m.update_core(w1);
    EXPECT(m.check_core_tag(w0, false, !initial) == 0, "possible mutation starts a new interval");
    m.update_core(w0);
    EXPECT(m.tag(a) == kTagAny, "comparison witnesses never change model memory");
    m.dut_reset();
    EXPECT(m.check_core_tag(w1, false, initial) == 0, "reset capture value is undocumented");
  }
}

static void test_refused_tag_write() {
  constexpr uint32_t a = 0x10000100;
  for (bool old_tag : {false, true}) {
    Model m(Map::cms_small());
    m.backdoor_tag(a, old_tag);
    CoreReq store{kPutFull, a, 2, 15, 0x12345678, !old_tag, kMuBi4True, false, true};
    EXPECT(m.predict_core(store).err, "tag-path d_error is returned to the core");
    m.update_core(store);
    EXPECT(m.tag(a) == tag_exact(old_tag), "refused tag write preserves the old tag exactly");
    CoreReq read{kGet, a, 2, 15, 0, true, kMuBi4True, false, false};
    EXPECT(m.predict_core(read).rdata == 0x12345678, "independent successful data write still applies");
  }
  Model m(Map::cms_small());
  m.open_tag(a);
  CoreReq read{kGet, a, 2, 0, 0, true, kMuBi4True, false, false};
  m.check_core_tag(read, false, false); m.update_core(read);
  CoreReq refused{kPutFull, a, 2, 15, 0, true, kMuBi4True, false, true};
  m.update_core(refused);
  EXPECT(m.check_core_tag(read, false, true) > 0,
         "must fail: a refused tag write cannot excuse a change in an open stored tag");
}

// External ports cannot tell whether the core or sweep received a shared-word fault.
// Both outcomes remain valid; unrelated counter/tag values must still be rejected.
static void test_blackbox_fault_alternatives() {
  Model m(Map::cms_small());
  const uint32_t a = Map::cms_small().sram_base + 0x400;
  m.backdoor_tag(a, true);
  m.set_mode(kMuBi4True);
  m.update_csr_write(kTbreBase, a);
  m.update_csr_write(kTbreNum, 1);
  m.update_csr_write(kTbreStart, 1);
  uint32_t off = Map::cms_small().tag_word_off(Map::cms_small().granule(a));
  EXPECT(m.sweep_may_access_meta(off), "programmed sweep can access the shared tag word");
  EXPECT(!m.sweep_may_access_meta(off + 4), "unrelated tag word is not a sweep candidate");
  m.sweep_meta_fault(off, true);
  m.update_csr_write(kTbreStatus, 1u << 9);
  EXPECT(m.csr_read_allowed(kTbreStatus, 1) && m.csr_read_allowed(kTbreStatus, 1 | (1u << 9)),
         "clear during ambiguous sweep retains both status outcomes");
  m.sweep_end();
  EXPECT(m.tag(a) == kTagAny, "ambiguous sweep tag outcome remains either");
  EXPECT(m.csr_read_allowed(kTbreStatus, 0), "sweep fault may belong to core");
  EXPECT(m.csr_read_allowed(kTbreStatus, 1u << 9), "sweep fault may belong to sweep");
  EXPECT(!m.csr_read_allowed(kTbreStatus, 1u << 8), "unrelated start error still rejected");
  EXPECT(m.csr_read_allowed(kTbreEpoch, 0), "failed sweep may keep epoch");
  EXPECT(m.csr_read_allowed(kTbreEpoch, 2), "successful sweep may advance epoch");
  EXPECT(!m.csr_read_allowed(kTbreEpoch, 4), "epoch cannot advance twice");
  m.update_csr_write(kTbreStatus, 1u << 9);
  EXPECT(!m.csr_read_allowed(kTbreStatus, 1u << 9), "explicit clear closes sweep_err alternatives");
  CoreReq unchanged{kPutFull, a, 2, 15, 0, true, kMuBi4True, false, false};
  m.backdoor_tag(a, true);
  m.update_core_possible_fault(unchanged);
  EXPECT(m.tag(a) == kTag1, "identical possible write outcomes remain exact");
  unchanged.tag = false;
  m.update_core_possible_fault(unchanged);
  EXPECT(m.tag(a) == kTagAny, "possibly refused write joins old and new tags");
  m.open_bitmap_write(0, 0x00010001);
  BitmapExp b = m.predict_bitmap(kGet, 0, 2, 15, kMuBi4True);
  EXPECT(b.rdata_care == ~0x00010001u, "ambiguous bitmap write opens only changed bits");
  m.update_bitmap(kPutFull, 0, 2, 15, 0, kMuBi4True);
  EXPECT(m.predict_bitmap(kGet, 0, 2, 15, kMuBi4True).rdata_care == 0xffffffffu,
         "definite bitmap write closes uncertainty");
  CoreReq load{kGet, a, 2, 15, 0, false, kMuBi4True, false, true};
  EXPECT(!m.predict_core(load).err, "unhinted load does not access tags");
  load.tag = true; load.addr += 4;
  EXPECT(!m.predict_core(load).err, "unaligned hinted load does not access tags");
}

int main() {
  test_blackbox_fault_alternatives();
  test_tag_store_load();
  test_data();
  test_epmp_mode();
  test_nvm_cap_store();
  test_bitmap_window();
  test_sweep();
  test_sweep_watch();
  test_reset_mid_sweep();
  test_sticky_tag();
  test_open_tag_relations();
  test_refused_tag_write();
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
