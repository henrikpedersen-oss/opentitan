// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// nist_cavp_check: the NIST CAVP test vectors run against the C references the EG2 chip bench and
// SystemC model compute with, before any chip test relies on them.
//
//   AES      hw/ip/aes/model/aes.c (the AES DV's reference), one block; ECB, CBC, CFB128, OFB and
//            GCM (SP 800-38A / 38D) built around it here, the modes EG2's AES has. CFB1 and CFB8
//            are not run: the hardware has neither.
//   SHA-2    hw/ip/hmac/dv/cryptoc_dpi (the HMAC DV's reference, also the model's hmac): SHA-256,
//            -384, -512 short, long and Monte Carlo messages; HMAC with them.
//   SHA-3    SHA3-224..512 and SHAKE128/256 twice: on digestpp (hw/ip/kmac/dv/dpi/vendor, the KMAC
//            DV's reference) and on the model's own sponge (eg2_model/ip/eg2_crypto_hash.h
//            Eg2KeccakSponge, its padding and squeezing).
//   CTR_DRBG AES-256 without derivation function, as csrng runs it: the model's Eg2CtrDrbg
//            (eg2_model/ip/eg2_crypto_entropy.h); no reseed, reseed, and prediction resistance.
//
// Usage: nist_cavp_check [--corrupt] <scratch/nist_cavp>
//   One line per vector file and implementation, PASS or FAIL with counts; exit 1 on any failure.
//   --corrupt flips one bit of the first expected value of every file: then every file must report
//   exactly one failure (exit 0 if so, 1 if not), the evidence that each comparison can fail.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hw/ip/aes/model/aes.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/hmac.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha256.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha384.h"
#include "hw/ip/hmac/dv/cryptoc_dpi/sha512.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"
#include "hw/top_earlgrey/dv/eg2_model/ip/eg2_crypto_entropy.h"
#include "hw/top_earlgrey/dv/eg2_model/ip/eg2_crypto_hash.h"

using Bytes = std::vector<uint8_t>;
using eg2_model::Eg2CtrDrbg;
using eg2_model::Eg2KeccakSponge;

// ---------------------------------------------------------------------------------------------
// The .rsp format: "[name = value]" or "[name]" section headers, then records of "key = value"
// lines separated by blank lines (a key may repeat: the DRBG files' AdditionalInput); a bare
// "FAIL" line marks a GCM decryption that must not authenticate.
struct Record {
  std::map<std::string, std::string> hdr;
  std::vector<std::pair<std::string, std::string>> kv;
  bool has(const std::string &k) const {
    for (auto &p : kv)
      if (p.first == k) return true;
    return false;
  }
  const std::string &get(const std::string &k, unsigned nth = 0) const {
    static const std::string empty;
    for (auto &p : kv)
      if (p.first == k && nth-- == 0) return p.second;
    return empty;
  }
};

static std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

static std::vector<Record> parse(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "nist_cavp_check: cannot read %s\n", path.c_str());
    std::exit(2);
  }
  std::vector<Record> out;
  std::map<std::string, std::string> hdr;
  Record cur;
  auto close = [&] {
    if (!cur.kv.empty()) out.push_back(cur);
    cur = Record();
    cur.hdr = hdr;
  };
  cur.hdr = hdr;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') {
      if (line.empty()) close();
      continue;
    }
    if (line[0] == '[') {
      close();
      std::string body = line.substr(1, line.find(']') - 1);
      size_t eq = body.find('=');
      if (eq == std::string::npos) {
        // [ENCRYPT] / [DECRYPT] and the DRBG's [AES-256 no df]
        if (body == "ENCRYPT" || body == "DECRYPT") hdr["dir"] = body;
        else hdr["alg"] = body;
      } else {
        hdr[trim(body.substr(0, eq))] = trim(body.substr(eq + 1));
      }
      cur.hdr = hdr;
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) cur.kv.emplace_back(line, "");
    else cur.kv.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
  }
  close();
  return out;
}

static Bytes hex(const std::string &s) {
  Bytes b;
  for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(uint8_t(std::stoul(s.substr(i, 2), 0, 16)));
  return b;
}

static std::string tohex(const Bytes &b) {
  std::string s;
  char t[3];
  for (uint8_t x : b) {
    std::snprintf(t, sizeof t, "%02x", x);
    s += t;
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// Per-file bookkeeping, and --corrupt.
static bool g_corrupt = false;
static unsigned g_bad_files = 0;

struct FileCheck {
  std::string name;
  unsigned pass = 0, fail = 0, skip = 0;
  std::string first_fail;
  explicit FileCheck(std::string n) : name(std::move(n)) {}

  void check(const Bytes &got, Bytes exp, const std::string &what) {
    if (g_corrupt && pass + fail == 0) {
      if (exp.empty()) exp.push_back(0);
      else exp[0] ^= 1;
    }
    if (got == exp) {
      ++pass;
    } else {
      if (!fail) first_fail = what + ": got " + tohex(got) + ", expected " + tohex(exp);
      ++fail;
    }
  }
  void check(bool got, bool exp, const std::string &what) {
    check(Bytes{uint8_t(got)}, Bytes{uint8_t(exp)}, what);
  }
  ~FileCheck() {
    bool ok = g_corrupt ? fail == 1 : fail == 0 && pass > 0;
    if (!ok) ++g_bad_files;
    std::printf("%s %-48s %5u passed %3u failed", ok ? "PASS" : "FAIL", name.c_str(), pass, fail);
    if (skip) std::printf(" %3u not applicable", skip);
    std::printf("\n");
    if (fail) std::printf("     first failure: %s\n", first_fail.c_str());
  }
};

static std::string base(const std::string &path) { return path.substr(path.rfind('/') + 1); }

// ---------------------------------------------------------------------------------------------
// AES modes around aes.c's block function.
static Bytes aes_enc(const Bytes &key, const uint8_t *in) {
  Bytes out(16);
  aes_encrypt_block(in, key.data(), int(key.size()), out.data());
  return out;
}
static Bytes aes_dec(const Bytes &key, const uint8_t *in) {
  Bytes out(16);
  aes_decrypt_block(in, key.data(), int(key.size()), out.data());
  return out;
}

static Bytes aes_mode(const std::string &mode, bool enc, const Bytes &key, Bytes iv,
                      const Bytes &in) {
  Bytes out(in.size());
  for (size_t off = 0; off < in.size(); off += 16) {
    const uint8_t *blk = in.data() + off;
    Bytes o;
    if (mode == "ECB") {
      o = enc ? aes_enc(key, blk) : aes_dec(key, blk);
    } else if (mode == "CBC") {
      if (enc) {
        Bytes x(16);
        for (int i = 0; i < 16; ++i) x[i] = blk[i] ^ iv[i];
        o = iv = aes_enc(key, x.data());
      } else {
        o = aes_dec(key, blk);
        for (int i = 0; i < 16; ++i) o[i] ^= iv[i];
        iv.assign(blk, blk + 16);
      }
    } else if (mode == "CFB128") {
      o = aes_enc(key, iv.data());
      for (int i = 0; i < 16; ++i) o[i] ^= blk[i];
      iv = enc ? o : Bytes(blk, blk + 16);
    } else {  // OFB
      iv = aes_enc(key, iv.data());
      o = iv;
      for (int i = 0; i < 16; ++i) o[i] ^= blk[i];
    }
    std::copy(o.begin(), o.end(), out.begin() + off);
  }
  return out;
}

static void run_aes_kat(const std::string &path) {
  std::string f = base(path), mode = f.substr(0, f.find_first_of("GKV"));
  if (mode != "ECB" && mode != "CBC" && mode != "CFB128" && mode != "OFB") return;
  FileCheck fc(f + " [aes.c]");
  for (auto &r : parse(path)) {
    if (!r.has("KEY")) continue;
    bool enc = r.hdr["dir"] == "ENCRYPT";
    Bytes key = hex(r.get("KEY")), iv = hex(r.get("IV"));
    Bytes pt = hex(r.get("PLAINTEXT")), ct = hex(r.get("CIPHERTEXT"));
    std::string what = r.hdr["dir"] + " COUNT " + r.get("COUNT");
    fc.check(aes_mode(mode, enc, key, iv, enc ? pt : ct), enc ? ct : pt, what);
  }
}

// GCM (SP 800-38D): GHASH's multiplication in GF(2^128), Algorithm 1.
static void gf_mul(uint8_t *x, const uint8_t *h) {
  uint8_t z[16] = {}, v[16];
  std::memcpy(v, h, 16);
  for (int i = 0; i < 128; ++i) {
    if (x[i / 8] & (0x80 >> (i % 8)))
      for (int k = 0; k < 16; ++k) z[k] ^= v[k];
    bool lsb = v[15] & 1;
    for (int k = 15; k > 0; --k) v[k] = uint8_t((v[k] >> 1) | (v[k - 1] << 7));
    v[0] >>= 1;
    if (lsb) v[0] ^= 0xe1;
  }
  std::memcpy(x, z, 16);
}

static void ghash_update(uint8_t *y, const uint8_t *h, const Bytes &data) {
  for (size_t off = 0; off < data.size(); off += 16) {
    for (size_t i = 0; i < 16 && off + i < data.size(); ++i) y[i] ^= data[off + i];
    gf_mul(y, h);
  }
}

static void ghash_lengths(uint8_t *y, const uint8_t *h, uint64_t a_bits, uint64_t c_bits) {
  Bytes l(16);
  for (int i = 0; i < 8; ++i) {
    l[7 - i] = uint8_t(a_bits >> (8 * i));
    l[15 - i] = uint8_t(c_bits >> (8 * i));
  }
  ghash_update(y, h, l);
}

// Returns the ciphertext (or plaintext) and the full 16-byte tag over the ciphertext.
static std::pair<Bytes, Bytes> gcm(bool enc, const Bytes &key, const Bytes &iv, const Bytes &in,
                                   const Bytes &aad) {
  uint8_t zero[16] = {};
  Bytes h = aes_enc(key, zero);
  uint8_t j0[16] = {};
  if (iv.size() == 12) {
    std::memcpy(j0, iv.data(), 12);
    j0[15] = 1;
  } else {
    ghash_update(j0, h.data(), iv);
    ghash_lengths(j0, h.data(), 0, uint64_t(iv.size()) * 8);
  }
  auto inc32 = [](uint8_t *cb) {
    for (int i = 15; i >= 12 && ++cb[i] == 0; --i) {
    }
  };
  uint8_t cb[16];
  std::memcpy(cb, j0, 16);
  Bytes out(in.size());
  for (size_t off = 0; off < in.size(); off += 16) {
    inc32(cb);
    Bytes ks = aes_enc(key, cb);
    for (size_t i = 0; i < 16 && off + i < in.size(); ++i) out[off + i] = in[off + i] ^ ks[i];
  }
  const Bytes &c = enc ? out : in;
  uint8_t s[16] = {};
  ghash_update(s, h.data(), aad);
  ghash_update(s, h.data(), c);
  ghash_lengths(s, h.data(), uint64_t(aad.size()) * 8, uint64_t(c.size()) * 8);
  Bytes tag = aes_enc(key, j0);
  for (int i = 0; i < 16; ++i) tag[i] ^= s[i];
  return {out, tag};
}

static void run_aes_gcm(const std::string &path) {
  bool enc = base(path).find("Encrypt") != std::string::npos;
  FileCheck fc(base(path) + " [aes.c]");
  for (auto &r : parse(path)) {
    if (!r.has("Key")) continue;
    Bytes key = hex(r.get("Key")), iv = hex(r.get("IV")), aad = hex(r.get("AAD"));
    Bytes tag = hex(r.get("Tag")), pt = hex(r.get("PT")), ct = hex(r.get("CT"));
    std::string what = "IVlen " + r.hdr["IVlen"] + " PTlen " + r.hdr["PTlen"] + " AADlen " +
                       r.hdr["AADlen"] + " Taglen " + r.hdr["Taglen"] + " Count " + r.get("Count");
    auto res = gcm(enc, key, iv, enc ? pt : ct, aad);
    Bytes got_tag(res.second.begin(), res.second.begin() + tag.size());
    if (enc) {
      Bytes got = res.first;
      got.insert(got.end(), got_tag.begin(), got_tag.end());
      Bytes exp = ct;
      exp.insert(exp.end(), tag.begin(), tag.end());
      fc.check(got, exp, what);
    } else if (r.has("FAIL")) {
      fc.check(got_tag == tag, false, what + " (must not authenticate)");
    } else {
      Bytes got = res.first;
      got.push_back(got_tag == tag);
      Bytes exp = pt;
      exp.push_back(1);
      fc.check(got, exp, what);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// SHA-2 and HMAC on CryptoC.
static Bytes sha2(unsigned bits, const Bytes &m) {
  Bytes d(bits / 8);
  if (bits == 256) SHA256_hash(m.data(), m.size(), d.data());
  else if (bits == 384) SHA384_hash(m.data(), m.size(), d.data());
  else SHA512_hash(m.data(), m.size(), d.data());
  return d;
}

static void run_sha2(const std::string &path, unsigned bits) {
  FileCheck fc(base(path) + " [cryptoc]");
  auto recs = parse(path);
  if (path.find("Monte") != std::string::npos) {
    // SHAVS 6.4: MD0 = MD1 = MD2 = Seed; MDi = SHA(MDi-3 || MDi-2 || MDi-1) up to 1002.
    Bytes seed;
    for (auto &r : recs) {
      if (r.has("Seed")) seed = hex(r.get("Seed"));
      if (!r.has("COUNT")) continue;
      Bytes md[3] = {seed, seed, seed};
      for (int i = 3; i < 1003; ++i) {
        Bytes m = md[0];
        m.insert(m.end(), md[1].begin(), md[1].end());
        m.insert(m.end(), md[2].begin(), md[2].end());
        md[0] = md[1];
        md[1] = md[2];
        md[2] = sha2(bits, m);
      }
      seed = md[2];
      fc.check(seed, hex(r.get("MD")), "COUNT " + r.get("COUNT"));
    }
    return;
  }
  for (auto &r : recs) {
    if (!r.has("Len")) continue;
    Bytes m = hex(r.get("Msg"));
    m.resize(std::stoul(r.get("Len")) / 8);
    fc.check(sha2(bits, m), hex(r.get("MD")), "Len " + r.get("Len"));
  }
}

static void run_hmac(const std::string &path) {
  FileCheck fc(base(path) + " [cryptoc]");
  for (auto &r : parse(path)) {
    if (!r.has("Count")) continue;
    unsigned l = std::stoul(r.hdr["L"]);
    if (l != 32 && l != 48 && l != 64) {  // HMAC-SHA-1 / -224: not in EG2's hmac
      ++fc.skip;
      continue;
    }
    Bytes key = hex(r.get("Key")), msg = hex(r.get("Msg")), mac = hex(r.get("Mac"));
    const uint8_t *p;
    LITE_HMAC_CTX lite;  // SHA-256: a 64-byte block
    HMAC_CTX full;       // SHA-384/512: 128 bytes
    if (l == 32) {
      HMAC_SHA256_init(&lite, key.data(), unsigned(key.size()));
      HMAC_update(&lite, msg.data(), msg.size());
      p = HMAC_final_LITE(&lite);
    } else {
      if (l == 48) HMAC_SHA384_init(&full, key.data(), unsigned(key.size()));
      else HMAC_SHA512_init(&full, key.data(), unsigned(key.size()));
      HMAC_update(&full, msg.data(), msg.size());
      p = HMAC_final(&full);
    }
    fc.check(Bytes(p, p + mac.size()), mac, "L " + r.hdr["L"] + " Count " + r.get("Count"));
  }
}

// ---------------------------------------------------------------------------------------------
// SHA-3 and SHAKE: digestpp, and the model's sponge.
enum class Keccak { kDigestpp, kModel };

static Bytes keccak(Keccak impl, bool shake, unsigned bits, const Bytes &m, size_t out_len) {
  Bytes d(out_len);
  if (impl == Keccak::kDigestpp) {
    if (!shake) digestpp::sha3(bits).absorb(m.data(), m.size()).digest(d.data(), d.size());
    else if (bits == 128) digestpp::shake128().absorb(m.data(), m.size()).squeeze(d.data(), d.size());
    else digestpp::shake256().absorb(m.data(), m.size()).squeeze(d.data(), d.size());
    return d;
  }
  // Eg2Kmac: rate 200 - 2 * capacity/8, domain 0x06 for SHA3 and 0x1f for SHAKE.
  Eg2KeccakSponge s;
  s.reset(200 - 2 * bits / 8);
  s.absorb(m.data(), m.size());
  s.finish(shake ? 0x1f : 0x06);
  for (size_t n = 0; n < out_len;) {
    for (unsigned i = 0; i < s.rate() && n < out_len; ++i) d[n++] = s.byte(i);
    if (n < out_len) s.permute();
  }
  return d;
}

static void run_sha3(const std::string &path, bool shake, unsigned bits, Keccak impl) {
  FileCheck fc(base(path) + (impl == Keccak::kDigestpp ? " [digestpp]" : " [eg2_model sponge]"));
  auto recs = parse(path);
  bool monte = path.find("Monte") != std::string::npos;
  if (monte && !shake) {
    // SHA3VS 6.2.3: MD0 = Seed; MDi = SHA3(MDi-1), 1000 times per COUNT.
    Bytes md;
    for (auto &r : recs) {
      if (r.has("Seed")) md = hex(r.get("Seed"));
      if (!r.has("COUNT")) continue;
      for (int i = 0; i < 1000; ++i) md = keccak(impl, false, bits, md, bits / 8);
      fc.check(md, hex(r.get("MD")), "COUNT " + r.get("COUNT"));
    }
    return;
  }
  if (monte) {
    // SHA3VS 6.3.3: Msgi = the leftmost 128 bits of Outputi-1; Outputi = SHAKE(Msgi, Outputlen);
    // the next Outputlen = min + (the rightmost 16 bits of Outputi mod (max - min + 1)) bytes.
    Bytes out;
    size_t min_b = 0, max_b = 0, len = 0;
    for (auto &r : recs) {
      if (r.has("Msg") && !r.has("COUNT")) {
        out = hex(r.get("Msg"));
        min_b = std::stoul(r.hdr["Minimum Output Length (bits)"]) / 8;
        max_b = std::stoul(r.hdr["Maximum Output Length (bits)"]) / 8;
        len = max_b;
      }
      if (!r.has("COUNT")) continue;
      for (int i = 0; i < 1000; ++i) {
        Bytes m(out.begin(), out.begin() + std::min<size_t>(16, out.size()));
        m.resize(16, 0);
        out = keccak(impl, true, bits, m, len);
        unsigned right = (unsigned(out[out.size() - 2]) << 8) | out[out.size() - 1];
        len = min_b + right % (max_b - min_b + 1);
      }
      Bytes got = out;
      got.push_back(uint8_t(out.size()));  // and its Outputlen, in bytes
      Bytes exp = hex(r.get("Output"));
      exp.push_back(uint8_t(std::stoul(r.get("Outputlen")) / 8));
      fc.check(got, exp, "COUNT " + r.get("COUNT"));
    }
    return;
  }
  for (auto &r : recs) {
    if (!r.has("Msg") || (!r.has("Len") && !r.has("COUNT"))) continue;
    Bytes m = hex(r.get("Msg"));
    if (r.has("Len")) m.resize(std::stoul(r.get("Len")) / 8);
    std::string what = r.has("Len") ? "Len " + r.get("Len") : "COUNT " + r.get("COUNT");
    if (!shake) {
      fc.check(keccak(impl, false, bits, m, bits / 8), hex(r.get("MD")), what);
    } else {
      Bytes exp = hex(r.get("Output"));
      fc.check(keccak(impl, true, bits, m, exp.size()), exp, what);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// CTR_DRBG, AES-256, no derivation function (SP 800-90A 10.2.1), on the model's Eg2CtrDrbg.
static Bytes pad48(const std::string &h) {
  Bytes b = hex(h);
  b.resize(Eg2CtrDrbg::kSeedLen, 0);
  return b;
}
static Bytes xor48(const std::string &a, const std::string &b) {
  Bytes x = pad48(a), y = pad48(b);
  for (unsigned i = 0; i < x.size(); ++i) x[i] ^= y[i];
  return x;
}

// Generate, 10.2.1.5.1: additional input folded in before, and the state updated after.
static Bytes drbg_generate(Eg2CtrDrbg &d, const std::string &adin, size_t nbytes) {
  Bytes a = pad48(adin);
  if (!adin.empty()) d.update(a.data());
  Bytes out;
  while (out.size() < nbytes) {
    uint8_t blk[16];
    d.block(blk);
    out.insert(out.end(), blk, blk + 16);
  }
  out.resize(nbytes);
  d.update(a.data());
  ++d.reseed_counter;
  return out;
}

static void run_drbg(const std::string &path, const std::string &variant) {
  FileCheck fc(variant + "/" + base(path) + " AES-256 no df [eg2_model Eg2CtrDrbg]");
  for (auto &r : parse(path)) {
    if (r.hdr["alg"] != "AES-256 no df" || !r.has("COUNT")) continue;
    bool pr = r.hdr["PredictionResistance"] == "True";
    size_t n = std::stoul(r.hdr["ReturnedBitsLen"]) / 8;
    Eg2CtrDrbg d;
    Bytes seed = xor48(r.get("EntropyInput"), r.get("PersonalizationString"));
    d.instantiate(seed.data(), true);
    if (r.has("EntropyInputReseed")) {
      Bytes rs = xor48(r.get("EntropyInputReseed"), r.get("AdditionalInputReseed"));
      d.reseed(rs.data(), true);
    }
    Bytes got;
    for (unsigned g = 0; g < 2; ++g) {
      std::string adin = r.get("AdditionalInput", g);
      if (pr) {
        // Prediction resistance: a reseed with fresh entropy and the additional input, then a
        // generate without additional input.
        Bytes rs = xor48(r.get("EntropyInputPR", g), adin);
        d.reseed(rs.data(), true);
        adin.clear();
      }
      got = drbg_generate(d, adin, n);
    }
    fc.check(got, hex(r.get("ReturnedBits")), "PR " + r.hdr["PredictionResistance"] + " PersLen " +
                                               r.hdr["PersonalizationStringLen"] + " AddLen " +
                                               r.hdr["AdditionalInputLen"] + " COUNT " + r.get("COUNT"));
  }
}

// The model headers bring SystemC in; libsystemc wants an sc_main, which this program never runs.
int sc_main(int, char **) { return 0; }

// ---------------------------------------------------------------------------------------------
int main(int argc, char **argv) {
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--corrupt")) g_corrupt = true;
    else dir = argv[i];
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: nist_cavp_check [--corrupt] <scratch/nist_cavp>\n");
    return 2;
  }
  auto files = [&](const std::string &sub, const std::vector<std::string> &names) {
    std::vector<std::string> v;
    for (auto &n : names) v.push_back(dir + "/" + sub + "/" + n);
    return v;
  };

  std::vector<std::string> kat;
  for (std::string m : {"ECB", "CBC", "CFB128", "OFB"})
    for (std::string t : {"GFSbox", "KeySbox", "VarKey", "VarTxt"})
      for (std::string k : {"128", "192", "256"}) kat.push_back(m + t + k + ".rsp");
  for (auto &f : files("nist_cavp_aes_kat", kat)) run_aes_kat(f);

  std::vector<std::string> gcm_files;
  for (std::string d : {"gcmEncryptExtIV", "gcmDecrypt"})
    for (std::string k : {"128", "192", "256"}) gcm_files.push_back(d + k + ".rsp");
  for (auto &f : files("nist_cavp_aes_gcm", gcm_files)) run_aes_gcm(f);

  for (unsigned b : {256u, 384u, 512u})
    for (std::string t : {"ShortMsg", "LongMsg", "Monte"})
      run_sha2(dir + "/nist_cavp_sha2_fips_180_4/SHA" + std::to_string(b) + t + ".rsp", b);
  run_hmac(dir + "/nist_cavp_hmac_fips_198_1/HMAC.rsp");

  for (Keccak impl : {Keccak::kDigestpp, Keccak::kModel}) {
    for (unsigned b : {224u, 256u, 384u, 512u})
      for (std::string t : {"ShortMsg", "LongMsg", "Monte"})
        run_sha3(dir + "/nist_cavp_sha3_fips_202/SHA3_" + std::to_string(b) + t + ".rsp", false, b,
                 impl);
    for (unsigned b : {128u, 256u})
      for (std::string t : {"ShortMsg", "LongMsg", "VariableOut", "Monte"})
        run_sha3(dir + "/nist_cavp_shake_fips_202/SHAKE" + std::to_string(b) + t + ".rsp", true, b,
                 impl);
  }

  for (std::string v : {"drbgvectors_no_reseed", "drbgvectors_pr_false", "drbgvectors_pr_true"})
    run_drbg(dir + "/nist_cavp_drbg_sp_800_90a_root/" + v + "/CTR_DRBG.rsp", v);

  if (g_corrupt)
    std::printf("%s: --corrupt, every file must fail exactly once; %u did not\n",
                g_bad_files ? "FAIL" : "PASS", g_bad_files);
  else
    std::printf("%s: %u vector files failed\n", g_bad_files ? "FAIL" : "PASS", g_bad_files);
  return g_bad_files ? 1 : 0;
}
