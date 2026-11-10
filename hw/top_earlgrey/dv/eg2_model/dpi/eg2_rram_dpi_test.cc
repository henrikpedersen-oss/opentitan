// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the rram_ctrl reference models through their DPI functions (no simulator):
//   g++ -std=c++17 eg2_rram_dpi.cc eg2_rram_dpi_test.cc -o eg2_rram_dpi_test && ./eg2_rram_dpi_test
// (make rram_test). Every expectation quotes the document it comes from. EG2_RRAM_TEST_BREAK=1
// inverts one expectation: the run must then fail.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "../ip/eg2_rram_scramble_core.h"

extern "C" {
void *eg2_rram_mp_create();
void eg2_rram_mp_release(void *h);
void eg2_rram_mp_set(void *h, int kind, int idx, unsigned int value);
void eg2_rram_mp_ctrl(void *h, unsigned int sel, unsigned int phase, unsigned int part,
                      unsigned int page, int write, int ovfl, unsigned int rram_disable, int *err,
                      int *fwd, int *scr, int *ecc, int *xr, int *rule, int *idx);
void eg2_rram_mp_host(void *h, unsigned int page, unsigned int sw_disable,
                      unsigned int rram_disable, int *err, int *fwd, int *scr, int *ecc,
                      int *rule, int *idx);
void *eg2_rram_scr_create(const uint32_t *rst_addr_key, const uint32_t *rst_data_key);
void eg2_rram_scr_release(void *h);
void eg2_rram_scr_step(void *h, unsigned int keys_disable, int keys_valid, const uint32_t *ak,
                       const uint32_t *dk, const uint32_t *rak, const uint32_t *rdk);
unsigned int eg2_rram_scr_epoch(void *h);
void eg2_rram_scr_mask(void *h, unsigned int addr, uint32_t *mask);
void eg2_rram_scr_op(void *h, const uint32_t *din, int decrypt, uint32_t *dout);
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

constexpr unsigned T = 0x6, F = 0x9;  // MuBi4True, MuBi4False
constexpr int No = 0, Yes = 1, Un = 2;
enum { kRegion, kRegionCfg, kDefault, kInfoCfg, kEmulRegwen, kEmulRegion, kSubRegwen, kSubTop,
       kSubCfg, kHwInfo, kLc };
enum { SelNone = 0, Sw = 1, Otp = 2, LcMgr = 3, Loop = 4, SwErr = 5 };
enum { Seed = 0, Rma = 1, PhNone = 2, Inv = 3 };

// MP_REGION_CFG / INFO_PAGE_CFG: EN, RD, WR, SCRAMBLE, ECC.
unsigned cfg(unsigned en, unsigned rd, unsigned wr, unsigned scr, unsigned ecc) {
  return en | rd << 4 | wr << 8 | scr << 12 | ecc << 16;
}
unsigned region(unsigned base, unsigned size) { return base | size << 12; }

struct C {
  int err, fwd, scr, ecc, xr, rule, idx;
};
C ctrl(void *h, unsigned sel, unsigned phase, unsigned part, unsigned page, bool wr,
       unsigned dis = F, bool ovfl = false) {
  C c{};
  eg2_rram_mp_ctrl(h, sel, phase, part, page, wr, ovfl, dis, &c.err, &c.fwd, &c.scr, &c.ecc, &c.xr,
                   &c.rule, &c.idx);
  return c;
}
C host(void *h, unsigned page, unsigned swdis = F, unsigned dis = F) {
  C c{};
  eg2_rram_mp_host(h, page, swdis, dis, &c.err, &c.fwd, &c.scr, &c.ecc, &c.rule, &c.idx);
  return c;
}
bool allowed(const C &c) { return c.err == No && c.fwd == Yes; }
bool denied(const C &c) { return c.err == Yes && c.fwd == No; }

void mp_tests(bool brk) {
  void *h = eg2_rram_mp_create();
  // programmers_guide "Configuring Memory Protection": "On reset, DEFAULT_REGION denies all
  // access"; regions and info pages disabled.
  check(denied(ctrl(h, Sw, Inv, 0, 0, false)), "reset: a software read of data page 0 is denied");
  check(denied(host(h, 0)), "reset: a host read of page 0 is denied");
  check(denied(ctrl(h, Sw, Inv, 1, 0, false)), "reset: info page 0 is disabled");

  eg2_rram_mp_set(h, kDefault, 0, T | T << 4 | F << 8 | T << 12);
  C c = ctrl(h, Sw, Inv, 0, 7, true);
  check(allowed(c) && c.scr == No && c.ecc == Yes && c.xr == Yes,
        "DEFAULT_REGION decides: write allowed, scrambling off, ECC on, address XOR always on");
  check(allowed(host(h, 7)), "host read through DEFAULT_REGION");

  // theory_of_operation "Memory Protection for OTP Hardware Plug".
  for (unsigned p : {4090u, 4095u}) {
    check(denied(ctrl(h, Sw, Inv, 0, p, false)), "SwInitDataCfg: software never reaches the OTP pages");
    check(denied(host(h, p)), "the host never reads the OTP pages");
    c = ctrl(h, Otp, Inv, 0, p, true);
    check(allowed(c) && c.scr == No && c.ecc == Yes && c.xr == No,
          "OTP plug: OTP pages read/write, scramble off, ECC on, address XOR off");
  }
  check(denied(ctrl(h, Otp, Inv, 0, 4089, false)), "OTP plug: every other address is rejected");
  check(denied(ctrl(h, Otp, Inv, 1, 0, false)), "OTP plug: the info partition is rejected");

  // MP regions: lowest index wins, [BASE, BASE + SIZE] inclusive.
  eg2_rram_mp_set(h, kRegion, 0, region(10, 9));
  eg2_rram_mp_set(h, kRegionCfg, 0, cfg(T, T, F, T, T));
  eg2_rram_mp_set(h, kRegion, 1, region(15, 15));
  eg2_rram_mp_set(h, kRegionCfg, 1, cfg(T, T, T, F, F));
  check(denied(ctrl(h, Sw, Inv, 0, 15, true)), "region 0 (read only) wins over region 1 at page 15");
  c = ctrl(h, Sw, Inv, 0, 25, true);
  check(c.rule == 4 && c.idx == 1, "rule report: MP region 1 decides page 25");
  check(allowed(ctrl(h, Sw, Inv, 0, 15, false)), "region 0 allows the read at page 15");
  check(allowed(ctrl(h, Sw, Inv, 0, 25, true)) != brk, "region 1 allows the write at page 25");
  check(allowed(ctrl(h, Sw, Inv, 0, 30, true)), "page BASE + SIZE is inside the region");
  check(denied(ctrl(h, Sw, Inv, 0, 19, true)), "page 19 is region 0's last page");
  check(ctrl(h, Sw, Inv, 0, 12, false).scr == Yes, "region 0's scramble_en applies");
  eg2_rram_mp_set(h, kRegionCfg, 0, cfg(0x3, T, F, T, T));
  check(allowed(ctrl(h, Sw, Inv, 0, 15, true)), "MP_REGION_CFG.EN not MuBi4True: not matched");
  eg2_rram_mp_set(h, kRegionCfg, 0, cfg(T, T, F, T, T));
  eg2_rram_mp_set(h, kRegionCfg, 2, cfg(T, T, T, 0x5, T));
  eg2_rram_mp_set(h, kRegion, 2, region(40, 0));
  c = ctrl(h, Sw, Inv, 0, 40, false);
  check(allowed(c) && c.scr == Un, "an invalid SCRAMBLE_EN encoding is not predicted");
  check(ctrl(h, Sw, Inv, 0, 41, false).scr == No, "SIZE 0: one page only");

  // Emulated info windows (theory_of_operation "Emulated Info Pages", registers.md).
  unsigned win0 = region(200, 9) | T << 24;
  eg2_rram_mp_set(h, kEmulRegion, 0, win0);
  check(denied(ctrl(h, Sw, Inv, 0, 205, true)), "an enabled window denies what no subregion covers");
  check(denied(host(h, 205)), "the host never reads an enabled window");
  check(allowed(ctrl(h, Sw, Inv, 0, 210, true)), "page 210 is past the window");
  eg2_rram_mp_set(h, kSubTop, 0, 3);
  eg2_rram_mp_set(h, kSubCfg, 0, cfg(0, T, T, T, F));
  eg2_rram_mp_set(h, kSubRegwen, 0, 0);
  check(denied(ctrl(h, Sw, Inv, 0, 202, false)), "subregion of an unlocked window is not enabled");
  eg2_rram_mp_set(h, kEmulRegwen, 0, 0);
  c = ctrl(h, Sw, Inv, 0, 202, true);
  check(allowed(c) && c.scr == Yes && c.ecc == No && c.xr == Yes, "locked subregion 0 decides [0, TOP_0]");
  check(denied(host(h, 202)), "host stays blocked by the window whatever the subregions say");
  check(denied(ctrl(h, Sw, Inv, 0, 204, false)), "offset 4 is not covered: denied");
  eg2_rram_mp_set(h, kSubTop, 2, 6);
  eg2_rram_mp_set(h, kSubCfg, 2, cfg(0, T, F, F, F));
  eg2_rram_mp_set(h, kSubRegwen, 2, 0);
  check(denied(ctrl(h, Sw, Inv, 0, 205, false)), "subregion 2 with subregion 1 unlocked is not enabled");
  eg2_rram_mp_set(h, kSubTop, 1, 4);
  eg2_rram_mp_set(h, kSubRegwen, 1, 0);
  check(allowed(ctrl(h, Sw, Inv, 0, 205, false)), "the whole lock chain: subregion 2 decides (TOP_1, TOP_2]");
  check(denied(ctrl(h, Sw, Inv, 0, 205, true)), "subregion 2 is read only");
  check(denied(ctrl(h, Sw, Inv, 0, 204, false)), "subregion 1 (offset 4) has no permissions");
  eg2_rram_mp_set(h, kSubTop, 2, 10);
  check(denied(ctrl(h, Sw, Inv, 0, 205, false)), "TOP beyond the window's SIZE: not enabled");
  eg2_rram_mp_set(h, kSubTop, 2, 6);
  eg2_rram_mp_set(h, kEmulRegion, 1, region(209, 5) | T << 24);
  check(denied(ctrl(h, Sw, Inv, 0, 202, false)), "overlapping enabled windows disable the subregions");
  check(denied(ctrl(h, Sw, Inv, 0, 212, false)), "both windows stay denied");
  eg2_rram_mp_set(h, kEmulRegion, 1, region(209, 5) | F << 24);
  check(allowed(ctrl(h, Sw, Inv, 0, 202, false)), "a disabled window does not count as overlapping");
  check(allowed(ctrl(h, Sw, Inv, 0, 212, false)), "a disabled window is not matched");
  eg2_rram_mp_set(h, kEmulRegion, 1, region(209, 5) | 0x1u << 24);
  check(ctrl(h, Sw, Inv, 0, 212, false).fwd == Un, "an invalid window EN is not predicted");
  eg2_rram_mp_set(h, kEmulRegion, 1, region(209, 5) | F << 24);
  eg2_rram_mp_set(h, kEmulRegion, 0, region(200, 9) | F << 24);
  check(allowed(ctrl(h, Sw, Inv, 0, 205, true)), "window disabled: its subregions are inert too");

  // Information partition (programmers_guide "Information Partition").
  eg2_rram_mp_set(h, kInfoCfg, 3, cfg(T, T, T, T, T));
  c = ctrl(h, Sw, Inv, 1, 3, true);
  check(allowed(c) && c.xr == Yes, "info page 3 read/write, address XOR on");
  eg2_rram_mp_set(h, kInfoCfg, 3, cfg(0x5, T, T, T, T));
  check(denied(ctrl(h, Sw, Inv, 1, 3, false)), "INFO_PAGE_CFG.EN not MuBi4True: all accesses denied");
  check(denied(ctrl(h, Sw, Inv, 1, 8, false)), "there is no info page 8");
  for (unsigned p = 5; p <= 7; ++p) eg2_rram_mp_set(h, kInfoCfg, p, cfg(T, T, T, T, T));
  check(denied(ctrl(h, Sw, Inv, 1, 5, false)), "creator page: life cycle off denies");
  eg2_rram_mp_set(h, kLc, 0, 1);
  check(allowed(ctrl(h, Sw, Inv, 1, 5, true)), "creator page: life cycle on, the register decides");
  check(denied(ctrl(h, Sw, Inv, 1, 6, true)), "owner page has its own life-cycle signal");
  eg2_rram_mp_set(h, kLc, 2, 1);
  check(allowed(ctrl(h, Sw, Inv, 1, 7, false)) && denied(ctrl(h, Sw, Inv, 1, 7, true)),
        "isolated page: read with iso_rd, write needs iso_wr");
  eg2_rram_mp_set(h, kLc, 3, 2);
  check(ctrl(h, Sw, Inv, 1, 7, true).fwd == Un, "a settling life-cycle enable is not predicted");

  // lcmgr rules (theory_of_operation "Memory Protection for LCMGR Hardware Plug").
  c = ctrl(h, LcMgr, Seed, 1, 5, false);
  check(allowed(c) && c.scr == Yes && c.ecc == Yes, "PhaseSeed reads the creator page, scrambled, ECC");
  check(denied(ctrl(h, LcMgr, Seed, 1, 6, true)), "PhaseSeed: seed pages read only");
  check(denied(ctrl(h, LcMgr, Seed, 1, 7, false)), "PhaseSeed: no isolated page");
  check(denied(ctrl(h, LcMgr, Seed, 0, 0, false)), "PhaseSeed: no data partition");
  check(allowed(ctrl(h, LcMgr, Rma, 1, 7, true)), "PhaseRma writes the isolated page");
  check(allowed(ctrl(h, LcMgr, Rma, 0, 4089, true)), "PhaseRma writes every non-OTP data page");
  check(denied(ctrl(h, LcMgr, Rma, 0, 4090, true)), "PhaseRma: not the OTP pages");
  check(denied(ctrl(h, LcMgr, Rma, 1, 3, false)), "lcmgr: no rule for info page 3");
  check(denied(ctrl(h, LcMgr, PhNone, 1, 5, false)), "PhaseNone: no rule");
  eg2_rram_mp_set(h, kHwInfo, 0, T | F << 4);
  c = ctrl(h, LcMgr, Rma, 1, 5, false);
  check(c.scr == No && c.ecc == Yes, "HW_INFO_CFG_OVERRIDE.SCRAMBLE_DIS turns scrambling off");
  eg2_rram_mp_set(h, kHwInfo, 0, F | 0x3u << 4);
  c = ctrl(h, LcMgr, Rma, 1, 5, false);
  check(c.scr == Yes && c.ecc == Un, "an invalid ECC_DIS is not predicted");

  // Loopback, other requesters, disables.
  check(ctrl(h, Loop, Inv, 1, 2, true).fwd == Un, "loopback: no documented rule, not predicted");
  check(denied(ctrl(h, Loop, Inv, 1, 9, false)), "loopback: no info page 9");
  check(denied(ctrl(h, Sw, Inv, 0, 7, false, F, true)), "an overflowed address is denied");
  check(denied(ctrl(h, SelNone, Inv, 0, 7, false)), "no requester: denied");
  check(denied(ctrl(h, SwErr, Inv, 0, 7, false)), "SwErr is not a requester: denied");
  check(ctrl(h, Sw, Seed, 0, 7, false).fwd == Un, "a software request with a phase is not predicted");
  c = ctrl(h, Sw, Inv, 0, 7, false, 0x1);
  check(c.err == Yes && c.fwd == Un, "rram_disable not MuBi4False: error, PHY suppression not predicted");
  check(denied(host(h, 7, T)), "rma_sw_dis: host reads answered with an error");
  check(host(h, 7, 0x0).fwd == Un, "an invalid sw_disable is not predicted");
  check(allowed(ctrl(h, Sw, Inv, 0, 7, false)), "rma_sw_dis does not touch the controller path here");
  c = host(h, 7, F, T);
  check(c.err == Yes && c.fwd == Un, "rram_disable: host read answered with an error");
  eg2_rram_mp_release(h);
}

using eg2_model::RramScramble;
using eg2_model::rram_u128;

rram_u128 u128(uint64_t hi, uint64_t lo) { return (rram_u128(hi) << 64) | lo; }

void scramble_tests(bool brk) {
  // PRINCE test vectors of the original paper (eprint 2012/529, appendix A), original key
  // schedule: the C reference prince_ref.h reproduces them.
  struct V {
    uint64_t pt, k0, k1, ct;
  } vec[] = {{0, 0, 0, 0x818665aa0d02dfdaull},
             {~0ull, 0, 0, 0x604ae6ca03c20adaull},
             {0, ~0ull, 0, 0x9fb51935fc3df524ull},
             {0, 0, ~0ull, 0x78a54cbe737bb7efull},
             {0x0123456789abcdefull, 0, 0xfedcba9876543210ull, 0xae25ad3ca8fa9ccfull}};
  for (const V &v : vec) {
    rram_u128 key = u128(v.k0, v.k1);
    check(RramScramble::prince(v.pt, key, false, true) == v.ct, "PRINCE paper vector (encrypt)");
    check(RramScramble::prince(v.ct, key, true, true) == v.pt, "PRINCE paper vector (decrypt)");
    // The improved key schedule alternates k0 and k1: with k0 == k1 it is the original one.
    if (v.k0 == v.k1)
      check(RramScramble::prince(v.pt, key, false) == v.ct, "improved schedule, k0 == k1");
    else
      check(RramScramble::prince(v.pt, key, false) != v.ct, "improved schedule differs, k0 != k1");
  }

  // GF(2^128): x^127 * x = x^128 = x^7 + x^2 + x + 1; 1 is the identity; commutative,
  // distributive.
  rram_u128 x127 = rram_u128(1) << 127;
  check(RramScramble::gf_mul(x127, 2) == (brk ? 0x86 : 0x87), "x^128 reduces to x^7 + x^2 + x + 1");
  check(RramScramble::gf_mul(rram_u128(1) << 64, rram_u128(1) << 64) == 0x87, "x^64 * x^64");
  uint64_t s = 0x243f6a8885a308d3ull;
  auto rnd = [&s]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  for (int i = 0; i < 200; ++i) {
    rram_u128 a = u128(rnd(), rnd()), b = u128(rnd(), rnd()), c = u128(rnd(), rnd());
    check(RramScramble::gf_mul(a, 1) == a, "1 is the identity");
    check(RramScramble::gf_mul(a, b) == RramScramble::gf_mul(b, a), "commutative");
    check(RramScramble::gf_mul(a, b ^ c) == (RramScramble::gf_mul(a, b) ^ RramScramble::gf_mul(a, c)),
          "distributive");
    rram_u128 k = u128(rnd(), rnd()), d = u128(rnd(), rnd());
    check(RramScramble::cipher(RramScramble::cipher(d, k, false), k, true) == d,
          "descramble(scramble(x)) == x (cipher)");
    RramScramble m(k, u128(rnd(), rnd()));
    uint32_t addr = uint32_t(rnd()) & 0x1ffff;
    check(m.descramble(addr, m.scramble(addr, d)) == d, "XEX round trip");
    check(m.scramble(addr, d) != m.scramble(addr ^ 1, d), "the tweak depends on the address");
  }

  // Key registers through DPI.
  uint32_t ra[4] = {1, 2, 3, 4}, rd[4] = {5, 6, 7, 8}, ak[4] = {9, 10, 11, 12},
           dk[4] = {13, 14, 15, 16}, rak[4] = {17, 18, 19, 20}, rdk[4] = {21, 22, 23, 24};
  void *h = eg2_rram_scr_create(ra, rd);
  uint32_t m0[4], m1[4], m2[4];
  eg2_rram_scr_mask(h, 1, m0);
  check(m0[0] == 1 && m0[1] == 2 && m0[2] == 3 && m0[3] == 4, "reset: mask(1) is RndCnstAddrKey");
  eg2_rram_scr_step(h, 0x9, 0, ak, dk, rak, rdk);
  check(eg2_rram_scr_epoch(h) == 0, "no keys_valid, no disable: keys held");
  eg2_rram_scr_step(h, 0x9, 1, ak, dk, rak, rdk);
  eg2_rram_scr_mask(h, 1, m1);
  check(m1[0] == 9 && m1[3] == 12 && eg2_rram_scr_epoch(h) == 1, "keys_valid latches the real keys");
  eg2_rram_scr_step(h, 0x3, 1, ak, dk, rak, rdk);
  eg2_rram_scr_mask(h, 1, m2);
  check(m2[0] == 17 && m2[3] == 20, "keys_disable not MuBi4False wins: the random keys");
  uint32_t din[4] = {0xdeadbeef, 0x01234567, 0x89abcdef, 0x55aa55aa}, enc[4], dec[4];
  eg2_rram_scr_op(h, din, 0, enc);
  eg2_rram_scr_op(h, enc, 1, dec);
  check(dec[0] == din[0] && dec[1] == din[1] && dec[2] == din[2] && dec[3] == din[3],
        "DPI cipher round trip");
  eg2_rram_scr_release(h);
}

}  // namespace

int main() {
  const bool brk = std::getenv("EG2_RRAM_TEST_BREAK") != nullptr;
  mp_tests(false);
  scramble_tests(brk);
  if (brk) mp_tests(true);
  std::printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? 1 : 0;
}
