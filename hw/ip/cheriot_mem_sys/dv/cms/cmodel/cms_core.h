// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// cms_core.h: a reference model of the CHERIoT memory subsystem (OpenTitan cheriot_mem_sys) for its
// testbenches: the OpenTitan UVM bench (through cms_dpi.cc and cms_cmodel_pkg.sv), the cms bench
// (cheriot-mem-ss) and the EG2 system model.
//
// Written from the documents, never from the RTL (cheriot_mem_sys_verification_spec.md, "Operating
// principles"):
//   [ToO]   hw/ip/cheriot_mem_sys/doc/theory_of_operation.md
//   [PG]    doc/programmers_guide.md      [REGS] doc/registers.md, data/cheriot_mem_sys.hjson
//   [ISA]   CHERIoT ISA v1.0 (capability encoding, sealing)
//   REQ_*   the detailed requirements of the subsystem's verification specification; where the
//           documents are silent the model follows a reading, which the comment names (Q2, ...)
//
// The model is self-contained: it never takes a value from the DUT. For each request a monitor
// observes, predict_*() gives the response the documents allow (an error, data in the bits the
// model is sure of, the allowed tag values) and update_*() advances the model's state; the bench's
// scoreboard compares. Where a race leaves an outcome open (a sweep and a core store to the same
// capability, the bitmap changed during a sweep), a tag is a set of allowed values (TagSet). The
// model has no clock: it learns that a sweep has ended as software does, from a register read
// (sync_sweep_end). The check-style calls (core(), bitmap(), csr()) predict, compare and update in
// one call, for the cms bench.
//
// Not modelled (the RTL does not implement them; the bench's known gaps): the MUBI-encoded tag
// sideband (REQ_CMS_01) and the lockstep copy (REQ_CMS_02). Cycle timing (REQ_CMS_07, the "add no
// cycle" rows) is the bench's to check, not this transaction-level model's.

#ifndef CMS_CORE_H_
#define CMS_CORE_H_

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cms {

// TL-UL A-channel opcodes.
enum Op : uint8_t { kPutFull = 0, kPutPartial = 1, kGet = 4 };
constexpr uint8_t kMuBi4True = 0x6;

// The address map: cheriot_mem_sys's address-map parameters. [ToO] "Meta SRAM Address Map": the
// revocation bitmap at the base of the meta SRAM, then the NVM tags, then the main SRAM tags; one
// bit per 8 bytes, 32 bits per word (one word per 256 bytes).
struct Map {
  uint32_t sram_base, sram_top, nvm_base, nvm_top, meta_base;
  // Earl Grey: 192 KiB main SRAM (main and secondary, contiguous), 2 MiB NVM, meta SRAM at
  // 0x1100_0000. Gives bitmap 0x0000 (3 KiB), NVM tags 0x0C00 (32 KiB), SRAM tags 0x8C00 (3 KiB).
  static Map earlgrey() { return {0x10000000, 0x10030000, 0x30000000, 0x30200000, 0x11000000}; }
  // The cms bench's default small map: 16 KiB SRAM, 8 KiB NVM.
  static Map cms_small() { return {0x10000000, 0x10004000, 0x30000000, 0x30002000, 0x11000000}; }
  uint32_t sram_bytes() const { return sram_top - sram_base; }
  uint32_t nvm_bytes() const { return nvm_top - nvm_base; }
  uint32_t revbm_bytes() const { return sram_bytes() / 64; }
  uint32_t nvm_tag_off() const { return revbm_bytes(); }
  uint32_t sram_tag_off() const { return nvm_tag_off() + nvm_bytes() / 64; }
  uint32_t meta_top_off() const { return sram_tag_off() + sram_bytes() / 64; }
  bool in_sram(uint32_t a) const { return a >= sram_base && a < sram_top; }
  bool in_nvm(uint32_t a) const { return a >= nvm_base && a < nvm_top; }
  bool tagged(uint32_t a) const { return in_sram(a) || in_nvm(a); }
  uint32_t sram_caps() const { return sram_bytes() / 8; }
  // Granule: the 8-byte capability slot, numbered over the SRAM, then the NVM.
  uint32_t granule(uint32_t a) const {
    return in_sram(a) ? (a - sram_base) >> 3 : sram_caps() + ((a - nvm_base) >> 3);
  }
  uint32_t granule_addr(uint32_t g) const {
    return g < sram_caps() ? sram_base + g * 8 : nvm_base + (g - sram_caps()) * 8;
  }
  // Where a granule's tag is: meta word offset and bit. The bit order is the bitmap's (reading Q2).
  uint32_t tag_word_off(uint32_t g) const {
    return g < sram_caps() ? sram_tag_off() + (g / 32) * 4 : nvm_tag_off() + ((g - sram_caps()) / 32) * 4;
  }
  unsigned tag_bit(uint32_t g) const { return g < sram_caps() ? g % 32 : (g - sram_caps()) % 32; }
};

// A tag that may still be open: bit 0 "may be 0", bit 1 "may be 1".
using TagSet = uint8_t;
constexpr TagSet kTag0 = 1, kTag1 = 2, kTagAny = 3;
inline TagSet tag_exact(bool t) { return t ? kTag1 : kTag0; }

// [ISA] capability bounds: word 1 holds R 31, p 30:25, otype 24:22, E 21:18 (15 meaning 24),
// T 17:9, B 8:0; base = ((a >> (E+9)) + c_b) << (E+9) | B << E, c_b = -1 when a[E+8:E] < B.
inline uint32_t cap_base(uint32_t addr, uint32_t meta) {
  unsigned ce = (meta >> 18) & 0xf, e = ce == 15 ? 24 : ce;
  uint64_t b = meta & 0x1ff, mid = (uint64_t(addr) >> e) & 0x1ff, top = uint64_t(addr) >> (e + 9);
  if (mid < b) top -= 1;
  return uint32_t((top << (e + 9)) | (b << e));
}
// [ISA] a sealing capability: compressed permissions p[4:3] == 0 with any of p[2:0]. A capability
// with no permissions is not sealing (reading Q17).
inline bool cap_sealing(uint32_t meta) {
  uint32_t p = (meta >> 25) & 0x3f;
  return ((p >> 3) & 3) == 0 && (p & 7) != 0;
}

// Register offsets [REGS].
enum Csr : uint32_t {
  kIntrState = 0x00, kIntrEnable = 0x04, kIntrTest = 0x08, kAlertTest = 0x0c, kTbreRegwen = 0x10,
  kTbreBase = 0x14, kTbreNum = 0x18, kTbreStart = 0x1c, kTbreStatus = 0x20, kTbreEpoch = 0x24
};

// A request on the core data port, and what the subsystem's host side answered for it.
struct CoreReq {
  uint8_t op;          // kGet, kPutFull, kPutPartial
  uint32_t addr;
  uint8_t size;        // 0, 1, 2
  uint8_t mask;
  uint32_t wdata;
  bool tag;            // cored_tag_h2d: a store's tag, a load's capability hint
  uint8_t ena;         // cheriot_ena_i
  bool host_err;       // the crossbar answered d_error
  bool meta_err;       // the meta SRAM answered an error or an integrity fault for its tag access
};

// The response the model expects: the error, the data in the bytes it is sure of (rdata_care),
// and the tag values allowed.
struct CoreExp {
  bool err;
  uint32_t rdata, rdata_care;
  TagSet rtag;
};

// The response to a bitmap window access, and to a register read: the value in the bits the model
// is sure of.
struct BitmapExp {
  bool err;
  uint32_t rdata, rdata_care;
};
struct CsrExp {
  uint32_t rdata, rdata_care;
};

// One transaction on the core data port, with what the bench observed.
struct CoreTxn {
  uint8_t op;          // kGet, kPutFull, kPutPartial
  uint32_t addr;
  uint8_t size;        // 0, 1, 2
  uint8_t mask;
  uint32_t wdata;
  bool tag;            // cored_tag_h2d: a store's tag, a load's capability hint
  uint8_t ena;         // cheriot_ena_i
  bool host_err;       // xbar_main answered d_error (on the WTRC's read for a capability store to the NVM)
  bool meta_err;       // the meta path answered an error (a fault the bench injected)
  // the response
  bool err;
  uint32_t rdata;
  bool rtag;
};

class Model {
 public:
  explicit Model(const Map &m = Map::earlgrey()) : m_(m) { reset(); }

  void reset() {
    tags_.clear();
    data_.clear();
    revbm_.assign(m_.revbm_bytes() / 4, 0);
    intr_state_ = intr_enable_ = false;
    alert_test_regwen_ = true;
    base_ = num_ = 0;
    start_err_ = sweep_err_ = sweep_err_open_ = sweep_err_clr_ = false;
    epoch_ = 0;
    active_ = false;
    wtrc_open_ = false;
    captured_known_ = true; captured_ = false;
    alerts_ = 0;
    alert_tests_ = 0;
    fatal_ = false;
    msgs_.clear();
  }

  // The subsystem's reset: registers, the sweep, the WTRC and the captured tag back to their reset
  // values; the memories (the meta SRAM and the data behind the crossbar) keep their contents.
  void dut_reset() {
    // A sweep cut short: each tag it would have cleared may or may not be cleared yet.
    if (active_) settle_sweep(true);
    fatal_ = false;                                 // latched until reset
    intr_state_ = intr_enable_ = false;
    alert_test_regwen_ = true;
    base_ = num_ = 0;
    start_err_ = sweep_err_ = sweep_err_open_ = sweep_err_clr_ = false;
    epoch_ = 0;
    active_ = false;
    wtrc_open_ = false;
    captured_known_ = false;  // the reset value of the captured tag is not documented
    watched_.clear(); skipped_.clear(); forced_revoke_.clear(); revbm_changed_.clear();
    dont_care_.clear();
  }
  // A tag the bench cannot know (a write in flight at a reset may or may not have reached the meta
  // SRAM): either value, until a load resolves it.
  void open_tag(uint32_t addr) {
    if (m_.tagged(addr)) tags_[m_.granule(addr)] = kTagAny;
  }

  // ---- What the bench sets up by backdoor, or the model learns from writes it sees ----
  void backdoor_data(uint32_t addr, uint32_t word) { data_[addr & ~3u] = word; }
  void backdoor_tag(uint32_t addr, bool t) {
    if (m_.tagged(addr)) tags_[m_.granule(addr)] = tag_exact(t);
  }
  void backdoor_revbm(uint32_t word_idx, uint32_t v) {
    if (word_idx < revbm_.size()) revbm_[word_idx] = v;
  }

  // ---- The core data port: the tag filter, the WTRC, the mode ([ToO] "Tag Filter", "WTRC") ----
  //
  // predict_core: the response the documents allow for a request, from the model's own state.
  // update_core: the model's state after the access, again from its own state alone. Where the
  // documents leave an outcome open (a race with a sweep), the model keeps both values allowed;
  // it never takes a value from the DUT.
  CoreExp predict_core(const CoreReq &r) const {
    CoreExp e{};
    bool wr = r.op != kGet, cheriot = r.ena == kMuBi4True, tagged = m_.tagged(r.addr);
    bool lookup = cheriot && tagged;                               // REQ_TF_02, 03, 04
    bool nvm_cap = wr && cheriot && m_.in_nvm(r.addr) && r.tag;    // REQ_WTRC: to the NVM
    uint32_t g = tagged ? m_.granule(r.addr) : 0;

    // The error: the host's answer, or the tag answer's (REQ_TF_12a, 12b); for a capability store
    // to the NVM, the WTRC's verdict (REQ_WTRC_04, 07, 08, 10).
    e.err = r.host_err || (lookup && r.meta_err);
    if (nvm_cap) e.err = !wtrc_step(r, g).ok || r.meta_err;
    // A write's answer is untagged (REQ_TF_11a). A failed read's data and tag are not documented.
    if (wr) { e.rtag = kTag0; return e; }
    if (e.err) { e.rtag = kTagAny; return e; }

    // A read: the host's data, unchanged (REQ_TF_07).
    e.rdata = get_data(r.addr);
    for (int b = 0; b < 4; b++) if (r.mask >> b & 1) e.rdata_care |= 0xffu << (8 * b);
    // The tag of a read (REQ_TF_08 to 11c).
    bool aligned = !(r.addr & 4);
    if (aligned && r.tag && lookup && r.size == 2) {
      e.rtag = tag_of(g);
      // While a sweep runs over the granule, a capability it may revoke reads either way: the
      // engine may or may not have cleared it yet (REQ_TBRE_19, 24).
      if (active_ && in_sweep(g) && (e.rtag & kTag1) && may_revoke(g)) e.rtag |= kTag0;
    } else if (aligned) {
      e.rtag = kTag0;
    } else {
      // The sticky tag (REQ_TF_09): an unaligned read response presents the tag captured from the
      // last aligned one, whatever the read and its address.
      e.rtag = captured_known_ ? tag_exact(captured_) : kTagAny;
    }
    return e;
  }

  void update_core(const CoreReq &r) {
    bool wr = r.op != kGet, cheriot = r.ena == kMuBi4True, tagged = m_.tagged(r.addr);
    bool lookup = cheriot && tagged;
    bool nvm_cap = wr && cheriot && m_.in_nvm(r.addr) && r.tag;
    uint32_t g = tagged ? m_.granule(r.addr) : 0;
    bool tag_set = false;
    CoreExp e = predict_core(r);

    if (nvm_cap) {
      WtrcStep s = wtrc_step(r, g);
      wtrc_open_ = s.open; wtrc_g_ = s.g; wtrc_w0_ok_ = s.w0_ok;
      tag_set = s.tag_set;
      if (s.alert) raise_fatal();
    } else if (wtrc_open_) {
      // Any other request while a capability store to the NVM waits for its W1 ([ToO] "Error
      // Handling": out of the W0-W1 sequence).
      raise_fatal();
    }

    if (wr) {
      // The data: forwarded unchanged (REQ_TF_01b); not for a capability store to the NVM, which
      // reaches the host only as reads (REQ_WTRC_01).
      if (!nvm_cap && !r.host_err) put_data(r.addr, r.wdata, r.mask);
      if (lookup) {
        // The tag: the store's sideband, so a plain store clears it (REQ_TAG_02, 03); a partial
        // write carries tag 0 (assumption A5). The tag write leaves with the host request in the
        // same cycle (REQ_TF_03, 16a), so the host's answer does not stop it; a meta error aborts
        // it (REQ_RMW_13). A capability store to the NVM sets the tag only when verified (REQ_WTRC_06).
        TagSet nset;
        if (nvm_cap) nset = tag_set ? kTag1 : tag_of(g);
        else nset = tag_exact(r.tag && r.op == kPutFull);
        if (r.meta_err) nset = kTagAny;
        set_tag(g, nset);
        if (active_ && in_sweep(g)) watched_.insert(g);      // the core write watch (REQ_TBRE_24)
      }
      // A write leaves the captured tag of a capability load alone (REQ_TF_11b).
      return;
    }

    // An aligned read response captures the tag it presents; an unaligned one clears it. Whether an
    // error response captures or clears is not documented, so the sticky tag is then open.
    bool aligned = !(r.addr & 4);
    if (e.err) {
      captured_known_ = false;
    } else if (!aligned) {
      captured_known_ = true;
      captured_ = false;
    } else {
      captured_known_ = e.rtag != kTagAny;
      captured_ = e.rtag == kTag1;
    }
  }

  // A complete transaction checked against the prediction (the cms bench's interface).
  int core(const CoreTxn &t) {
    int n0 = n_;
    CoreReq r{t.op, t.addr, t.size, t.mask, t.wdata, t.tag, t.ena, t.host_err, t.meta_err};
    CoreExp e = predict_core(r);
    char ctx[160];
    std::snprintf(ctx, sizeof ctx, "core %s addr=0x%08x size=%u mask=%x tag=%d ena=%x",
                  t.op == kGet ? "Get" : (t.op == kPutFull ? "PutFull" : "PutPartial"), t.addr,
                  t.size, t.mask, t.tag, t.ena);
    if (t.err != e.err) {
      fail(std::string(ctx) + ": d_error " + std::to_string(t.err) + ", expected " +
           std::to_string(e.err));
    } else if (!t.err) {
      if ((t.rdata & e.rdata_care) != (e.rdata & e.rdata_care)) {
        char s[220];
        std::snprintf(s, sizeof s, "%s: rdata 0x%08x, expected 0x%08x", ctx,
                      t.rdata & e.rdata_care, e.rdata & e.rdata_care);
        fail(s);
      }
      if (!(e.rtag >> t.rtag & 1)) {
        fail(std::string(ctx) + ": tag " + std::to_string(t.rtag) + " not allowed (allowed: " +
             tagset_str(e.rtag) + ")");
      }
    }
    update_core(r);
    return n_ - n0;
  }

  // ---- The bitmap windows: revbm_tl_d (software) and corerevbm_tl (the core's TRVK) ----
  // [ToO] "Access Checkers": forwarded only in CHERIoT mode, inside the port's region (the bitmap),
  // word-aligned full words, Get or PutFullData; anything else gets an error answer and no alert
  // (REQ_META_03 to 07, REQ_ALT_04). Writes from either port follow the rule as written (reading Q45).
  BitmapExp predict_bitmap(uint8_t op, uint32_t off, uint8_t size, uint8_t mask,
                           uint8_t ena) const {
    BitmapExp e{};
    e.err = !bitmap_legal(op, off, size, mask, ena);
    if (!e.err && op == kGet) {
      e.rdata = revbm_[off / 4];
      e.rdata_care = 0xffffffff;
    }
    return e;
  }

  void update_bitmap(uint8_t op, uint32_t off, uint8_t size, uint8_t mask, uint32_t wdata,
                     uint8_t ena) {
    if (!bitmap_legal(op, off, size, mask, ena) || op == kGet) return;
    uint32_t w = off / 4;
    if (active_) revbm_changed_[w] |= revbm_[w] ^ wdata;  // the sweep's lookups may see either value
    revbm_[w] = wdata;
  }

  int bitmap(bool core_port, uint8_t op, uint32_t off, uint8_t size, uint8_t mask, uint32_t wdata,
             uint8_t ena, bool err, uint32_t rdata) {
    int n0 = n_;
    BitmapExp e = predict_bitmap(op, off, size, mask, ena);
    char ctx[120];
    std::snprintf(ctx, sizeof ctx, "%s %s off=0x%x ena=%x", core_port ? "corerevbm" : "revbm",
                  op == kGet ? "Get" : "Put", off, ena);
    if (err != e.err) {
      fail(std::string(ctx) + ": d_error " + std::to_string(err) + ", expected " +
           std::to_string(e.err));
    } else if ((rdata & e.rdata_care) != (e.rdata & e.rdata_care)) {
      char s[200];
      std::snprintf(s, sizeof s, "%s: rdata 0x%08x, expected 0x%08x", ctx, rdata, e.rdata);
      fail(s);
    }
    update_bitmap(op, off, size, mask, wdata, ena);
    return n_ - n0;
  }

  // ---- The registers [REGS], [PG] ----
  // The model has no clock: it learns that a sweep has ended when software would, from a read of
  // TBRE_STATUS.busy or TBRE_EPOCH.active at 0, or TBRE_REGWEN at 1 (sync_sweep_end, before the
  // read is predicted).
  void sync_sweep_end(uint32_t off, uint32_t rdata) {
    if (active_ && ((off == kTbreStatus && !(rdata & 1)) || (off == kTbreEpoch && !(rdata & 1)) ||
                    (off == kTbreRegwen && (rdata & 1)))) {
      sweep_end();
    }
  }

  CsrExp predict_csr_read(uint32_t off) const {
    CsrExp e{csr_value(off), 0xffffffff};
    // sweep_err: a fault in the running sweep sets it when it hits, which the bench does not see;
    // one cleared during that sweep may or may not be set again at its end (until a read shows it).
    if (off == kTbreStatus && ((active_ && sweep_err_now_) || sweep_err_open_)) {
      e.rdata_care &= ~(1u << 9);
    }
    return e;
  }

  // A register write. Reads change nothing: a sweep_err left open by a race stays open until
  // software clears it.
  void update_csr_write(uint32_t off, uint32_t wdata) { csr_write(off, wdata); }

  int csr(bool write, uint32_t off, uint32_t wdata, uint32_t rdata) {
    int n0 = n_;
    if (!write) {
      CsrExp e = predict_csr_read(off);
      if ((rdata & e.rdata_care) != (e.rdata & e.rdata_care)) {
        char s[120];
        std::snprintf(s, sizeof s, "csr read 0x%02x: 0x%08x, expected 0x%08x", off, rdata, e.rdata);
        fail(s);
      }
    }
    if (write) update_csr_write(off, wdata);
    return n_ - n0;
  }

  // ---- The revocation engine [ToO] "Revocation Engine" ----
  // Faults the bench injected into a sweep ([ToO] "Error Handling"): an error answer to a word read
  // leaves that capability alone and sets sweep_err (REQ_TBRE_32); any other error or integrity
  // fault also raises fatal_fault (REQ_TBRE_33); a failed bitmap lookup revokes (REQ_TBRE_34).
  // What a test declares about faults it injects into a running sweep (the bench's scoreboard gets
  // the same): a capability kept (a read of it failed), revoked (a failed bitmap lookup), either;
  // and that the sweep ends with sweep_err (alert: a fault, not a refused read).
  void sweep_keep(uint32_t addr) { skipped_.insert(m_.granule(addr)); }
  void sweep_revoke(uint32_t addr) { forced_revoke_.insert(m_.granule(addr)); }
  void sweep_dont_care(uint32_t addr) { dont_care_.insert(m_.granule(addr)); }
  void sweep_error(bool alert) { sweep_err_now_ = true; if (alert) raise_fatal(); }
  void sweep_read_error(uint32_t addr) { sweep_err_now_ = true; skipped_.insert(m_.granule(addr)); }
  void sweep_fault(bool on_bitmap_lookup, uint32_t addr) {
    sweep_err_now_ = true;
    raise_fatal();
    if (on_bitmap_lookup) forced_revoke_.insert(m_.granule(addr));
  }
  // A fault on the engine's bitmap lookup, seen on the meta port at bitmap word `word_off`: the
  // capability whose lookup it was counts as revoked (REQ_TBRE_34). The bench cannot see which
  // capability that was: if only one capability of the sweep has its base in that word, it is
  // revoked; otherwise each of them may or may not be.
  void sweep_lookup_fault(uint32_t word_off) {
    sweep_err_now_ = true;
    raise_fatal();
    std::vector<uint32_t> cand;
    uint32_t first = m_.granule(base_), caps = sweep_caps();
    for (uint32_t g = first; g < first + caps; g++) {
      if (!(tag_of(g) & kTag1)) continue;
      uint32_t a = m_.granule_addr(g), w0 = get_data(a), w1 = get_data(a + 4);
      if (cap_sealing(w1)) continue;
      uint32_t base = cap_base(w0, w1);
      if (!m_.in_sram(base)) continue;
      if (((base - m_.sram_base) >> 8) * 4 == word_off) cand.push_back(g);
    }
    if (cand.size() == 1) forced_revoke_.insert(cand[0]);
    else for (uint32_t g : cand) dont_care_.insert(g);
  }
  // A fault on the engine's access to a tag word (`word_off` in the meta SRAM): each capability of
  // the sweep in that word may keep its tag or lose it (REQ_TBRE_33).
  void sweep_tag_fault(uint32_t word_off) {
    sweep_err_now_ = true;
    raise_fatal();
    uint32_t first = m_.granule(base_), caps = sweep_caps();
    for (uint32_t g = first; g < first + caps; g++) {
      if (m_.tag_word_off(g) == word_off) dont_care_.insert(g);
    }
  }

  // The end of a sweep, as the bench sees it (TBRE_STATUS.busy falls, or the interrupt): the tags
  // the sweep leaves (REQ_TBRE_19, 20, 38, 39), the epoch (REQ_TBRE_10) and the interrupt (REQ_TBRE_28).
  void sweep_end() {
    if (!active_) return;
    settle_sweep(false);
    if (sweep_err_now_) {
      sweep_err_ = true;
      sweep_err_open_ = sweep_err_clr_;
    } else {
      epoch_ = (epoch_ + 1) & 0x7fffffff;                  // counts only a sweep without error (Q47)
    }
    sweep_err_clr_ = false;
    active_ = false;
    intr_state_ = true;
    watched_.clear(); skipped_.clear(); forced_revoke_.clear(); revbm_changed_.clear();
    dont_care_.clear();
  }
  // The tags a sweep leaves: every tag it clears becomes 0, or, for a sweep a reset cut short
  // (partial), either value, since the bench does not know how far it got.
  void settle_sweep(bool partial) {
    const TagSet cleared = partial ? kTagAny : kTag0;
    uint32_t first = m_.granule(base_), caps = sweep_caps();
    for (uint32_t g = first; g < first + caps; g++) {
      if (dont_care_.count(g)) { set_tag(g, kTagAny); continue; }
      if (skipped_.count(g)) continue;                       // its read failed: kept
      if (forced_revoke_.count(g)) { set_tag(g, cleared); continue; }
      if (watched_.count(g)) {
        // The core rewrote it during the sweep: the watch keeps what it wrote if the write came
        // after the engine's read; before, the engine judged the new value. The bench does not
        // see the engine's reads, so a revocable new value may end either way.
        if ((tag_of(g) & kTag1) && may_revoke(g)) set_tag(g, kTagAny);
        continue;
      }
      TagSet t = tag_of(g);
      if (!(t & kTag1)) continue;                            // untagged: nothing to clear
      uint32_t a = m_.granule_addr(g), w0 = get_data(a), w1 = get_data(a + 4);
      if (cap_sealing(w1)) continue;                         // never revoked
      uint32_t base = cap_base(w0, w1);
      if (!m_.in_sram(base)) continue;                       // no lookup outside main SRAM (reading Q18)
      uint32_t bit = (base - m_.sram_base) >> 3;
      bool rev = revbm_[bit / 32] >> (bit % 32) & 1;
      bool open = revbm_changed_.count(bit / 32) && (revbm_changed_[bit / 32] >> (bit % 32) & 1);
      if (open) set_tag(g, kTagAny);                         // the lookup may have seen either value
      else if (rev) set_tag(g, cleared);
    }
  }

  // Whether the sweep may clear granule g: a non-sealing capability whose base's bitmap bit is
  // set, or changed during the sweep (the engine may have read either value), or a forced revoke.
  bool may_revoke(uint32_t g) const {
    if (forced_revoke_.count(g)) return true;
    uint32_t a = m_.granule_addr(g), w0 = get_data(a), w1 = get_data(a + 4);
    if (cap_sealing(w1)) return false;
    uint32_t base = cap_base(w0, w1);
    if (!m_.in_sram(base)) return false;
    uint32_t bit = (base - m_.sram_base) >> 3;
    auto ch = revbm_changed_.find(bit / 32);
    return (revbm_[bit / 32] >> (bit % 32) & 1) ||
           (ch != revbm_changed_.end() && (ch->second >> (bit % 32) & 1));
  }

  // ---- Observable state ----
  bool intr_expected() const { return intr_state_ && intr_enable_; }      // REQ_TBRE_31
  bool sweep_active() const { return active_; }
  unsigned alerts_expected() const { return alerts_; }
  // fatal_fault: latched by a fault until reset. ALERT_TEST: one alert per write that raises it.
  bool fatal_expected() const { return fatal_; }
  unsigned alert_tests() const { return alert_tests_; }
  // A fault the bench observed on one of the subsystem's inputs that the documents say raises
  // fatal_fault ([ToO] "Error Handling").
  void raise_fatal() { alerts_++; fatal_ = true; }
  TagSet tag(uint32_t addr) const { return m_.tagged(addr) ? tag_of(m_.granule(addr)) : kTag0; }
  // A whole meta SRAM word as the model expects it, and which bits it is sure of.
  void meta_word(uint32_t off, uint32_t &value, uint32_t &care) const {
    value = 0; care = 0xffffffff;
    if (off < m_.revbm_bytes()) { value = revbm_[off / 4]; return; }
    if (off >= m_.meta_top_off()) { care = 0; return; }
    bool sram = off >= m_.sram_tag_off();
    uint32_t first = sram ? (off - m_.sram_tag_off()) * 8 : m_.sram_caps() + (off - m_.nvm_tag_off()) * 8;
    for (unsigned b = 0; b < 32; b++) {
      TagSet t = tag_of(first + b);
      if (t == kTag1) value |= 1u << b;
      else if (t == kTagAny) care &= ~(1u << b);
    }
  }
  int check_meta_word(uint32_t off, uint32_t actual) {
    uint32_t v, c;
    meta_word(off, v, c);
    if ((actual & c) == (v & c)) return 0;
    char s[120];
    std::snprintf(s, sizeof s, "meta word 0x%04x: 0x%08x, expected 0x%08x (care 0x%08x)", off, actual, v, c);
    fail(s);
    return 1;
  }
  const std::vector<std::string> &messages() const { return msgs_; }
  void clear_messages() { msgs_.clear(); }
  int mismatches() const { return n_; }
  const Map &map() const { return m_; }

 private:
  // The WTRC: a capability store to the NVM ([ToO] "Write-to-Read-and-Compare Filter"). W0 is
  // answered with an error unless it is a full-word PutFullData in sequence that matches the NVM;
  // W1 sets the tag only when it and its W0 both matched (REQ_WTRC_04 to 08). A partial word or a
  // broken sequence also raises fatal_fault (REQ_WTRC_10, 11); a mismatch or a failed NVM read
  // does not (REQ_WTRC_09). wtrc_step computes the step and the WTRC's next state; update_core
  // applies it.
  struct WtrcStep {
    bool ok, tag_set, alert;
    bool open, w0_ok;  // the next state
    uint32_t g;
  };
  WtrcStep wtrc_step(const CoreReq &r, uint32_t g) const {
    WtrcStep s{false, false, false, wtrc_open_, wtrc_w0_ok_, wtrc_g_};
    bool full = r.op == kPutFull && r.mask == 0xf && r.size == 2;
    bool match = full && !r.host_err && r.wdata == get_data(r.addr);
    bool seq_bad;
    if (!(r.addr & 4)) {
      seq_bad = wtrc_open_;
      s.open = true;
      s.g = g;
      s.w0_ok = match && !seq_bad;
      s.ok = s.w0_ok;
    } else {
      seq_bad = !wtrc_open_ || wtrc_g_ != g;
      s.ok = !seq_bad && wtrc_w0_ok_ && match;
      s.open = false;
      s.tag_set = s.ok;
    }
    s.alert = !full || seq_bad;
    return s;
  }

  bool bitmap_legal(uint8_t op, uint32_t off, uint8_t size, uint8_t mask, uint8_t ena) const {
    return ena == kMuBi4True && off < m_.revbm_bytes() && (off & 3) == 0 && size == 2 &&
           (op == kGet || (op == kPutFull && mask == 0xf));
  }

  static std::string tagset_str(TagSet s) {
    return s == kTag0 ? "0" : s == kTag1 ? "1" : "0 or 1";
  }

  void csr_write(uint32_t off, uint32_t v) {
    switch (off) {
      case kIntrState: if (v & 1) intr_state_ = false; break;        // rw1c (REQ_TBRE_29)
      case kIntrEnable: intr_enable_ = v & 1; break;
      case kIntrTest: if (v & 1) intr_state_ = true; break;          // REQ_TBRE_28
      case kAlertTest:
        if ((v & 1) && alert_test_regwen_) { alerts_++; alert_tests_++; }  // REQ_ALT_02
        if (!(v >> 31 & 1)) alert_test_regwen_ = false;             // rw0c (REQ_ALT_03)
        break;
      // While the engine is active, writes to BASE_ADDR, NUM_CAPS and START have no effect
      // (REQ_TBRE_05); a start then is dropped before any check, so no start_err (reading Q46).
      case kTbreBase: if (!active_) base_ = v & ~7u; break;          // bits 2:0 read 0 (REQ_TBRE_06)
      case kTbreNum: if (!active_) num_ = v & 0x7fffffff; break;
      case kTbreStart:
        if ((v & 1) && !active_) start();
        break;
      case kTbreStatus:                                              // rw1c (REQ_TBRE_04b, 36b)
        if (v >> 8 & 1) start_err_ = false;
        if (v >> 9 & 1) {
          sweep_err_ = sweep_err_open_ = false;
          if (active_ && sweep_err_now_) sweep_err_clr_ = true;   // the fault may have hit before
        }
        break;
      default: break;
    }
  }
  uint32_t csr_value(uint32_t off) const {
    switch (off) {
      case kIntrState: return intr_state_;
      case kIntrEnable: return intr_enable_;
      case kAlertTest: return uint32_t(alert_test_regwen_) << 31;
      case kTbreRegwen: return !active_;                             // REQ_TBRE_07
      case kTbreBase: return base_;
      case kTbreNum: return num_;
      case kTbreStatus: return uint32_t(active_) | uint32_t(start_err_) << 8 | uint32_t(sweep_err_) << 9;
      case kTbreEpoch: return epoch_ << 1 | uint32_t(active_);       // REQ_TBRE_09
      default: return 0;
    }
  }
  // A start: ignored, with start_err, outside CHERIoT mode, with NUM_CAPS zero or a base outside the
  // tagged regions ([REGS] TBRE_STATUS.start_err, REQ_TBRE_02, 03). The mode is the bench's
  // (set_mode), as it was when the register write was taken.
  void start() {
    if (mode_ != kMuBi4True || num_ == 0 || !m_.tagged(base_)) { start_err_ = true; return; }
    active_ = true;
    sweep_err_now_ = sweep_err_clr_ = false;
  }
 public:
  void set_mode(uint8_t ena) { mode_ = ena; }

 private:
  uint32_t sweep_caps() const {
    uint32_t top = m_.in_sram(base_) ? m_.sram_top : m_.nvm_top;  // ends at its region's top
    uint64_t room = (top - base_) / 8;
    return uint32_t(num_ < room ? num_ : room);
  }
  bool in_sweep(uint32_t g) const {
    uint32_t first = m_.granule(base_);
    return g >= first && g < first + sweep_caps();
  }
  TagSet tag_of(uint32_t g) const {
    auto it = tags_.find(g);
    return it == tags_.end() ? kTag0 : it->second;               // the meta SRAM starts at 0
  }
  void set_tag(uint32_t g, TagSet s) { tags_[g] = s; }
  uint32_t get_data(uint32_t a) const {
    auto it = data_.find(a & ~3u);
    return it == data_.end() ? 0 : it->second;
  }
  void put_data(uint32_t a, uint32_t d, uint8_t mask) {
    uint32_t w = get_data(a), m = 0;
    for (int b = 0; b < 4; b++) if (mask >> b & 1) m |= 0xffu << (8 * b);
    data_[a & ~3u] = (w & ~m) | (d & m);
  }
  void fail(const std::string &s) { n_++; msgs_.push_back(s); }

  Map m_;
  uint8_t mode_ = kMuBi4True;
  std::map<uint32_t, TagSet> tags_;
  std::map<uint32_t, uint32_t> data_;
  std::vector<uint32_t> revbm_;
  bool intr_state_, intr_enable_, alert_test_regwen_;
  uint32_t base_, num_, epoch_;
  bool start_err_, sweep_err_, sweep_err_now_ = false, active_;
  bool sweep_err_open_ = false, sweep_err_clr_ = false;  // sweep_err not known; cleared mid-sweep
  std::set<uint32_t> watched_, skipped_, forced_revoke_;
  std::map<uint32_t, uint32_t> revbm_changed_;
  std::set<uint32_t> dont_care_;
  bool wtrc_open_, wtrc_w0_ok_ = false;
  uint32_t wtrc_g_ = 0;
  bool captured_known_ = true, captured_ = false;  // the tag filter's sticky tag
  unsigned alerts_, alert_tests_ = 0;
  bool fatal_ = false;
  int n_ = 0;
  std::vector<std::string> msgs_;
};

}  // namespace cms

#endif  // CMS_CORE_H_
