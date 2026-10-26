#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Generate eg2_pbf_crypto_kat.h, the known answers of the post-boot crypto tests.

Usage: gen_pbf_crypto_kat.py <scratch/nist_cavp> > eg2_pbf_crypto_kat.h

Every value is a NIST CAVP value from the archives OpenTitan pins
(third_party/nist_cavp_testvectors; eg2_top_verification `make nist-cavp` unpacks them), taken by
this script, never copied by hand:

  AES      ECBVarTxt{128,192,256}.rsp: four (X, E_0(X)) pairs per key length, from which the
           tests compose ECB, CBC and CFB-128 chains of 1-4 blocks and single-block OFB and CTR
           (each ciphertext block is a NIST ciphertext); ECBVarKey{128,192,256}.rsp: one
           non-zero key per length.
  SHA-2    SHA{256,384,512}{Short,Long}Msg.rsp: the messages of 0, 1, block - 1 and block bytes
           and the first long message.
  SHA-3    SHA3_{256,512}{Short,Long}Msg.rsp: the same lengths (block = the rate).
  HMAC     HMAC.rsp: the first vector of [L=32] and of [L=64] with a key no longer than the block.
  CTR_DRBG drbgvectors_no_reseed/CTR_DRBG.rsp: the first AES-256 no df vector with a 384-bit
           personalization string and additional input.
  ECDSA    SigGen.txt [P-256,SHA-256]: the first vector (d, k, R, S); SigVer.rsp [P-256,SHA-256]:
           the first two valid and the first two invalid vectors.

The tables hold offsets into one byte pool, no pointers (the CHERIoT build keeps an object with a
pointer out of .rodata).
"""

import hashlib
import sys
from pathlib import Path


def records(path):
    hdr, cur, out = {}, {}, []
    for raw in Path(path).read_text(errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            if not line and cur:
                out.append((dict(hdr), cur))
                cur = {}
            continue
        if line.startswith("["):
            if cur:
                out.append((dict(hdr), cur))
                cur = {}
            body = line[1:line.index("]")]
            if "=" in body:
                k, v = body.split("=", 1)
                hdr[k.strip()] = v.strip()
            else:
                hdr = {"sec": body}
            continue
        k, _, v = line.partition("=")
        k = k.strip()
        # Repeated keys (AdditionalInput twice): keep each.
        while k in cur:
            k += "'"
        cur[k] = v.strip()
    if cur:
        out.append((dict(hdr), cur))
    return out


def unhex(s):
    s = s or ""
    return bytes.fromhex("0" + s if len(s) % 2 else s)


pool = bytearray()
offs = {}


def put(b):
    if not b:
        return 0
    if b in offs:
        return offs[b]
    offs[b] = len(pool)
    pool.extend(b)
    return offs[b]


def main():
    d = Path(sys.argv[1])
    lines = []
    w = lines.append

    # ---- AES
    aes = []
    for bits in (128, 192, 256):
        vt = [r for h, r in records(d / f"nist_cavp_aes_kat/ECBVarTxt{bits}.rsp")
              if h.get("sec") == "ENCRYPT" and "KEY" in r]
        pick = [vt[i] for i in (0, 31, 64, 127)]
        vk = [r for h, r in records(d / f"nist_cavp_aes_kat/ECBVarKey{bits}.rsp")
              if h.get("sec") == "ENCRYPT" and "KEY" in r][bits // 2]
        aes.append((bits, pick, vk))
    w("// AES: per key length, four (X, E_0(X)) pairs (ECBVarTxt, key 0) and one VarKey vector.")
    w("typedef struct eg2_pbf_kat_aes {")
    w("  uint32_t key_bits;")
    w("  uint32_t x[4], y[4];      // offsets of 16-byte X_i and E_0(X_i)")
    w("  uint32_t vk_key, vk_ct;  // VarKey: key (key_bits/8 bytes), plaintext 0, ciphertext")
    w("} eg2_pbf_kat_aes_t;")
    w("static const eg2_pbf_kat_aes_t kEg2PbfKatAes[3] = {")
    for bits, pick, vk in aes:
        xs = ", ".join(str(put(unhex(r["PLAINTEXT"]))) for r in pick)
        ys = ", ".join(str(put(unhex(r["CIPHERTEXT"]))) for r in pick)
        w(f"    {{{bits}, {{{xs}}}, {{{ys}}}, {put(unhex(vk['KEY']))}, "
          f"{put(unhex(vk['CIPHERTEXT']))}}},  // ECBVarTxt{bits} COUNT "
          + " ".join(r["COUNT"] for r in pick) + f", ECBVarKey{bits} COUNT {vk['COUNT']}")
    w("};")
    w("")

    # ---- Hashes
    hashes = []
    for name, short, long_, block, nbytes in (
            ("SHA-256", "nist_cavp_sha2_fips_180_4/SHA256ShortMsg.rsp",
             "nist_cavp_sha2_fips_180_4/SHA256LongMsg.rsp", 64, 32),
            ("SHA-384", "nist_cavp_sha2_fips_180_4/SHA384ShortMsg.rsp",
             "nist_cavp_sha2_fips_180_4/SHA384LongMsg.rsp", 128, 48),
            ("SHA-512", "nist_cavp_sha2_fips_180_4/SHA512ShortMsg.rsp",
             "nist_cavp_sha2_fips_180_4/SHA512LongMsg.rsp", 128, 64),
            ("SHA3-256", "nist_cavp_sha3_fips_202/SHA3_256ShortMsg.rsp",
             "nist_cavp_sha3_fips_202/SHA3_256LongMsg.rsp", 136, 32),
            ("SHA3-512", "nist_cavp_sha3_fips_202/SHA3_512ShortMsg.rsp",
             "nist_cavp_sha3_fips_202/SHA3_512LongMsg.rsp", 72, 64)):
        sv = {int(r["Len"]) // 8: r for h, r in records(d / short) if "Len" in r
              and int(r["Len"]) % 8 == 0}
        lv = [r for h, r in records(d / long_) if "Len" in r]
        for cls, n in (("0", 0), ("1", 1), ("block-1", block - 1), ("block", block)):
            r = sv[n]
            msg = unhex(r["Msg"])[:n]
            hashes.append((name, cls, msg, unhex(r["MD"]), f"{Path(short).name} Len {r['Len']}"))
        r = lv[0]
        msg = unhex(r["Msg"])[:int(r["Len"]) // 8]
        hashes.append((name, "long", msg, unhex(r["MD"]), f"{Path(long_).name} Len {r['Len']}"))
    w("// Hashes: algorithm (0 SHA-256, 1 SHA-384, 2 SHA-512, 3 SHA3-256, 4 SHA3-512), length class")
    w("// (0: 0 bytes, 1: 1, 2: block - 1, 3: block, 4: long), message, digest.")
    w("typedef struct eg2_pbf_kat_hash {")
    w("  uint32_t alg, cls, msg, msg_len, md, md_len;")
    w("} eg2_pbf_kat_hash_t;")
    w(f"static const eg2_pbf_kat_hash_t kEg2PbfKatHash[{len(hashes)}] = {{")
    algs = ["SHA-256", "SHA-384", "SHA-512", "SHA3-256", "SHA3-512"]
    clss = ["0", "1", "block-1", "block", "long"]
    for name, cls, msg, md, src in hashes:
        w(f"    {{{algs.index(name)}, {clss.index(cls)}, {put(msg)}, {len(msg)}, {put(md)}, "
          f"{len(md)}}},  // {name} {cls}: {src}")
    w("};")
    w("")

    # ---- HMAC
    hm = []
    for L, block in ((32, 64), (64, 128)):
        r = next(r for h, r in records(d / "nist_cavp_hmac_fips_198_1/HMAC.rsp")
                 if h.get("L") == str(L) and "Key" in r and int(r["Klen"]) <= block)
        hm.append((L, r))
    w("// HMAC: digest bits, key, message, MAC (Tlen bytes).")
    w("typedef struct eg2_pbf_kat_hmac {")
    w("  uint32_t bits, key, key_len, msg, msg_len, mac, mac_len;")
    w("} eg2_pbf_kat_hmac_t;")
    w("static const eg2_pbf_kat_hmac_t kEg2PbfKatHmac[2] = {")
    for L, r in hm:
        k, m, t = unhex(r["Key"]), unhex(r["Msg"]), unhex(r["Mac"])
        w(f"    {{{L * 8}, {put(k)}, {len(k)}, {put(m)}, {len(m)}, {put(t)}, {len(t)}}},"
          f"  // HMAC.rsp [L={L}] Count {r['Count']}")
    w("};")
    w("")

    # ---- CTR_DRBG
    dr = next(r for h, r in records(
        d / "nist_cavp_drbg_sp_800_90a_root/drbgvectors_no_reseed/CTR_DRBG.rsp")
        if h.get("sec") == "AES-256 no df" and h.get("PersonalizationStringLen") == "384"
        and h.get("AdditionalInputLen") == "384" and "EntropyInput" in r)
    add2 = dr["AdditionalInput'"]
    w("// CTR_DRBG AES-256 no df: EntropyInput (48), PersonalizationString (48), AdditionalInput")
    w("// (48) twice, ReturnedBits (64): instantiate, generate twice, the second output returned.")
    w("typedef struct eg2_pbf_kat_drbg {")
    w("  uint32_t ent, pers, add1, add2, out;")
    w("} eg2_pbf_kat_drbg_t;")
    w(f"static const eg2_pbf_kat_drbg_t kEg2PbfKatDrbg = {{{put(unhex(dr['EntropyInput']))}, "
      f"{put(unhex(dr['PersonalizationString']))}, {put(unhex(dr['AdditionalInput']))}, "
      f"{put(unhex(add2))}, {put(unhex(dr['ReturnedBits']))}}};"
      f"  // CTR_DRBG.rsp (no reseed) AES-256 no df COUNT {dr['COUNT']}")
    w("")

    # ---- ECDSA P-256 SHA-256
    gen = next(r for h, r in records(d / "nist_cavp_ecdsa_fips_186_4/SigGen.txt")
               if h.get("sec") == "P-256,SHA-256" and "k" in r)
    ver = [r for h, r in records(d / "nist_cavp_ecdsa_fips_186_4/SigVer.rsp")
           if h.get("sec") == "P-256,SHA-256" and "Msg" in r]
    good = [r for r in ver if r["Result"].startswith("P")][:2]
    bad = [r for r in ver if r["Result"].startswith("F")][:2]
    w("// ECDSA P-256 with SHA-256: message, public key (Qx, Qy), signature (R, S), 32 bytes each")
    w("// big-endian; d and k for signing; valid = the file's Result; digest = SHA-256(message),")
    w("// computed here on the host as the reference checker does (input preparation, as the")
    w("// crypto library's caller hashes before it signs or verifies).")
    w("typedef struct eg2_pbf_kat_ecdsa {")
    w("  uint32_t msg, msg_len, digest, qx, qy, r, s, d, k, valid;")
    w("} eg2_pbf_kat_ecdsa_t;")
    w("static const eg2_pbf_kat_ecdsa_t kEg2PbfKatEcdsa[5] = {")

    def ec(r, valid, src):
        m = unhex(r["Msg"])
        b32 = lambda k: unhex(r[k]).rjust(32, b"\0") if k in r else b""
        w(f"    {{{put(m)}, {len(m)}, {put(hashlib.sha256(m).digest())}, {put(b32('Qx'))}, {put(b32('Qy'))}, {put(b32('R'))}, "
          f"{put(b32('S'))}, {put(b32('d'))}, {put(b32('k'))}, {valid}}},  // {src}")
    ec(gen, 1, "SigGen.txt [P-256,SHA-256] first")
    for i, r in enumerate(good):
        ec(r, 1, f"SigVer.rsp [P-256,SHA-256] Result {r['Result']}")
    for r in bad:
        ec(r, 0, f"SigVer.rsp [P-256,SHA-256] Result {r['Result']}")
    w("};")
    w("")

    out = ["// Copyright lowRISC contributors (OpenTitan project).",
           "// Licensed under the Apache License, Version 2.0, see LICENSE for details.",
           "// SPDX-License-Identifier: Apache-2.0",
           "//",
           "// ------------------- W A R N I N G: A U T O - G E N E R A T E D   C O D E !! "
           "-------------------//",
           "// Generated by sw/device/tests/eg2_pbf/gen_pbf_crypto_kat.py from the NIST CAVP files "
           "OpenTitan pins",
           "// (third_party/nist_cavp_testvectors); do not edit.",
           "#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_CRYPTO_KAT_H_",
           "#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_CRYPTO_KAT_H_",
           "",
           "#include <stdint.h>",
           "",
           "// clang-format off"]
    out += lines
    out.append(f"static const uint8_t kEg2PbfKatPool[{len(pool)}] = {{")
    for i in range(0, len(pool), 16):
        out.append("    " + ", ".join(f"0x{b:02x}" for b in pool[i:i + 16]) + ",")
    out.append("};")
    out.append("// clang-format on")
    out.append("")
    out.append("#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_CRYPTO_KAT_H_")
    # Each test uses some of the tables only.
    out = [ln.replace(" = {", " __attribute__((unused)) = {", 1) if ln.startswith("static const")
           else ln for ln in out]
    print("\n".join(out))


if __name__ == "__main__":
    main()
