#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""nist_otbn_check: the NIST CAVP ECDSA, ECDH and RSA vectors on OTBN's instruction-set simulator.

The reference for OTBN is its ISS (hw/ip/otbn/dv/otbnsim) running the crypto library's OTBN
programs, so before any chip test relies on it, the ISS runs those programs here on the NIST
vectors. Each vector loads the program's ELF (Bazel //sw/otbn/crypto:run_p256, run_p384, run_rsa),
writes the inputs into DMEM by symbol the way the crypto library's driver does
(sw/device/lib/crypto/impl/ecc/p256.c, p384.c, rsa/run_rsa.c), runs the simulator to ECALL and
compares what the driver would read back with the NIST value.

  ECDSA  SigVer.rsp, SigGen.txt (with the vector's k): P-256 and P-384 with SHA-256 and SHA-384.
         Verify: valid if `ok` is true and x_r == r, the driver's rule; sign: MODE_SIGN_CONFIG_K,
         r and s against the vector.
  ECDH   KAS_ECC_CDH_PrimitiveTest.txt, P-256 and P-384: the x-coordinate of d*Q (the XOR of the
         program's two shares) against ZIUT.
  RSA    SigVer15_186-3.rsp, 2048 and 3072 bits with SHA-256, -384, -512: s^e mod n on OTBN
         (MODEXP_F4 for e = 65537, else MODEXP_E with rsa_e, as rsa_modexp_vartime_start picks),
         valid if it equals the EMSA-PKCS1-v1_5 encoding of the digest. With --rsa-sign,
         SigGen15_186-3.txt too: EM^d mod n with the blinded constant-time modexp (MODEXP_DE, d in
         Boolean shares), against S. The files have no 4096-bit vectors.

The messages are hashed on the host (hashlib) and the PKCS#1 encoding is built on the host: that
is input preparation, OTBN computes everything the vector checks. Secret scalars and exponents go
in as the driver's two shares, split with a fixed seed. A run the driver would refuse fails: an
execution error (ERR_BITS), a result word never written, or an INSN_CNT of a constant-time mode
outside the counts the driver accepts (read from the driver sources).

Usage: nist_otbn_check.py [--corrupt] [--jobs N] [--budget N] [--rsa-sign N] --elf-dir DIR
                          <scratch/nist_cavp>
  One line per vector file, curve or modulus, and program: PASS or FAIL with counts; exit 1 on any
  failure. --corrupt flips one bit of the first expected value of every line: then every line must
  report exactly one failure (exit 0 if so, 1 if not), the evidence that each comparison can fail.
  --budget caps the vectors per line and group (hash, expected result): the first, the last and
  evenly spaced between; the line says how many of how many ran.
"""

import argparse
import functools
import hashlib
import multiprocessing
import random
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

_EG2 = Path(__file__).resolve().parents[5]
sys.path[:0] = [str(_EG2 / 'hw/ip/otbn/dv/otbnsim'), str(_EG2 / 'hw/ip/otbn/util'), str(_EG2)]

HARDENED_BOOL_TRUE = 0x739

# ---------------------------------------------------------------------------------------------
# The .rsp/.txt format: "[name = value]" or "[name]" section headers, then records of
# "key = value" lines separated by blank lines.


def parse(path: Path) -> List[Tuple[Dict[str, str], Dict[str, str]]]:
    out = []
    hdr: Dict[str, str] = {}
    cur: Dict[str, str] = {}

    def close() -> None:
        nonlocal cur
        if cur:
            out.append((dict(hdr), cur))
        cur = {}

    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                if not line:
                    close()
                continue
            if line.startswith('['):
                close()
                body = line[1:line.index(']')]
                if '=' in body:
                    k, v = body.split('=', 1)
                    hdr[k.strip()] = v.strip()
                else:
                    hdr = {'alg': body}
                continue
            k, _, v = line.partition('=')
            cur[k.strip()] = v.strip()
    close()
    return out


def hexbytes(s: str) -> bytes:
    """A hex byte string. Some CAVS values have an odd number of digits, the leading zero dropped
    (SigVer15_186-3.rsp has 255-digit messages): put it back, as OpenTitan's own parser does
    (sw/host/cryptotest/testvectors/parsers/cryptotest_util.py, str_to_byte_array)."""
    return bytes.fromhex('0' + s if len(s) % 2 else s)


def be(x: int, n: int) -> bytes:
    return x.to_bytes(n, 'big')


def le(x: int, n: int) -> bytes:
    return x.to_bytes(n, 'little')


# ---------------------------------------------------------------------------------------------
# One OTBN run: runs in a worker process.

_ELF_DIR: Optional[Path] = None


def _init_worker(elf_dir: str) -> None:
    global _ELF_DIR
    _ELF_DIR = Path(elf_dir)


def run_otbn(job: Tuple[str, str, Dict[str, bytes], List[Tuple[str, int]]]
             ) -> Tuple[int, int, int, Dict[str, Optional[int]]]:
    """Run one program to ECALL. job = (elf, mode symbol, {symbol: LE bytes}, [(symbol, bytes)])

    Returns (INSN_CNT, ERR_BITS, STOP_PC, {symbol: value or None if never written}).
    Reads only as many words as the driver reads.
    """
    elf, mode_sym, inputs, reads = job
    try:
        return _run_otbn(job)
    except Exception as e:  # a simulator exception fails the vector, not the whole run
        print(f'nist_otbn_check: {elf} {mode_sym}: otbnsim raised {e!r}', file=sys.stderr)
        return -1, -1, 0, {sym: None for sym, _ in reads}


def _run_otbn(job: Tuple[str, str, Dict[str, bytes], List[Tuple[str, int]]]
              ) -> Tuple[int, int, int, Dict[str, Optional[int]]]:
    from sim.load_elf import load_elf
    from sim.standalonesim import StandaloneSim

    elf, mode_sym, inputs, reads = job
    assert _ELF_DIR is not None
    sim = StandaloneSim()
    load_elf(sim, str(_ELF_DIR / (elf + '.elf')))
    dmem = {'mode': le(sim.symbols[mode_sym], 4)}
    dmem.update(inputs)
    sim.load_dmem_vars(dmem)
    sim.state.ext_regs.commit()
    sim.start(False)
    sim.run(verbose=False, dump_file=None)
    insns = sim.state.ext_regs.read('INSN_CNT', False)
    err = sim.state.ext_regs.read('ERR_BITS', False)
    stop_pc = sim.state.ext_regs.read('STOP_PC', False)
    got: Dict[str, Optional[int]] = {}
    for sym, nbytes in reads:
        base = sim.symbols[sym]
        val, valid = 0, True
        for i in range(nbytes // 4):
            w, v = sim.state.dmem.load_u32(base + 4 * i)
            val |= w << (32 * i)
            valid = valid and v
        got[sym] = val if valid else None
    return insns, err, stop_pc, got


# The instruction counts the driver checks after the constant-time modes: it traps
# (HARDENED_CHECK_EQ on otbn_instruction_count_get()) on any count outside the set its finalize
# function accepts, so such a run fails here too. The ECC finalize functions do not know which
# mode ran and accept the count of any of them (p256_ecdsa_sign_finalize: sideload, config-k or
# random-k signing); the RSA one reads the mode back and accepts that mode's count only. Read from
# the driver sources, so they cannot drift apart.
_DRIVERS = {'run_p256': 'sw/device/lib/crypto/impl/ecc/p256.c',
            'run_p384': 'sw/device/lib/crypto/impl/ecc/p384.c',
            'run_rsa': 'sw/device/lib/crypto/impl/rsa/run_rsa.c'}
# mode: (the mode's own constant, the others its finalize function accepts as well)
_ECC_CNT = {
    'MODE_SIGN_CONFIG_K': ('kModeEcdsaSignConfigKInsCnt',
                           ('kModeEcdsaSignInsCnt', 'kModeEcdsaSignSideloadInsCnt')),
    'MODE_ECDH': ('kModeEcdhInsCnt', ('kModeEcdhSideloadInsCnt',)),
}


@functools.lru_cache(maxsize=None)
def driver_insn_cnt(prog: str, mode: str) -> Tuple[Optional[str], Dict[int, str]]:
    """(the mode's own constant, {accepted count: constant name}); (None, {}) if unchecked."""
    src = (_EG2 / _DRIVERS[prog]).read_text()
    consts = {k: int(v) for k, v in re.findall(r'\b(kMode\w+InsCnt)\s*=\s*(\d+)', src)}
    if prog == 'run_rsa':
        m = re.fullmatch(r'MODE_RSA_(\d+)_MODEXP(?:_(F4|E|DE))?', mode)
        own, also = (f'kModeRsa{m.group(1)}Modexp{m.group(2) or ""}InsCnt', ()) if m else (None, ())
    else:
        own, also = _ECC_CNT.get(mode, (None, ()))
    if own is None:
        return None, {}
    for name in (own,) + also:
        if name not in consts:
            raise SystemExit(f'nist_otbn_check: {name} not found in {_DRIVERS[prog]}')
    return own, {consts[name]: name for name in (own,) + also}


# ---------------------------------------------------------------------------------------------
# Per-line bookkeeping, and --corrupt.


class Line:
    corrupt = False
    bad = 0

    def __init__(self, name: str, budget_note: str = '') -> None:
        self.name = name
        self.note = budget_note
        self.passed = self.failed = 0
        self.first_fail = ''
        self.insn_cnts: List[int] = []
        self.run_errors: List[str] = []
        self.notes: set = set()

    def check(self, got: bytes, exp: bytes, what: str) -> None:
        if Line.corrupt and self.passed + self.failed == 0:
            exp = bytes([exp[0] ^ 1]) + exp[1:] if exp else b'\x00'
        if got == exp:
            self.passed += 1
        else:
            if not self.failed:
                self.first_fail = f'{what}: got {got.hex() or "-"}, expected {exp.hex()}'
            self.failed += 1

    def check_bool(self, got: bool, exp: bool, what: str) -> None:
        self.check(bytes([got]), bytes([exp]), what)

    def done(self) -> None:
        # A run the driver would refuse (execution error, unexpected instruction count) fails the
        # line whatever the comparison said.
        ok = (self.failed == 1 if Line.corrupt else self.failed == 0 and self.passed > 0) \
            and not self.run_errors
        if not ok:
            Line.bad += 1
        lo, hi = min(self.insn_cnts, default=0), max(self.insn_cnts, default=0)
        cnt = f'{lo}' if lo == hi else f'{lo}..{hi}'
        print(f'{"PASS" if ok else "FAIL"} {self.name:<47} {self.passed:4d} passed {self.failed:3d} '
              f'failed{self.note}  (INSN_CNT {cnt})', flush=True)
        for note in sorted(self.notes):
            print(f'     {note}')
        if self.run_errors:
            print(f'     {len(self.run_errors)} runs failed, first: {self.run_errors[0]}')
        if self.failed:
            print(f'     first failure: {self.first_fail}', flush=True)


def pick(items: list, budget: int, key=lambda r: '') -> Tuple[list, str]:
    """At most `budget` vectors of each group (key: the hash, the expected result), the first,
    the last and evenly spaced between; returned in file order."""
    if budget <= 0:
        return items, ''
    groups: Dict[str, List[int]] = {}
    for i, r in enumerate(items):
        groups.setdefault(key(r), []).append(i)
    idx: List[int] = []
    for g in groups.values():
        if len(g) <= budget:
            idx += g
        elif budget == 1:
            idx.append(g[0])
        else:
            idx += sorted({g[round(i * (len(g) - 1) / (budget - 1))] for i in range(budget)})
    idx.sort()
    if len(idx) == len(items):
        return items, ''
    return [items[i] for i in idx], f' (budget: {len(idx)} of {len(items)})'


# ---------------------------------------------------------------------------------------------
# Curves.

CURVES = {
    # name: (program, order n, coordinate bytes, DMEM bytes per scalar/coord buffer,
    #        masked-share bytes the driver writes)
    'P-256': ('run_p256',
              0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551, 32, 32, 40),
    'P-384': ('run_p384',
              0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973,
              48, 64, 56),
}
HASHES = {'SHA-256': hashlib.sha256, 'SHA-384': hashlib.sha384, 'SHA-512': hashlib.sha512}


def digest_int(curve: str, sha: str, msg: bytes) -> int:
    """The ECDSA message representative: the leftmost bits of the hash, as many as n has."""
    h = HASHES[sha](msg).digest()
    nbytes = CURVES[curve][2]
    return int.from_bytes(h[:nbytes], 'big')


def masked_shares(x: int, n: int, share_bytes: int, rng: random.Random) -> Tuple[bytes, bytes]:
    """Arithmetic shares x = (s0 + s1) mod n, each written as the driver does: share_bytes of the
    scalar, zero-padded to the 64-byte DMEM buffer."""
    s1 = rng.getrandbits(8 * share_bytes) % n
    s0 = (x - s1) % n
    return le(s0, 64), le(s1, 64)


def ecdsa_jobs(path: Path, kind: str, budget: int, rng: random.Random):
    """Lines for SigVer.rsp (kind 'ver') or SigGen.txt (kind 'gen')."""
    recs = parse(path)
    lines = []
    for curve in ('P-256', 'P-384'):
        for sha in ('SHA-256', 'SHA-384'):
            sec = [r for h, r in recs if h.get('alg') == f'{curve},{sha}' and 'Msg' in r]
            sec, note = pick(sec, budget, lambda r: r.get('Result', '')[:1])
            prog, n, cb, buf, share = CURVES[curve]
            jobs = []
            for r in sec:
                e = digest_int(curve, sha, hexbytes(r['Msg']))
                inputs = {'msg': le(e, buf)}
                if kind == 'ver':
                    inputs.update({'r': le(int(r['R'], 16), buf), 's': le(int(r['S'], 16), buf),
                                   'x': le(int(r['Qx'], 16), buf), 'y': le(int(r['Qy'], 16), buf)})
                    jobs.append((prog, 'MODE_VERIFY', inputs, [('ok', 4), ('x_r', cb)]))
                else:
                    d0, d1 = masked_shares(int(r['d'], 16), n, share, rng)
                    k0, k1 = masked_shares(int(r['k'], 16), n, share, rng)
                    inputs.update({'d0_io': d0, 'd1_io': d1, 'k0_io': k0, 'k1_io': k1})
                    jobs.append((prog, 'MODE_SIGN_CONFIG_K', inputs, [('r', cb), ('s', cb)]))
            lines.append((f'{path.name} {curve},{sha} [{prog}]', note, sec, jobs,
                          ecdsa_compare(kind, curve)))
    return lines


def ecdsa_compare(kind: str, curve: str):
    cb = CURVES[curve][2]

    def cmp(line: Line, r: Dict[str, str], res) -> None:
        insns, err, _, got = res
        what = f'Msg {r["Msg"][:16]}...'
        if kind == 'ver':
            valid = (err == 0 and got['ok'] == HARDENED_BOOL_TRUE and got['x_r'] is not None
                     and got['x_r'] == int(r['R'], 16))
            line.check_bool(valid, r['Result'].startswith('P'),
                            what + f' Result {r["Result"]} (1 = valid)')
        else:
            sig = (be(got['r'], cb) + be(got['s'], cb)
                   if err == 0 and got['r'] is not None and got['s'] is not None else b'')
            line.check(sig, be(int(r['R'], 16), cb) + be(int(r['S'], 16), cb), what + ' R || S')
    return cmp


def ecdh_jobs(path: Path, budget: int, rng: random.Random):
    recs = parse(path)
    lines = []
    for curve in ('P-256', 'P-384'):
        sec = [r for h, r in recs if h.get('alg') == curve and 'ZIUT' in r]
        sec, note = pick(sec, budget)
        prog, n, cb, buf, share = CURVES[curve]
        jobs = []
        for r in sec:
            d0, d1 = masked_shares(int(r['dIUT'], 16), n, share, rng)
            inputs = {'d0_io': d0, 'd1_io': d1,
                      'x': le(int(r['QCAVSx'], 16), buf), 'y': le(int(r['QCAVSy'], 16), buf)}
            jobs.append((prog, 'MODE_ECDH', inputs,
                         [('ok', 4), ('ecdh_x0', cb), ('ecdh_x1', cb)]))

        def cmp(line: Line, r: Dict[str, str], res, cb=cb) -> None:
            insns, err, _, got = res
            x = (be(got['ecdh_x0'] ^ got['ecdh_x1'], cb)
                 if err == 0 and got['ok'] == HARDENED_BOOL_TRUE
                 and None not in (got['ecdh_x0'], got['ecdh_x1']) else b'')
            line.check(x, be(int(r['ZIUT'], 16), cb), f'COUNT {r["COUNT"]} ZIUT')
        lines.append((f'{path.name} {curve} [{prog}]', note, sec, jobs, cmp))
    return lines


# ---------------------------------------------------------------------------------------------
# RSA PKCS#1 v1.5 (RFC 8017 9.2): EM = 00 01 FF..FF 00 || DigestInfo || H.

DIGEST_INFO = {
    'SHA256': bytes.fromhex('3031300d060960864801650304020105000420'),
    'SHA384': bytes.fromhex('3041300d060960864801650304020205000430'),
    'SHA512': bytes.fromhex('3051300d060960864801650304020305000440'),
}


def emsa_pkcs1_v15(sha: str, msg: bytes, k: int) -> int:
    t = DIGEST_INFO[sha] + hashlib.new(sha.lower(), msg).digest()
    return int.from_bytes(b'\x00\x01' + b'\xff' * (k - len(t) - 3) + b'\x00' + t, 'big')


def rsa_records(path: Path, skipped: Dict[str, int]) -> Dict[int, list]:
    """Records by modulus size, each with the section's n, e and d (they stand in records of
    their own in front of the vectors); vectors OTBN or EG2 cannot run are counted in skipped."""
    ctx: Dict[str, str] = {}
    by_mod: Dict[int, list] = {}
    for h, r in parse(path):
        for k in ('n', 'e', 'd'):
            if k in r:
                ctx[k] = r[k]
        if 'S' not in r:
            continue
        bits = int(h['mod'])
        if bits not in (2048, 3072, 4096) or r['SHAAlg'] not in DIGEST_INFO:
            key = f'{path.name} mod {bits} {r["SHAAlg"]}'
            skipped[key] = skipped.get(key, 0) + 1
            continue
        by_mod.setdefault(bits, []).append(dict(ctx, **r))
    return by_mod


def rsa_ver_jobs(path: Path, budget: int, skipped: Dict[str, int]):
    lines = []
    for bits, sec in sorted(rsa_records(path, skipped).items()):
        sec, note = pick(sec, budget, lambda r: r['SHAAlg'] + r['Result'][:1])
        nb = bits // 8
        jobs = []
        for r in sec:
            # rsa_modexp_vartime_start: F4 for e = 65537, else the custom-exponent mode.
            e = int(r['e'], 16)
            mode = f'MODE_RSA_{bits}_MODEXP_F4' if e == 65537 else f'MODE_RSA_{bits}_MODEXP_E'
            inputs = {'inout': le(int(r['S'], 16), nb), 'rsa_n': le(int(r['n'], 16), nb)}
            if e != 65537:
                inputs['rsa_e'] = le(e, 4)
            jobs.append(('run_rsa', mode, inputs, [('ok', 4), ('inout', nb)]))

        def cmp(line: Line, r: Dict[str, str], res, nb=nb) -> None:
            insns, err, _, got = res
            em = emsa_pkcs1_v15(r['SHAAlg'], hexbytes(r['Msg']), nb)
            valid = err == 0 and got['ok'] == HARDENED_BOOL_TRUE and got['inout'] == em
            line.check_bool(valid, r['Result'].startswith('P'),
                            f'{r["SHAAlg"]} e {r["e"]} S {r["S"][:16]}... Result {r["Result"]} '
                            '(1 = valid)')
        lines.append((f'{path.name} {bits} [run_rsa]', note, sec, jobs, cmp))
    return lines


def rsa_gen_jobs(path: Path, budget: int, skipped: Dict[str, int], rng: random.Random):
    """SigGen15: EM^d mod n with the constant-time, blinded modexp (MODEXP_DE, as
    rsa_modexp_consttime_start picks for a custom e), d in Boolean shares d = d0 ^ d1."""
    lines = []
    for bits, sec in sorted(rsa_records(path, skipped).items()):
        sec, note = pick(sec, budget, lambda r: r['SHAAlg'])
        nb = bits // 8
        jobs = []
        for r in sec:
            d, mask = int(r['d'], 16), rng.getrandbits(bits)
            em = emsa_pkcs1_v15(r['SHAAlg'], hexbytes(r['Msg']), nb)
            inputs = {'inout': le(em, nb), 'rsa_n': le(int(r['n'], 16), nb),
                      'rsa_d0': le(d ^ mask, nb), 'rsa_d1': le(mask, nb),
                      'rsa_e': le(int(r['e'], 16), 4)}
            jobs.append(('run_rsa', f'MODE_RSA_{bits}_MODEXP_DE', inputs,
                         [('ok', 4), ('inout', nb)]))

        def cmp(line: Line, r: Dict[str, str], res, nb=nb) -> None:
            insns, err, _, got = res
            sig = (be(got['inout'], nb) if err == 0 and got['ok'] == HARDENED_BOOL_TRUE
                   and got['inout'] is not None else b'')
            line.check(sig, be(int(r['S'], 16), nb), f'{r["SHAAlg"]} Msg {r["Msg"][:16]}... S')
        lines.append((f'{path.name} {bits} [run_rsa]', note, sec, jobs, cmp))
    return lines


# ---------------------------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('nist_cavp', type=Path, help='scratch/nist_cavp (make nist-cavp)')
    ap.add_argument('--elf-dir', type=Path, required=True,
                    help='directory with run_p256.elf, run_p384.elf, run_rsa.elf '
                         '(bazel-bin/sw/otbn/crypto)')
    ap.add_argument('--corrupt', action='store_true')
    ap.add_argument('--jobs', type=int, default=3, help='simulator processes (at most 3)')
    ap.add_argument('--budget', type=int, default=0,
                    help='vectors per line and group (hash, expected result), 0 = all')
    ap.add_argument('--rsa-sign', type=int, default=0, metavar='N',
                    help='also run SigGen15 (RSA signing: about 8 minutes per 2048-bit '
                         'signature on the ISS, half an hour per 3072-bit), N per modulus and '
                         'hash; 0 = not run')
    ap.add_argument('--only', default='', help='run only lines whose name contains this')
    args = ap.parse_args()
    Line.corrupt = args.corrupt
    jobs_n = max(1, min(args.jobs, 3))
    for elf in ('run_p256', 'run_p384', 'run_rsa'):
        if not (args.elf_dir / (elf + '.elf')).is_file():
            print(f'nist_otbn_check: {args.elf_dir / (elf + ".elf")} missing', file=sys.stderr)
            return 2

    # Fixed seed: the shares differ per vector but every run is the same.
    rng = random.Random(0x0e62)
    d = args.nist_cavp
    lines = []
    lines += ecdsa_jobs(d / 'nist_cavp_ecdsa_fips_186_4/SigVer.rsp', 'ver', args.budget, rng)
    lines += ecdsa_jobs(d / 'nist_cavp_ecdsa_fips_186_4/SigGen.txt', 'gen', args.budget, rng)
    lines += ecdh_jobs(d / 'nist_cavp_ecdh_sp_800_56a/KAS_ECC_CDH_PrimitiveTest.txt',
                       args.budget, rng)
    skipped: Dict[str, int] = {}
    lines += rsa_ver_jobs(d / 'nist_cavp_rsa_fips_186_3/SigVer15_186-3.rsp', args.budget, skipped)
    if args.rsa_sign > 0:
        lines += rsa_gen_jobs(d / 'nist_cavp_rsa_fips_186_3/SigGen15_186-3.txt', args.rsa_sign,
                              skipped, rng)
    if args.only:
        lines = [ln for ln in lines if args.only in ln[0]]

    t0 = time.time()
    total_insns = 0
    with multiprocessing.Pool(jobs_n, initializer=_init_worker,
                              initargs=(str(args.elf_dir.resolve()),)) as pool:
        # Queue every line's runs at once so the workers stay busy; report line by line in order.
        pending = [(name, note, recs, jobs, cmp, pool.map_async(run_otbn, jobs, chunksize=1))
                   for name, note, recs, jobs, cmp in lines]
        for name, note, recs, jobs, cmp, fut in pending:
            line = Line(name, note)
            for r, job, res in zip(recs, jobs, fut.get()):
                insn_cnt, err, stop_pc, got = res
                line.insn_cnts.append(insn_cnt)
                total_insns += max(insn_cnt, 0)
                own, accepted = driver_insn_cnt(job[0], job[1])
                if err or (accepted and insn_cnt not in accepted):
                    line.run_errors.append(
                        f'{job[1]}: ERR_BITS {err:#x}, STOP_PC {stop_pc:#x}, INSN_CNT {insn_cnt}'
                        + (f' (the driver accepts {sorted(accepted)})' if accepted else ''))
                    # A run the driver would not accept gives the driver no result to compare.
                    res = (insn_cnt, err or -2, stop_pc, {k: None for k in got})
                elif accepted and accepted[insn_cnt] != own:
                    line.notes.add(f'note: {job[1]} retires {insn_cnt} instructions, the driver\'s '
                                   f'{accepted[insn_cnt]}, not its {own} '
                                   f'({[c for c, n in accepted.items() if n == own][0]}); '
                                   'the driver accepts either')
                cmp(line, r, res)
            line.done()

    for key, cnt in sorted(skipped.items()):
        why = ('1024-bit modulus: run_rsa has no custom-exponent mode for it and the crypto '
               'library no 1024-bit RSA' if ' mod 1024 ' in key else
               'no SHA-1 or SHA-224 hardware on EG2')
        print(f'     not run: {key}, {cnt} vectors ({why})')
    if not args.rsa_sign:
        print('     not run: SigGen15_186-3.txt (RSA signing; --rsa-sign N to run N per modulus '
              'and hash)')
    print(f'     {total_insns / 1e6:.0f}M OTBN instructions in {time.time() - t0:.0f} s, '
          f'{jobs_n} simulator processes')
    if Line.corrupt:
        print(f'{"FAIL" if Line.bad else "PASS"}: --corrupt, every line must fail exactly once; '
              f'{Line.bad} did not')
    else:
        print(f'{"FAIL" if Line.bad else "PASS"}: {Line.bad} lines failed')
    return 1 if Line.bad else 0


if __name__ == '__main__':
    sys.exit(main())
