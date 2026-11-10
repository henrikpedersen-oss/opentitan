// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl memory protection: the reference model of the decision rram_ctrl_mp and
// rram_ctrl_region_cfg make, shared by eg2sim (Eg2RramCtrl, eg2_mem.h) and the memory-protection
// unit bench (hw/ip/rram_ctrl/dv/edagen/rram_ctrl_mp, through dpi/eg2_rram_dpi.cc).
//
// Written from the documentation only, never from the RTL:
//   hw/ip/rram_ctrl/doc/theory_of_operation.md "Memory Protection" (requesters, data partition,
//     emulated info pages, information partition, access attributes, the LCMGR and OTP hardware
//     plugs), "RMA Handling" (rma_sw_dis: host reads answered with an error), "RRAM Escalation &
//     Disable" (rram_disable: every host and controller request blocked);
//   doc/programmers_guide.md "Configuring Memory Protection", "Debugging Errors" (the lcmgr pages
//     assume scrambling and ECC unless HW_INFO_CFG_OVERRIDE says otherwise);
//   doc/registers.md MP_REGION_CFG, MP_REGION, DEFAULT_REGION, INFO_PAGE_CFG, EMUL_INFO_REGWEN,
//     EMUL_INFO_REGION, EMUL_INFO_SUBREGION_REGWEN, EMUL_INFO_SUBREGION, EMUL_INFO_SUBREGION_CFG,
//     HW_INFO_CFG_OVERRIDE (field layouts as below);
//   lc_ctrl: a life-cycle enable allows only when it is On.
//
// Every answer is three-valued. kRramUnpred marks a case the documentation does not decide; the
// bench does not check it and counts it, and eg2sim, which needs an answer, denies it. The cases:
//   - a SCRAMBLE_EN or ECC_EN field that is neither MuBi4True nor MuBi4False (MP_REGION_CFG,
//     DEFAULT_REGION, INFO_PAGE_CFG, EMUL_INFO_SUBREGION_CFG say only "MuBi4 values");
//   - EMUL_INFO_REGION.EN neither MuBi4True nor MuBi4False ("Once EN is set" / "If disabled");
//   - HW_INFO_CFG_OVERRIDE fields not valid MuBi4 ("The behaviour is not specified");
//   - rma_sw_dis (sw_disable) neither MuBi4True nor MuBi4False;
//   - scramble, ECC and address-XOR of the lcmgr data rules and the lcmgr info rules (address XOR)
//     (the documentation gives only the OTP plug's attributes and says the software regions and
//     info pages have address XOR on);
//   - the loopback requester (Rewrite) altogether: the requester table lists it, but no rule says
//     what it may reach. programmers_guide.md "Issuing a Rewrite Operation" ties Rewrite to the
//     matching MP_REGION_CFG / DEFAULT_REGION / EMUL_INFO_SUBREGION_CFG / INFO_PAGE_CFG (ECC_EN)
//     and reports its failures "exactly as for a normal read or write error"; whether the loopback
//     requester itself is checked against them is open (status/rtl_tickets.md: rram_ctrl_mp lets
//     the loopback requester reach every page, so Rewrite bypasses memory protection). eg2sim checks
//     a Rewrite against the software rules (Eg2RramCtrl::start);
//   - whether a request rram_disable blocks still reaches the PHY output of rram_ctrl_mp: the
//     documentation gives the blocking to rram_phy ("rram_phy observes the same rram_disable bus and
//     suppresses all new host and controller requests"), the error is predicted;
//   - a software, OTP or loopback request with a phase other than PhaseInvalid (phases belong to the
//     lcmgr rules; the arbiter drives no phase for the other requesters);
//   - a life-cycle enable that is changing (the caller passes kRramUnpred while it settles).
//
// Register layouts (registers.md): MP_REGION_CFG / INFO_PAGE_CFG EN 3:0, RD_EN 7:4, WR_EN 11:8,
// SCRAMBLE_EN 15:12, ECC_EN 19:16; MP_REGION / EMUL_INFO_REGION BASE 11:0, SIZE 23:12 (pages - 1),
// EMUL_INFO_REGION EN 27:24; DEFAULT_REGION RD_EN 3:0, WR_EN 7:4, SCRAMBLE_EN 11:8, ECC_EN 15:12;
// EMUL_INFO_SUBREGION TOP 7:0; EMUL_INFO_SUBREGION_CFG as MP_REGION_CFG with 3:0 reserved;
// HW_INFO_CFG_OVERRIDE SCRAMBLE_DIS 3:0, ECC_DIS 7:4. A *_REGWEN of 0 is locked.

#ifndef EG2_RRAM_MP_CORE_H_
#define EG2_RRAM_MP_CORE_H_

#include <cstdint>

namespace eg2_model {

enum RramTri : int { kRramNo = 0, kRramYes = 1, kRramUnpred = 2 };

inline RramTri rram_not(RramTri a) { return a == kRramUnpred ? a : (a == kRramYes ? kRramNo : kRramYes); }
inline RramTri rram_and(RramTri a, RramTri b) {
  if (a == kRramNo || b == kRramNo) return kRramNo;
  if (a == kRramYes && b == kRramYes) return kRramYes;
  return kRramUnpred;
}
inline RramTri rram_or(RramTri a, RramTri b) { return rram_not(rram_and(rram_not(a), rram_not(b))); }
inline RramTri rram_bool(bool b) { return b ? kRramYes : kRramNo; }

struct RramMp {
  static constexpr uint32_t kMuBi4True = 0x6, kMuBi4False = 0x9;
  static constexpr uint32_t kLcOn = 0x5;  // lc_tx_t On (Off is 0xa)
  static constexpr unsigned kMpRegions = 10, kWindows = 2, kSubregions = 24, kSubPerWindow = 12;
  static constexpr unsigned kDataPages = 4096, kInfoPages = 8;
  static constexpr unsigned kOtpFirstPage = 4090;  // the top six data pages: OTP and its integrity
  static constexpr unsigned kCreatorPage = 5, kOwnerPage = 6, kIsolatedPage = 7;

  // Requester (rram_ctrl_mp's if_sel) and phase, as the unit's ports encode them.
  enum Sel : unsigned { kSelNone = 0, kSelSw = 1, kSelHwOtp = 2, kSelHwLcMgr = 3, kSelLoopBack = 4 };
  enum Phase : unsigned { kPhaseSeed = 0, kPhaseRma = 1, kPhaseNone = 2, kPhaseInvalid = 3 };

  // The registers (software configuration) and the hardware inputs that configure protection.
  uint32_t region[kMpRegions] = {};
  uint32_t region_cfg[kMpRegions] = {};
  uint32_t default_cfg = 0x9999;
  uint32_t info_cfg[kInfoPages] = {};
  uint32_t emul_regwen[kWindows] = {1, 1};
  uint32_t emul_region[kWindows] = {};
  uint32_t sub_regwen[kSubregions] = {};
  uint32_t sub_top[kSubregions] = {};
  uint32_t sub_cfg[kSubregions] = {};
  uint32_t hw_info_override = 0x99;
  // Life-cycle enables: creator seed, owner seed, isolated read, isolated write.
  RramTri lc_creator = kRramNo, lc_owner = kRramNo, lc_iso_rd = kRramNo, lc_iso_wr = kRramNo;

  RramMp() {
    for (unsigned i = 0; i < kMpRegions; ++i) region_cfg[i] = 0x99999;
    for (unsigned i = 0; i < kInfoPages; ++i) info_cfg[i] = 0x99999;
    for (unsigned i = 0; i < kWindows; ++i) emul_region[i] = 0x9u << 24;
    for (unsigned i = 0; i < kSubregions; ++i) {
      sub_regwen[i] = 1;
      sub_cfg[i] = 0x99990;
    }
  }

  // Which rule decided (for coverage): kind and index (MP region, subregion, window, page).
  enum Rule : int {
    kRuleNone = 0, kRuleOtpExcl, kRuleSubregion, kRuleWindowDeny, kRuleMpRegion, kRuleDefault,
    kRuleInfoPage, kRuleInfoLc, kRuleLcMgr, kRuleOtpPlug, kRuleLoopBack, kRuleOutOfRange,
    kRuleUnpred, kRuleCount
  };
  // The attributes of the rule that decides an access.
  struct Attr {
    RramTri rd, wr, scr, ecc, xr;
    int rule = kRuleNone, idx = 0;
  };
  // A controller access: err (rram_ctrl_mp answers with an error), fwd (it goes on to the PHY),
  // and the attributes it goes with (meaningful when fwd is kRramYes).
  struct Ctrl {
    RramTri err, fwd, scr, ecc, xr;
    int rule, idx;
  };
  struct Host {
    RramTri err, fwd, scr, ecc;
    int rule, idx;
  };

  static uint32_t field(uint32_t v, unsigned lsb) { return (v >> lsb) & 0xf; }
  // A permission or an enable that must be MuBi4True to allow (any other value denies).
  static RramTri strict(uint32_t v) { return rram_bool(v == kMuBi4True); }
  // An attribute whose invalid encodings the documentation leaves open.
  static RramTri attr(uint32_t v) {
    return v == kMuBi4True ? kRramYes : (v == kMuBi4False ? kRramNo : kRramUnpred);
  }
  static Attr with(Attr a, int rule, int idx = 0) {
    a.rule = rule;
    a.idx = idx;
    return a;
  }
  static Attr deny(int rule = kRuleNone, int idx = 0) {
    return with({kRramNo, kRramNo, kRramNo, kRramNo, kRramNo}, rule, idx);
  }
  static Attr unpred() {
    return with({kRramUnpred, kRramUnpred, kRramUnpred, kRramUnpred, kRramUnpred}, kRuleUnpred);
  }
  // MP_REGION_CFG / INFO_PAGE_CFG / EMUL_INFO_SUBREGION_CFG layout: RD 7:4 ... ECC 19:16.
  static Attr cfg_attr(uint32_t c) {
    return {strict(field(c, 4)), strict(field(c, 8)), attr(field(c, 12)), attr(field(c, 16)), kRramYes};
  }

  static uint32_t base_of(uint32_t r) { return r & 0xfff; }
  static uint32_t size_of(uint32_t r) { return (r >> 12) & 0xfff; }
  // [BASE, BASE + SIZE] inclusive, no wrap (registers.md MP_REGION.SIZE).
  static bool in(uint32_t page, uint32_t base, uint32_t size) { return page >= base && page <= base + size; }

  RramTri window_en(unsigned w) const { return attr(field(emul_region[w], 24)); }
  bool in_window(unsigned w, uint32_t page) const {
    return in(page, base_of(emul_region[w]), size_of(emul_region[w]));
  }
  // "if two enabled windows overlap, the hardware disables the subregions of both windows".
  RramTri overlap() const {
    uint32_t b0 = base_of(emul_region[0]), e0 = b0 + size_of(emul_region[0]);
    uint32_t b1 = base_of(emul_region[1]), e1 = b1 + size_of(emul_region[1]);
    return rram_and(rram_and(window_en(0), window_en(1)), rram_bool(b0 <= e1 && b1 <= e0));
  }
  // EMUL_INFO_SUBREGION: window enabled and locked, its own and every lower-indexed placement of
  // the window locked, TOP not beyond the window's SIZE, no overlap with the other enabled window.
  RramTri sub_enabled(unsigned i) const {
    unsigned w = i / kSubPerWindow;
    bool chain = emul_regwen[w] == 0;
    for (unsigned j = w * kSubPerWindow; j <= i; ++j) chain = chain && sub_regwen[j] == 0;
    bool fits = (sub_top[i] & 0xff) <= size_of(emul_region[w]);
    return rram_and(rram_and(window_en(w), rram_bool(chain && fits)), rram_not(overlap()));
  }

  // The data-partition rule list of software requests (host = false) or host reads (host = true):
  // the OTP exclusion, the subregions (software only), the window-deny entries, MP_REGION_0..9,
  // DEFAULT_REGION; the lowest-indexed match decides.
  Attr data_rule(uint32_t page, bool host) const {
    if (page >= kOtpFirstPage) return deny(kRuleOtpExcl);
    if (!host) {
      for (unsigned i = 0; i < kSubregions; ++i) {
        unsigned w = i / kSubPerWindow;
        uint32_t base = base_of(emul_region[w]);
        RramTri m = rram_and(sub_enabled(i), rram_bool(in(page, base, sub_top[i] & 0xff)));
        if (m == kRramUnpred) return unpred();
        if (m == kRramYes) return with(cfg_attr(sub_cfg[i]), kRuleSubregion, int(i));
      }
    }
    for (unsigned w = 0; w < kWindows; ++w) {
      RramTri m = rram_and(window_en(w), rram_bool(in_window(w, page)));
      if (m == kRramUnpred) return unpred();
      if (m == kRramYes) return deny(kRuleWindowDeny, int(w));
    }
    for (unsigned i = 0; i < kMpRegions; ++i) {
      // MP_REGION_CFG.EN: "With any other value [than MuBi4True], the region is disabled and not
      // matched against any incoming transaction."
      if (field(region_cfg[i], 0) != kMuBi4True) continue;
      if (in(page, base_of(region[i]), size_of(region[i])))
        return with(cfg_attr(region_cfg[i]), kRuleMpRegion, int(i));
    }
    return with({strict(field(default_cfg, 0)), strict(field(default_cfg, 4)),
                 attr(field(default_cfg, 8)), attr(field(default_cfg, 12)), kRramYes},
                kRuleDefault);
  }

  // A software info-page access: INFO_PAGE_CFG of the page ("With any other value [of EN], the
  // info page is disabled and all accesses to it are denied"), pages 5, 6 and 7 ANDed with the
  // life-cycle enables.
  Attr info_rule_sw(uint32_t page) const {
    if (page >= kInfoPages) return deny(kRuleOutOfRange);
    uint32_t c = info_cfg[page];
    if (field(c, 0) != kMuBi4True) return deny(kRuleInfoPage, int(page));
    Attr a = with(cfg_attr(c), page >= kCreatorPage ? kRuleInfoLc : kRuleInfoPage, int(page));
    if (page == kCreatorPage) {
      a.rd = rram_and(a.rd, lc_creator);
      a.wr = rram_and(a.wr, lc_creator);
    } else if (page == kOwnerPage) {
      a.rd = rram_and(a.rd, lc_owner);
      a.wr = rram_and(a.wr, lc_owner);
    } else if (page == kIsolatedPage) {
      a.rd = rram_and(a.rd, lc_iso_rd);
      a.wr = rram_and(a.wr, lc_iso_wr);
    }
    return a;
  }

  // The lcmgr rules: PhaseSeed reads the creator and owner pages, PhaseRma reads and writes them,
  // the isolated page and every non-OTP data page. Scrambling and ECC of the info pages are on
  // unless HW_INFO_CFG_OVERRIDE disables them.
  Attr lcmgr_rule(unsigned part, uint32_t page, unsigned phase) const {
    if (part == 0) {
      if (phase == kPhaseRma && page < kOtpFirstPage)
        return with({kRramYes, kRramYes, kRramUnpred, kRramUnpred, kRramUnpred}, kRuleLcMgr, 0);
      return deny(kRuleLcMgr, 0);
    }
    Attr a = deny(kRuleLcMgr, 1);
    bool seed_page = page == kCreatorPage || page == kOwnerPage;
    if (seed_page && phase == kPhaseSeed)
      a = with({kRramYes, kRramNo, kRramYes, kRramYes, kRramUnpred}, kRuleLcMgr, 2);
    else if ((seed_page || page == kIsolatedPage) && phase == kPhaseRma)
      a = with({kRramYes, kRramYes, kRramYes, kRramYes, kRramUnpred}, kRuleLcMgr, 3);
    else return a;
    a.scr = rram_not(attr(field(hw_info_override, 0)));
    a.ecc = rram_not(attr(field(hw_info_override, 4)));
    return a;
  }

  // The OTP plug: the OTP pages only, read and write, scrambling off, ECC on, address XOR off.
  static Attr otp_rule(unsigned part, uint32_t page) {
    if (part == 0 && page >= kOtpFirstPage && page < kDataPages)
      return with({kRramYes, kRramYes, kRramNo, kRramYes, kRramNo}, kRuleOtpPlug, 1);
    return deny(kRuleOtpPlug, 0);
  }

  // One controller access. rram_disable and the other mubi inputs are the 4-bit port values.
  Ctrl ctrl(unsigned sel, unsigned phase, unsigned part, uint32_t page, bool write, bool ovfl,
            uint32_t rram_disable) const {
    Attr a;
    bool out_of_range = ovfl || page >= (part == 0 ? kDataPages : kInfoPages);
    if (sel != kSelHwLcMgr && sel <= kSelLoopBack && sel != kSelNone && phase != kPhaseInvalid) {
      a = unpred();
    } else if (out_of_range) {
      a = deny(kRuleOutOfRange);
    } else if (sel == kSelSw) {
      a = part == 0 ? data_rule(page, false) : info_rule_sw(page);
    } else if (sel == kSelHwOtp) {
      a = otp_rule(part, page);
    } else if (sel == kSelHwLcMgr) {
      a = lcmgr_rule(part, page, phase);
    } else if (sel == kSelLoopBack) {
      a = with({kRramUnpred, kRramUnpred, kRramUnpred, kRramUnpred, kRramUnpred}, kRuleLoopBack);
    } else {
      a = deny();  // "There are four possible requesters"
    }
    RramTri ok = write ? a.wr : a.rd;
    Ctrl c{rram_not(ok), ok, a.scr, a.ecc, a.xr, a.rule, a.idx};
    if (rram_disable != kMuBi4False) {
      c.err = kRramYes;
      c.fwd = kRramUnpred;
    }
    return c;
  }

  // One host read of a data page.
  Host host(uint32_t page, uint32_t sw_disable, uint32_t rram_disable) const {
    Attr a = data_rule(page, true);
    RramTri ok = rram_and(a.rd, rram_not(attr(sw_disable)));
    Host h{rram_not(ok), ok, a.scr, a.ecc, a.rule, a.idx};
    if (rram_disable != kMuBi4False) {
      h.err = kRramYes;
      h.fwd = kRramUnpred;
    }
    return h;
  }
};

}  // namespace eg2_model

#endif  // EG2_RRAM_MP_CORE_H_
