#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Generate the C vector table of an EG2 NIST CAVP chip test from the pinned CAVP files.

Usage: gen_nist_vectors.py --kind <aes|gcm|sha2|hmac|sha3|drbg> --out <header.h>
                           [--random N] <file.rsp> ...

The vectors are never copied by hand: Bazel runs this over the archives OpenTitan pins
(third_party/nist_cavp_testvectors). Each file contributes a pool:

  boundary vectors, always run: the first and last of each section, the shortest and longest
      message (and output), each key, tag and output length the file has, the first FAIL vector of
      each tag length (GCM decryption), and the first COUNT of a Monte Carlo file;
  random vectors: --random per file, chosen with a generator seeded from the file name, so the
      pool is the same in every build. The test draws from them with the run's seed (kEg2Seed).

The header lists every vector in the pool with its file and COUNT (or Len), so a run's report says
exactly which vectors ran. Values are NIST byte strings, in a byte pool addressed by offsets: the
tables hold no pointers (an object with a pointer is not in .rodata in the CHERIoT build).

Kinds and fields (F_* indices into eg2_nist_vec_t.off/len):
  aes   KAT ECB/CBC/CFB128/OFB: KEY IV IN OUT ; param = DIF mode bit ; flag DECRYPT
  gcm   GCM through the crypto library (software, EG2 HLS), IVs of whole 32-bit words (the
        files' 96- and 1024-bit ones): KEY IV IN OUT AAD TAG ; flags DECRYPT, FAIL
  sha2  SHA-256/384/512 short, long, Monte: MSG MD ; param = digest bits ; flag MONTE (MSG = Seed)
  hmac  HMAC with SHA-256/384/512 (L = 32/48/64): KEY MSG MAC ; param = digest bits
  sha3  SHA3-224..512 and SHAKE128/256: MSG MD ; param = bits ; flags SHAKE, MONTE ; SHAKE Monte
        also MINLEN MAXLEN (bytes) in len[] of F_IV and F_AAD
  drbg  CTR_DRBG AES-256 no df: ENT PERS ENT_RS ADD_RS ADD1 ADD2 EPR1 EPR2 OUT ; flags PR, RESEED
  kwp   AES key wrap with padding (SP 800-38F KWP-AE, KWP-AD): KEY IN OUT ; flags DECRYPT, FAIL ;
        param = plaintext length in bits (the section)
  ecdsa SigVer.rsp (verify) and SigGen.txt (sign with the vector's k), P-256 and P-384 with
        SHA-256 and SHA-384: E R S QX QY D0 D1 K0 K1 ; flags SIGN, FAIL ; param = curve bits
  ecdh  KAS_ECC_CDH_PrimitiveTest.txt, P-256 and P-384: QX QY D0 D1 Z ; param = curve bits
  rsa   SigVer15_186-3.rsp (verify) and SigGen15_186-3.txt (sign), PKCS#1 v1.5, 2048 and 3072
        bits, SHA-256/384/512: N E S EM D0 D1 ; flags SIGN, FAIL ; param = modulus bits

For the OTBN kinds the integers are written as the crypto library's drivers take them: little
endian, zero-padded to the driver's buffer (sw/device/lib/crypto/impl/ecc/p256.h, p384.h,
rsa/run_rsa.h), the way the OTBN reference checker writes DMEM
(hw/top_earlgrey/dv/eg2_model/nist/nist_otbn_check.py). The host prepares what the driver's
callers prepare: the message digest (truncated to the curve's order, ECDSA), the EMSA-PKCS1-v1_5
encoding (RSA), and the secret values in the driver's two shares (ECDSA/ECDH d and k: arithmetic
shares mod n of 320 bits (P-256) or 448 bits (P-384); RSA d: two Boolean shares), split with a
generator seeded from the file name. These are inputs; every result the test checks is computed
on the chip.

Vectors a kind cannot run (another curve, hash or modulus; KW without padding; the inverse-cipher
files) are counted per file and listed in kEg2NistNotRun with the reason, so a run reports them
as "not run", never as passed.
"""

import argparse
import hashlib
import random
import re
import zlib
from pathlib import Path

F = {"KEY": 0, "IV": 1, "IN": 2, "OUT": 3, "AAD": 4, "TAG": 5, "MSG": 2, "MD": 3, "MAC": 3,
     "ENT": 0, "PERS": 1, "ENT_RS": 2, "ADD_RS": 3, "ADD1": 4, "ADD2": 5, "EPR1": 6, "EPR2": 7,
     "ROUT": 8,
     # ecdsa
     "E": 0, "R": 1, "S": 2, "QX": 3, "QY": 4, "D0": 5, "D1": 6, "K0": 7, "K1": 8}
# ecdh and rsa use their own indices (F_ECDH, F_RSA).
F_ECDH = {"QX": 0, "QY": 1, "D0": 2, "D1": 3, "Z": 4}
F_RSA = {"N": 0, "E": 1, "S": 2, "EM": 3, "D0": 4, "D1": 5}
NFIELDS = 9
FLAGS = {"BOUNDARY": 1, "DECRYPT": 2, "FAIL": 4, "MONTE": 8, "SHAKE": 16, "PR": 32, "RESEED": 64,
         "SIGN": 128}


def parse(path):
    """Records of an .rsp file: (headers, [(key, value)], line number)."""
    hdr, cur, out, start = {}, [], [], 0
    for n, raw in enumerate(Path(path).read_text(errors="replace").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            if not line and cur:
                out.append((dict(hdr), cur, start))
                cur = []
            continue
        if line.startswith("["):
            if cur:
                out.append((dict(hdr), cur, start))
                cur = []
            body = line[1:line.index("]")]
            if "=" in body:
                k, v = body.split("=", 1)
                hdr[k.strip()] = v.strip()
            elif body in ("ENCRYPT", "DECRYPT"):
                hdr["dir"] = body
            else:
                hdr["alg"] = body
            continue
        if not cur:
            start = n
        if "=" in line:
            k, v = line.split("=", 1)
            cur.append((k.strip(), v.strip()))
        else:
            cur.append((line, ""))
    if cur:
        out.append((dict(hdr), cur, start))
    return out


def get(kv, key, nth=0):
    for k, v in kv:
        if k == key:
            if nth == 0:
                return v
            nth -= 1
    return None


def unhex(s):
    return bytes.fromhex(s) if s else b""


class Vec:
    def __init__(self, fidx, count, line):
        self.file, self.count, self.line = fidx, count, line
        self.flags, self.param = 0, 0
        self.fields = [b""] * NFIELDS
        self.lens = [None] * NFIELDS  # a length without data (SHAKE Monte bounds)
        self.section = ""

    def set(self, name, data):
        self.fields[F[name]] = data


# ---------------------------------------------------------------------------------------------
# Per-kind readers: (vectors, boundary predicate inputs)

def read_aes(path, fidx):
    name = Path(path).name
    mode = {"ECB": 1, "CBC": 2, "CFB128": 4, "OFB": 8}[re.match(r"(ECB|CBC|CFB128|OFB)", name).group(1)]
    vs = []
    for hdr, kv, line in parse(path):
        if get(kv, "KEY") is None:
            continue
        v = Vec(fidx, int(get(kv, "COUNT")), line)
        dec = hdr.get("dir") == "DECRYPT"
        v.param = mode
        v.flags |= FLAGS["DECRYPT"] if dec else 0
        v.set("KEY", unhex(get(kv, "KEY")))
        v.set("IV", unhex(get(kv, "IV") or ""))
        pt, ct = unhex(get(kv, "PLAINTEXT")), unhex(get(kv, "CIPHERTEXT"))
        v.set("IN", ct if dec else pt)
        v.set("OUT", pt if dec else ct)
        v.section = hdr.get("dir", "")
        vs.append(v)
    return vs


def read_gcm(path, fidx):
    # EG2 has GCM in software only (EG2 HLS: "GCM mode through software on Ibex"; AES built with
    # AESGCMEnable 0), the crypto library's aes_gcm over the AES engine (2026-10-26). It takes IVs
    # of any whole number of 32-bit words (its interface is a word buffer), and the files have 96,
    # 1024 and 8: the 96- and 1024-bit IVs run, the 8-bit ones are reported not run (SP 800-38D
    # allows any length; an interface limit of the crypto library, dv_tickets.md F52).
    dec = "Decrypt" in Path(path).name
    vs = []
    for hdr, kv, line in parse(path):
        if get(kv, "Key") is None:
            continue
        if int(hdr.get("IVlen", "0")) % 32 != 0:
            not_run(Path(path).name, "IV of %s bits: the crypto library's aes_gcm takes whole 32-bit "
                    "words" % hdr.get("IVlen"))
            continue
        v = Vec(fidx, int(get(kv, "Count")), line)
        v.flags |= FLAGS["DECRYPT"] if dec else 0
        if any(k == "FAIL" for k, _ in kv):
            v.flags |= FLAGS["FAIL"]
        v.set("KEY", unhex(get(kv, "Key")))
        v.set("IV", unhex(get(kv, "IV")))
        pt, ct = unhex(get(kv, "PT") or ""), unhex(get(kv, "CT") or "")
        v.set("IN", ct if dec else pt)
        v.set("OUT", b"" if v.flags & FLAGS["FAIL"] else (pt if dec else ct))
        v.set("AAD", unhex(get(kv, "AAD") or ""))
        v.set("TAG", unhex(get(kv, "Tag")))
        v.section = "IV%s PT%s AAD%s TAG%s" % (hdr.get("IVlen"), hdr.get("PTlen"), hdr.get("AADlen"),
                                               hdr.get("Taglen"))
        vs.append(v)
    return vs


def read_hash(path, fidx, kind):
    name = Path(path).name
    monte = "Monte" in name
    shake = name.startswith("SHAKE")
    bits = int(re.search(r"(\d+)", name.replace("SHA3_", "").replace("SHAKE", "")).group(1))
    vs, seed, minlen, maxlen = [], None, 0, 0
    for hdr, kv, line in parse(path):
        if monte and get(kv, "COUNT") is None:
            seed = unhex(get(kv, "Seed") or get(kv, "Msg"))
            minlen = int(hdr.get("Minimum Output Length (bits)", "0")) // 8
            maxlen = int(hdr.get("Maximum Output Length (bits)", "0")) // 8
            continue
        if monte:
            if int(get(kv, "COUNT")) != 0:
                continue  # the chip runs the first COUNT: 1000 iterations from the seed
            v = Vec(fidx, 0, line)
            v.flags |= FLAGS["MONTE"]
            v.set("MSG", seed)
            v.set("MD", unhex(get(kv, "MD") or get(kv, "Output")))
            if shake:
                v.lens[F["IV"]], v.lens[F["AAD"]] = minlen, maxlen
        elif get(kv, "Len") is not None:
            n = int(get(kv, "Len")) // 8
            v = Vec(fidx, int(get(kv, "Len")), line)
            v.set("MSG", unhex(get(kv, "Msg"))[:n])
            v.set("MD", unhex(get(kv, "MD") or get(kv, "Output")))
        elif get(kv, "COUNT") is not None and get(kv, "Msg") is not None:
            v = Vec(fidx, int(get(kv, "COUNT")), line)
            v.set("MSG", unhex(get(kv, "Msg")))
            v.set("MD", unhex(get(kv, "Output")))
        else:
            continue
        v.param = bits
        v.flags |= FLAGS["SHAKE"] if shake else 0
        vs.append(v)
    return vs


def read_hmac(path, fidx):
    vs = []
    for hdr, kv, line in parse(path):
        if get(kv, "Count") is None or hdr.get("L") not in ("32", "48", "64"):
            continue
        v = Vec(fidx, int(get(kv, "Count")), line)
        v.param = int(hdr["L"]) * 8
        v.set("KEY", unhex(get(kv, "Key")))
        v.set("MSG", unhex(get(kv, "Msg")))
        v.set("MAC", unhex(get(kv, "Mac")))
        v.section = "L%s K%s T%s" % (hdr["L"], get(kv, "Klen"), get(kv, "Tlen"))
        vs.append(v)
    return vs


def read_drbg(path, fidx):
    vs = []
    for hdr, kv, line in parse(path):
        if hdr.get("alg") != "AES-256 no df" or get(kv, "COUNT") is None:
            continue
        v = Vec(fidx, int(get(kv, "COUNT")), line)
        if hdr.get("PredictionResistance") == "True":
            v.flags |= FLAGS["PR"]
        if get(kv, "EntropyInputReseed") is not None:
            v.flags |= FLAGS["RESEED"]
            v.set("ENT_RS", unhex(get(kv, "EntropyInputReseed")))
            v.set("ADD_RS", unhex(get(kv, "AdditionalInputReseed") or ""))
        v.set("ENT", unhex(get(kv, "EntropyInput")))
        v.set("PERS", unhex(get(kv, "PersonalizationString") or ""))
        v.set("ADD1", unhex(get(kv, "AdditionalInput", 0) or ""))
        v.set("ADD2", unhex(get(kv, "AdditionalInput", 1) or ""))
        if v.flags & FLAGS["PR"]:
            v.set("EPR1", unhex(get(kv, "EntropyInputPR", 0)))
            v.set("EPR2", unhex(get(kv, "EntropyInputPR", 1)))
        v.set("ROUT", unhex(get(kv, "ReturnedBits")))
        v.param = int(hdr["ReturnedBitsLen"])
        v.section = "P%s A%s" % (hdr["PersonalizationStringLen"], hdr["AdditionalInputLen"])
        vs.append(v)
    return vs


# ---------------------------------------------------------------------------------------------
# Key wrap and the OTBN kinds.

# (file name, reason) -> number of vectors not run.
NOT_RUN = {}


def not_run(name, reason, n=1):
    NOT_RUN[(name, reason)] = NOT_RUN.get((name, reason), 0) + n


def hexbytes(s):
    """A CAVS hex string; some have an odd number of digits, the leading zero dropped (as the
    OTBN reference checker and OpenTitan's own parser read them)."""
    return bytes.fromhex("0" + s if len(s) % 2 else s) if s else b""


def le(x, n):
    return x.to_bytes(n, "little")


def read_kwp(path, fidx):
    name = Path(path).name
    dec = "_AD_" in name
    vs = []
    for hdr, kv, line in parse(path):
        if get(kv, "COUNT") is None:
            continue
        bits = int(hdr.get("PLAINTEXT LENGTH", "0"))
        if bits <= 64:
            # SP 800-38F algorithm 5 step 4 / algorithm 6 step 3: a plaintext of at most 64 bits
            # is wrapped with one block cipher call. sw/device/lib/crypto/impl/aes_kwp/aes_kwp.c
            # returns OTCRYPTO_BAD_ARGS for it (fewer than two semiblocks).
            not_run(name, "plaintext of at most 64 bits: the crypto library's aes_kwp refuses it")
            continue
        v = Vec(fidx, int(get(kv, "COUNT")), line)
        v.param = bits
        v.section = str(bits)
        v.set("KEY", unhex(get(kv, "K")))
        p, c = unhex(get(kv, "P") or ""), unhex(get(kv, "C"))
        if dec:
            v.flags |= FLAGS["DECRYPT"]
            if any(k == "FAIL" for k, _ in kv):
                v.flags |= FLAGS["FAIL"]
            v.set("IN", c)
            v.set("OUT", p)
        else:
            v.set("IN", p)
            v.set("OUT", c)
        vs.append(v)
    return vs


# The curves the crypto library has OTBN programs for (run_p256, run_p384):
# (order n, coordinate and scalar bytes, masked-share bytes the driver takes).
CURVES = {
    "P-256": (0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551, 32, 40),
    "P-384": (0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973,
              48, 56),
}
HASHES = {"SHA-256": hashlib.sha256, "SHA-384": hashlib.sha384, "SHA-512": hashlib.sha512}


def masked_shares(x, n, share_bytes, rng):
    """Arithmetic shares x = (s0 + s1) mod n, each share_bytes long (p256_masked_scalar_t)."""
    s1 = rng.getrandbits(8 * share_bytes) % n
    s0 = (x - s1) % n
    return le(s0, share_bytes), le(s1, share_bytes)


def ecdsa_skip_reason(curve, sha):
    if curve not in CURVES:
        return "curve without an OTBN program in the crypto library"
    if sha in ("SHA-1", "SHA-224"):
        return "no SHA-1 or SHA-224 hardware on EG2"
    if sha == "SHA-512":
        return ("SHA-512: on OTBN it differs from SHA-384 only in the truncated digest; "
                "not run, as at the reference level")
    return None


def read_ecdsa(path, fidx, rng):
    name = Path(path).name
    sign = name.startswith("SigGen")
    vs = []
    for hdr, kv, line in parse(path):
        alg = hdr.get("alg", "")
        if get(kv, "Msg") is None or "," not in alg:
            continue
        curve, sha = alg.split(",", 1)
        why = ecdsa_skip_reason(curve, sha)
        if why:
            not_run(name, why)
            continue
        n, cb, share = CURVES[curve]
        v = Vec(fidx, len(vs), line)
        v.param = int(curve[2:])
        v.section = alg + ("" if sign else " " + get(kv, "Result")[:1])
        e = int.from_bytes(HASHES[sha](hexbytes(get(kv, "Msg"))).digest()[:cb], "big")
        v.set("E", le(e, cb))
        v.set("R", le(int(get(kv, "R"), 16), cb))
        v.set("S", le(int(get(kv, "S"), 16), cb))
        v.set("QX", le(int(get(kv, "Qx"), 16), cb))
        v.set("QY", le(int(get(kv, "Qy"), 16), cb))
        if sign:
            v.flags |= FLAGS["SIGN"]
            d0, d1 = masked_shares(int(get(kv, "d"), 16), n, share, rng)
            k0, k1 = masked_shares(int(get(kv, "k"), 16), n, share, rng)
            v.set("D0", d0)
            v.set("D1", d1)
            v.set("K0", k0)
            v.set("K1", k1)
        elif not get(kv, "Result").startswith("P"):
            v.flags |= FLAGS["FAIL"]
        vs.append(v)
    return vs


def read_ecdh(path, fidx, rng):
    name = Path(path).name
    vs = []
    for hdr, kv, line in parse(path):
        if get(kv, "ZIUT") is None:
            continue
        curve = hdr.get("alg", "")
        if curve not in CURVES:
            not_run(name, "curve without an OTBN program in the crypto library")
            continue
        n, cb, share = CURVES[curve]
        v = Vec(fidx, int(get(kv, "COUNT")), line)
        v.param = int(curve[2:])
        v.section = curve
        d0, d1 = masked_shares(int(get(kv, "dIUT"), 16), n, share, rng)
        v.fields[F_ECDH["QX"]] = le(int(get(kv, "QCAVSx"), 16), cb)
        v.fields[F_ECDH["QY"]] = le(int(get(kv, "QCAVSy"), 16), cb)
        v.fields[F_ECDH["D0"]] = d0
        v.fields[F_ECDH["D1"]] = d1
        v.fields[F_ECDH["Z"]] = le(int(get(kv, "ZIUT"), 16), cb)
        vs.append(v)
    return vs


# RFC 8017 9.2: EM = 00 01 FF..FF 00 || DigestInfo || H.
DIGEST_INFO = {
    "SHA256": bytes.fromhex("3031300d060960864801650304020105000420"),
    "SHA384": bytes.fromhex("3041300d060960864801650304020205000430"),
    "SHA512": bytes.fromhex("3051300d060960864801650304020305000440"),
}


def emsa_pkcs1_v15(sha, msg, k):
    t = DIGEST_INFO[sha] + hashlib.new(sha.lower(), msg).digest()
    return b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t


def read_rsa(path, fidx, rng):
    name = Path(path).name
    sign = name.startswith("SigGen")
    vs, ctx = [], {}
    for hdr, kv, line in parse(path):
        for k in ("n", "e", "d"):
            if get(kv, k) is not None:
                ctx[k] = get(kv, k)
        if get(kv, "S") is None or get(kv, "SHAAlg") is None:
            continue
        bits, sha = int(hdr["mod"]), get(kv, "SHAAlg")
        if bits not in (2048, 3072):
            not_run(name, "%d-bit modulus: no custom-exponent mode in run_rsa and no %d-bit RSA in "
                    "the crypto library" % (bits, bits) if bits == 1024 else
                    "%d-bit modulus" % bits)
            continue
        if sha not in DIGEST_INFO:
            not_run(name, "no SHA-1 or SHA-224 hardware on EG2")
            continue
        nb = bits // 8
        v = Vec(fidx, len(vs), line)
        v.param = bits
        v.section = "%d %s %s" % (bits, sha, "" if sign else get(kv, "Result")[:1])
        e = int(ctx["e"], 16)
        v.fields[F_RSA["N"]] = le(int(ctx["n"], 16), nb)
        v.fields[F_RSA["E"]] = le(e, 4)
        v.fields[F_RSA["S"]] = le(int(get(kv, "S"), 16), nb)
        v.fields[F_RSA["EM"]] = emsa_pkcs1_v15(sha, hexbytes(get(kv, "Msg")), nb)[::-1]
        if sign:
            v.flags |= FLAGS["SIGN"]
            d, mask = int(ctx["d"], 16), rng.getrandbits(bits)
            v.fields[F_RSA["D0"]] = le(d ^ mask, nb)
            v.fields[F_RSA["D1"]] = le(mask, nb)
        elif not get(kv, "Result").startswith("P"):
            v.flags |= FLAGS["FAIL"]
        vs.append(v)
    return vs


def count_records(path):
    return sum(1 for _, kv, _ in parse(path) if get(kv, "COUNT") is not None)


# ---------------------------------------------------------------------------------------------
def mark_boundary(vs, kind):
    def mark(v):
        if v is not None:
            v.flags |= FLAGS["BOUNDARY"]

    if not vs:
        return
    sections = {}
    for v in vs:
        sections.setdefault(v.section, []).append(v)
    if kind in ("aes", "drbg", "ecdsa", "ecdh", "rsa", "kwp"):
        for sec in sections.values():
            mark(sec[0])
            mark(sec[-1])
            # Key wrap: the first refusal of each plaintext length (an AD vector marked FAIL).
            mark(next((v for v in sec if v.flags & FLAGS["FAIL"]), None))
    elif kind == "gcm":
        seen = set()
        for v in vs:
            key = (len(v.fields[F["IV"]]), len(v.fields[F["IN"]]), len(v.fields[F["AAD"]]))
            tl = ("tag", len(v.fields[F["TAG"]]), bool(v.flags & FLAGS["FAIL"]))
            if key not in seen and len(v.fields[F["TAG"]]) == 16 and not v.flags & FLAGS["FAIL"]:
                seen.add(key)
                mark(v)
            if tl not in seen:
                seen.add(tl)
                mark(v)
        mark(vs[-1])
    elif kind == "hmac":
        seen = set()
        for v in vs:
            for key in (("L", v.param), ("K", v.param, len(v.fields[F["KEY"]])),
                        ("T", v.param, len(v.fields[F["MAC"]]))):
                if key not in seen:
                    seen.add(key)
                    mark(v)
        mark(vs[-1])
    else:  # sha2, sha3: shortest and longest message and output, block edges, first and last
        msgs = sorted(vs, key=lambda v: len(v.fields[F["MSG"]]))
        mark(msgs[0])
        mark(msgs[-1])
        outs = sorted(vs, key=lambda v: len(v.fields[F["MD"]]))
        mark(outs[0])
        mark(outs[-1])
        mark(vs[0])
        mark(vs[-1])
        bits = vs[0].param
        shake = vs[0].flags & FLAGS["SHAKE"]
        block = (200 - 2 * bits // 8) if (kind == "sha3" or shake) else (64 if bits == 256 else 128)
        by_len = {len(v.fields[F["MSG"]]): v for v in vs}
        for n in (block - 1, block, block + 1, 2 * block - 1, 2 * block):
            mark(by_len.get(n))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kind", required=True,
                    choices=["aes", "gcm", "sha2", "hmac", "sha3", "drbg", "kwp", "ecdsa", "ecdh",
                             "rsa"])
    ap.add_argument("--not-run", action="append", default=[], metavar="FILE",
                    help="a vector file of the algorithm that this kind does not run: its vectors "
                         "are counted into kEg2NistNotRun with the reason")
    ap.add_argument("--out", required=True)
    ap.add_argument("--random", type=int, default=4)
    ap.add_argument("--max-msg", type=int, default=0,
                    help="leave out non-boundary vectors whose message is longer (bytes)")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    pool, table, names = bytearray(), [], []
    offsets = {}

    def put(data):
        if not data:
            return 0
        if data in offsets:
            return offsets[data]
        off = len(pool)
        pool.extend(data)
        offsets[data] = off
        return off

    for fidx, path in enumerate(args.files):
        name = Path(path).name
        if args.kind in ("drbg",):
            name = Path(path).parent.name + "/" + name
        names.append(name)
        # The secret shares: fixed per file, so every build splits them the same way.
        srng = random.Random(zlib.crc32(("shares " + name).encode()))
        vs = {"aes": lambda: read_aes(path, fidx), "gcm": lambda: read_gcm(path, fidx),
              "sha2": lambda: read_hash(path, fidx, "sha2"),
              "sha3": lambda: read_hash(path, fidx, "sha3"),
              "hmac": lambda: read_hmac(path, fidx), "drbg": lambda: read_drbg(path, fidx),
              "kwp": lambda: read_kwp(path, fidx),
              "ecdsa": lambda: read_ecdsa(path, fidx, srng),
              "ecdh": lambda: read_ecdh(path, fidx, srng),
              "rsa": lambda: read_rsa(path, fidx, srng)}[args.kind]()
        if not vs:
            raise SystemExit(f"gen_nist_vectors: no {args.kind} vectors in {path}")
        mark_boundary(vs, args.kind)
        rest = [v for v in vs if not v.flags & FLAGS["BOUNDARY"]]
        if args.max_msg:
            rest = [v for v in rest if len(v.fields[F["MSG"]]) <= args.max_msg]
        rng = random.Random(zlib.crc32(name.encode()))
        chosen = set(id(v) for v in rng.sample(rest, min(args.random, len(rest))))
        for v in vs:
            if v.flags & FLAGS["BOUNDARY"] or id(v) in chosen:
                table.append(v)

    for path in args.not_run:
        name = Path(path).name
        why = ("TDEA key wrap: no TDEA hardware on EG2" if name.startswith("TKW")
               else "the inverse-cipher variant (SP 800-38F section 5.1): the crypto library "
                    "wraps with the forward cipher" if "_inv" in name
               else "KW without padding: the crypto library implements KWP only (impl/aes_kwp)"
               if name.startswith("KW_")
               else "not run by this test")
        not_run(name, why, count_records(path))

    out = []
    out.append("// Generated by sw/device/tests/eg2_nist/gen_nist_vectors.py --kind %s; do not edit."
               % args.kind)
    out.append("// Source: the NIST CAVP files below, as OpenTitan pins them "
               "(third_party/nist_cavp_testvectors).")
    out.append("")
    out.append("#include \"sw/device/tests/eg2_nist/eg2_nist.h\"")
    out.append("")
    out.append("const char kEg2NistFileNames[] =")
    name_off = []
    acc = 0
    for n in names:
        name_off.append(acc)
        out.append('    "%s\\0"' % n)
        acc += len(n) + 1
    out.append("    ;")
    out.append("const uint16_t kEg2NistFileName[] = {%s};" % ", ".join(str(o) for o in name_off))
    out.append("const uint32_t kEg2NistNumFiles = %d;" % len(names))
    out.append("")
    out.append("const eg2_nist_vec_t kEg2NistVecs[] = {")
    for v in table:
        offs = [put(d) for d in v.fields]
        lens = [v.lens[i] if v.lens[i] is not None else len(d) for i, d in enumerate(v.fields)]
        out.append("    {%d, %d, 0x%x, %d, {%s}, {%s}},  // %s line %d"
                   % (v.file, v.count, v.flags, v.param, ", ".join(map(str, offs)),
                      ", ".join(map(str, lens)), names[v.file], v.line))
    out.append("};")
    out.append("const uint32_t kEg2NistNumVecs = %d;" % len(table))
    out.append("")
    # The vectors of the algorithm's files that this test does not run, with the reason.
    out.append("const char kEg2NistNotRun[] =")
    for (fname, why), cnt in sorted(NOT_RUN.items()):
        out.append('    "%s: %d vectors (%s)\\0"' % (fname, cnt, why))
    out.append('    "";')
    out.append("const uint32_t kEg2NistNotRunCount = %d;" % len(NOT_RUN))
    out.append("")
    out.append("const uint8_t kEg2NistPool[%d] = {" % max(1, len(pool)))
    for i in range(0, len(pool), 16):
        out.append("    " + ", ".join("0x%02x" % b for b in pool[i:i + 16]) + ",")
    out.append("};")
    Path(args.out).write_text("\n".join(out) + "\n")
    nb = sum(1 for v in table if v.flags & FLAGS["BOUNDARY"])
    print(f"gen_nist_vectors: {args.kind}: {len(names)} files, {len(table)} vectors "
          f"({nb} boundary), {len(pool)} bytes")


if __name__ == "__main__":
    main()
