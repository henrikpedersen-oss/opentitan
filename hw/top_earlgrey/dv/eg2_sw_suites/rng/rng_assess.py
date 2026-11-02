#!/usr/bin/env python3
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Statistical assessment of the EG2 random-number captures (EG2 spec "Random Number Generation
Test Suite" > "Taps and assessment" and "Published test suites", REQ_PBF_24):
`make sw-suite-rng`.

For every `chip_sw_eg2_rng_capture` run found under RUN (a run directory, a log, or a scratch root:
dvsim's */chip_earlgrey_asic-sim-*/*rng_capture*/latest/ and model-tests' *rng_capture*/), the
captured words are extracted (rng_extract.py) and every stream <tap>_<mode> goes through
the tools of its tap (`nix build .#rng-tools`), each with the parameters below. A tool
the data is too short for is SKIPPED with the size it needs, never passed; a tool that is not
built is NOT RUN.

  tool               taps              data it needs           verdict
  sp800-90b    (P)   raw               1,000,000 symbols       ea_non_iid, and ea_iid (accepted
                                                               only if its permutation, chi-square
                                                               and LRS tests pass): PASS if the
                                                               smaller min-entropy per bit >= the
                                                               claim (--claim, 0.375: entropy_src
                                                               README's "non-tolerable defect"
                                                               level of raw min-entropy)
  sp800-90b-restart (P) raw            1000 restarts x 1000    SKIPPED: the capture has no restart
                                                               data (no such capture yet)
  sp800-22     (P)   cond drbg edn     100 x 10^6 bits         STS 2.1.2, all 15 tests, n = 10^6,
                                                               m = bits / 10^6 sequences (multiple
                                                               of 10, at most 1000); see STS below
  ais31              raw (B), cond drbg (A)  B 7.2 Mbit, A 8.31 Mbit  BSI's own pass/fail
  rngtest            cond drbg         20,000 bits             FIPS 140-2 block failures within
                                                               chance (q below)
  dieharder          cond drbg         10^8 bits, more per     each test alone, with fewer
                                       test                    p-samples until it does not rewind
                                                               the file (else skipped); FAIL on
                                                               FAILED
  testu01-rabbit, -alphabit, -blockalphabit  cond drbg  2^20 bits   FAIL on p < 1e-6 or > 1 - 1e-6
  testu01-smallcrush cond drbg         250,000,000 words       as above (never wraps the file)
  practrand          drbg              2^20 bytes              RNG_test -te 1 -tf 2 up to the
                                                               largest 2^k <= size (<= 2^30);
                                                               FAIL on any FAIL evaluation; if
                                                               the expanded set aborts, the core
                                                               set (-te 0) decides; abort = FAIL
  ent                every tap         any                     INFO, no verdict
  (P) = pass criterion of REQ_PBF_24; the others are supplementary: a FAIL there is investigated
  as a finding but does not fail the requirement.

STS (SP 800-22 section 4.2): STS marks a result line with `*` when its proportion of passing
sequences or the uniformity of its P-values is outside the section 4.2 bounds. With 188 lines
(148 of them non-overlapping templates) a perfect generator gets about 0.66 marked lines per run
at m = 100, at least one in about half the runs. So the verdict allows the marked lines chance
gives: FAIL if more lines are marked than a perfect generator exceeds with probability 0.001
(the exact per-line probability from STS's own integer bounds, binomial in the sequences; plus
1e-4 for uniformity), and if any test gave no result.

Must fail: a run of an `_inject_fail` image (every captured word written twice) must make
sp800-22 FAIL (the spec) and rngtest FAIL (its continuous-run test sees the repeated words at the
chip's capture size, where sp800-22 is skipped), and a run with a `pattern:*` noise source must
make sp800-90b FAIL; those verdicts read CAUGHT (failed as it must) or MISSED. The other tools'
verdicts on such runs are shown as they are and grade nothing.

Output (--out, default scratch/rng-assess): <run>/ (the extracted files, rng_capture.json, one
directory per stream with every tool's log), <run>/details.txt (one line per stream and tool),
summary.txt (one line per run and tool) and summary.json. Exit status 1 if a pass criterion
FAILs on a run that must pass, or a must-fail run is MISSED; 2 on bad input.

--selftest: the tools on synthetic streams instead of captures, to show
each can fail: `good` (os.urandom, which is getrandom(), the host source's fallback) must pass,
and the bad streams (biased, stuck bits, a short repeated block, an LFSR, every word twice) must
each fail somewhere. Exit status 1 if a tool fails on good data or never fails on bad data.

Usage: rng_assess.py RUN [--out DIR] [--tools BINDIR] [--jobs 2] [--claim 0.375] [--skip|--only tool,..]
       rng_assess.py --selftest [--out DIR] [--words 3200000] [--smallcrush] [--cases good,dup,..]
"""
import argparse
import array
import concurrent.futures
import json
import math
import os
import re
import resource
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rng_extract  # noqa: E402

# ─── Sizes (each tool's minimum, with where it comes from) ─────────────────────────────────────
EA_SAMPLES = 1_000_000            # SP 800-90B 3.1.1: at least 1,000,000 samples
EA_IID_TIMEOUT = 1800
STS_N = 1_000_000                 # SP 800-22 rev. 1a: n = 10^6 for every test's input-size rule
STS_M_MIN, STS_M_MAX = 100, 1000  # spec: at least 100 sequences; at most 1000 (time)
AIS31_A_BITS = 48 * 65536 + 129 * 40000  # T0: 2^16 x 48 bits; T1-T5: 257 rounds (bit width 1)
AIS31_B_BITS = 7_200_000          # procedure B, fixed in Tester.einlesen
RNGTEST_BLOCK = 20_000            # FIPS 140-2: one test per 20,000 bits
# rngtest's per-block failure rate on good data, measured 2026-10-26: 157 of 199,999 blocks of
# /dev/urandom (monobit 18, poker 22, runs 59, long run 59).
RNGTEST_Q = 0.0008
DIEHARDER_BITS = 10 ** 8          # spec: 10^8 - 10^10 bits for the full set
# (test, ntuple) as `dieharder -a` runs them (-d 14 "Do Not Use" left out). The rgb tests 200-203
# need an explicit -n: alone with the default ntuple 0, -d 201 reports FAILED on /dev/urandom and
# -d 200 refuses to run (self-test, 2026-10-26).
DIEHARDER_TESTS = ([(d, None) for d in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17,
                                         100, 101, 102)]
                   + [(200, n) for n in range(1, 13)] + [(201, n) for n in range(2, 6)]
                   + [(202, n) for n in range(2, 6)] + [(203, n) for n in range(0, 33)]
                   + [(d, None) for d in (204, 205, 206, 207, 208, 209)])
DIEHARDER_MIN_PSAMPLES = 10       # fewer p-values make its KS test meaningless
TESTU01_FILE_BITS = 2 ** 20       # spec: Rabbit and Alphabit, any length >= 2^20
# SmallCrush's demand, measured 2026-10-26 with eg2-testu01 (it counts what it draws): 226.7 M
# 32-bit words on a wrapped 16 MiB file, 227.1 M on /dev/urandom, 232.7 M on duplicated words (the
# gap and coupon-collector tests draw until they are done). 250 M words (1 GB) leaves a margin; a
# run that still wraps is SKIPPED with its count.
SMALLCRUSH_WORDS = 250_000_000
TESTU01_FAIL_P = 1e-6             # TestU01 guide: p outside [1e-10, 1 - 1e-10] is a clear failure,
                                  # [1e-4 .. 1e-10] suspicious; 1e-6 as dieharder's FAILED
PRACTRAND_MIN, PRACTRAND_MAX = 2 ** 20, 2 ** 30  # spec: doubling from 2^20 bytes, up to 2^30
CHANCE = 0.001                    # the false-alarm level of the "within chance" rules
DEFAULT_TIMEOUT = 4 * 3600
MEM_LIMIT = 8_000_000 * 1024      # ulimit -v 8000000 (the laptop's rule), per tool

CRITERIA = ["sp800-90b", "sp800-90b-restart", "sp800-22"]
TOOLS_BY_TAP = {
    "raw": ["sp800-90b", "sp800-90b-restart", "ais31", "ent"],
    "cond": ["sp800-22", "ais31", "rngtest", "dieharder", "testu01-rabbit", "testu01-alphabit",
             "testu01-blockalphabit", "testu01-smallcrush", "ent"],
    "drbg": ["sp800-22", "ais31", "rngtest", "dieharder", "testu01-rabbit", "testu01-alphabit",
             "testu01-blockalphabit", "testu01-smallcrush", "practrand", "ent"],
    "edn": ["sp800-22", "ent"],
}
TOOL_ORDER = ["sp800-90b", "sp800-90b-restart", "sp800-22", "ais31", "rngtest", "dieharder",
              "testu01-rabbit", "testu01-alphabit", "testu01-blockalphabit", "testu01-smallcrush",
              "practrand", "ent"]


class Result:
    def __init__(self, verdict, detail, need=None, data=None):
        self.verdict, self.detail, self.need, self.data = verdict, detail, need, data or {}


def fmt(n):
    return f"{n:,}"


# One thread per tool: the SP 800-90B tools are OpenMP programs that take every core otherwise
# (--jobs is the parallelism; the laptop's rule is at most 3 jobs).
TOOL_ENV = {**os.environ, "OMP_NUM_THREADS": "1"}


def limit_memory():
    resource.setrlimit(resource.RLIMIT_AS, (MEM_LIMIT, MEM_LIMIT))


class Ctx:
    """One stream of one run: its files, its work directory and the tools."""

    def __init__(self, run_dir, name, s, bindir, claim, timeout):
        self.name, self.s, self.tap = name, s, s["tap"]
        self.bits_file = (run_dir / s["files"]["bits"]).resolve()
        self.sym_file = (run_dir / s["files"]["sym4"]).resolve() if "sym4" in s["files"] else None
        self.nbytes = self.bits_file.stat().st_size
        self.nbits = 8 * self.nbytes
        self.work = run_dir / name
        self.work.mkdir(exist_ok=True)
        self.bindir, self.claim, self.timeout = bindir, claim, timeout

    def tool(self, exe):
        p = (self.bindir / exe) if self.bindir else None
        if p and p.exists():
            return str(p)
        return shutil.which(exe) if not self.bindir else None

    def run(self, log, cmd, stdin=None, cwd=None, input_text=None, timeout=None):
        """Runs cmd with its output in <work>/<log>; returns the output."""
        timeout = min(timeout or self.timeout, self.timeout)
        logf = self.work / log
        with open(logf, "w") as out:
            out.write("$ " + " ".join(cmd) + "\n")
            out.flush()
            fin = open(stdin, "rb") if stdin else None
            feed = {"input": input_text.encode()} if input_text is not None else {"stdin": fin}
            try:
                subprocess.run(cmd, **feed, stdout=out, stderr=subprocess.STDOUT, cwd=cwd or self.work,
                               preexec_fn=limit_memory, timeout=timeout, env=TOOL_ENV)
            except subprocess.TimeoutExpired:
                out.write(f"\nrng_assess: timeout after {timeout} s\n")
            finally:
                if fin:
                    fin.close()
        return logf.read_text(errors="replace")


def not_built(exe):
    return Result("NOT RUN", f"{exe} not found (nix build .#rng-tools)")


# ─── SP 800-90B ───────────────────────────────────────────────────────────────────────────────
H_MIN = re.compile(r"min\(H_original, \d X H_bitstring\): (-?[0-9.]+)")
H_ORIG = re.compile(r"^H_original: (-?[0-9.]+)", re.M)


def ea_entropy(text, bits_per_symbol):
    m = H_MIN.search(text) if bits_per_symbol > 1 else None
    m = m or H_ORIG.search(text)
    return float(m.group(1)) / bits_per_symbol if m else None


def t_sp800_90b(c):
    if c.sym_file is None:
        return Result("NOT RUN", "no symbol file")
    n = c.s["symbols"]
    if n < EA_SAMPLES:
        return Result("SKIPPED", f"{fmt(n)} symbols, needs {fmt(EA_SAMPLES)} ({fmt(EA_SAMPLES // 8)} raw words)",
                      need=EA_SAMPLES // 8)
    ni, ii = c.tool("ea_non_iid"), c.tool("ea_iid")
    if not ni or not ii:
        return not_built("ea_non_iid/ea_iid")
    # The first 1,000,000 symbols: 90B's sample size (more only makes the estimators slower).
    sel = ["-l", f"0,{EA_SAMPLES}"] if n > EA_SAMPLES else []
    link = c.work / "raw.sym4"
    if not link.exists():
        link.symlink_to(c.sym_file)
    t_ni = c.run("ea_non_iid.log", [ni, *sel, "raw.sym4", "4"])
    # ea_iid only decides whether the IID estimate may be used; on dependent data its permutation
    # tests can run for hours single-threaded (a repeated-word stream: over 18 minutes,
    # 2026-10-26), and a timeout counts as "IID rejected", the conservative side.
    t_ii = c.run("ea_iid.log", [ii, *sel, "raw.sym4", "4"], timeout=EA_IID_TIMEOUT)
    h_ni = ea_entropy(t_ni, 4)
    if h_ni is None:
        return Result("FAIL", "ea_non_iid gave no estimate (see ea_non_iid.log)")
    iid = all(f"** Passed {t}" in t_ii for t in ("chi square tests", "length of longest repeated substring test",
                                                 "IID permutation tests"))
    h_ii = ea_entropy(t_ii, 4) if iid else None
    h = min(h_ni, h_ii) if h_ii is not None else h_ni
    detail = (f"min-entropy {h:.4f} bits/bit (non-IID {h_ni:.4f}, IID "
              f"{f'{h_ii:.4f}' if h_ii is not None else 'track rejected'}), claim {c.claim}")
    return Result("PASS" if h >= c.claim else "FAIL", detail)


def t_restart(c):
    return Result("SKIPPED", "no restart data: SP 800-90B 3.1.4 needs 1000 restarts x 1000 samples, "
                  "which no capture produces yet")


# ─── SP 800-22 ────────────────────────────────────────────────────────────────────────────────
STS_DIRS = ["Frequency", "BlockFrequency", "Runs", "LongestRun", "Rank", "FFT", "NonOverlappingTemplate",
            "OverlappingTemplate", "Universal", "LinearComplexity", "Serial", "ApproximateEntropy",
            "CumulativeSums", "RandomExcursions", "RandomExcursionsVariant"]
STS_LINES = {"Frequency": 1, "BlockFrequency": 1, "CumulativeSums": 2, "Runs": 1, "LongestRun": 1,
             "Rank": 1, "FFT": 1, "NonOverlappingTemplate": 148, "OverlappingTemplate": 1,
             "Universal": 1, "ApproximateEntropy": 1, "RandomExcursions": 8,
             "RandomExcursionsVariant": 18, "Serial": 2, "LinearComplexity": 1}


def binom_pmf(n, k, p):
    return math.comb(n, k) * p ** k * (1 - p) ** (n - k)


def sts_line_q(m):
    """P(a perfect generator's line is marked) with m sequences: STS's integer proportion bounds
    (assess.c computeMetrics) on Binomial(m, 0.99), plus 1e-4 for the uniformity when computed."""
    if m <= 0:
        return 0.0
    a = 0.01
    lo = int((1 - a - 3 * math.sqrt((1 - a) * a / m)) * m)
    hi = int((1 - a + 3 * math.sqrt((1 - a) * a / m)) * m)
    q = sum(binom_pmf(m, k, 1 - a) for k in range(m + 1) if k < lo or k > hi)
    return q + (1e-4 if m // 10 > 0 else 0.0)


def chance_max(qs, alpha=CHANCE):
    """Largest k a perfect generator exceeds with probability < alpha (Poisson-binomial of qs)."""
    dist = [1.0]
    for q in qs:
        nd = [0.0] * (len(dist) + 1)
        for k, p in enumerate(dist):
            nd[k] += p * (1 - q)
            nd[k + 1] += p * q
        dist = nd
    tail = 0.0
    for k in range(len(dist) - 1, -1, -1):
        tail += dist[k]
        if tail >= alpha:
            return k
    return 0


def t_sp800_22(c):
    m = min(c.nbits // STS_N, STS_M_MAX) // 10 * 10
    if m < STS_M_MIN:
        return Result("SKIPPED", f"{fmt(c.nbits)} bits, needs {STS_M_MIN} x 10^6 = {fmt(STS_M_MIN * STS_N)} bits "
                      f"({fmt(STS_M_MIN * STS_N // 32)} words)", need=STS_M_MIN * STS_N // 32)
    exe = c.tool("sts-assess")
    if not exe:
        return not_built("sts-assess")
    w = c.work / "sts"
    w.mkdir(exist_ok=True)
    tmpl = Path(exe).resolve().parent.parent / "share/nist-sts/templates"
    for name, target in (("templates", tmpl), ("data.bin", c.bits_file)):
        if not (w / name).exists():
            (w / name).symlink_to(target)
    for d in STS_DIRS:
        (w / "experiments/AlgorithmTesting" / d).mkdir(parents=True, exist_ok=True)
    # Answers to assess's questions: generator 0 (input file), the file, 1 (every test), 0 (no
    # parameter change), m bitstreams, 1 (binary).
    c.run("sts.log", [exe, str(STS_N)], cwd=w, input_text=f"0\ndata.bin\n1\n0\n{m}\n1\n")
    rep = w / "experiments/AlgorithmTesting/finalAnalysisReport.txt"
    if not rep.exists():
        return Result("FAIL", "STS wrote no finalAnalysisReport.txt (see sts.log)")
    shutil.copy(rep, c.work / "sts_finalAnalysisReport.txt")
    lines, marked, sizes, empty, got = [], 0, [], 0, {}
    for line in rep.read_text(errors="replace").splitlines():
        tok = line.split()
        if len(tok) < 12 or not all(t.isdigit() for t in tok[:10]):
            continue
        name = tok[-1]
        got[name] = got.get(name, 0) + 1
        prop = next((t for t in tok[10:-1] if "/" in t), None)
        if prop is None:  # " ------ ": no sequence gave this test a result
            empty += 1
            continue
        sizes.append(int(prop.split("/")[1]))
        if "*" in tok[10:-1]:
            marked += 1
            lines.append(line.strip())
    missing = [t for t, n in STS_LINES.items() if got.get(t, 0) < n]
    allowed = chance_max([sts_line_q(s) for s in sizes])
    detail = (f"m={m} x n=10^6: {marked} of {len(sizes)} result lines outside the 4.2 bounds "
              f"(chance allows {allowed})")
    if empty:
        detail += f", {empty} lines without a result"
    if missing:
        return Result("FAIL", detail + f"; tests without their results: {', '.join(missing)}")
    if marked > allowed:
        worst = sorted({re.sub(r'\s+', ' ', x).split(' ')[-1] for x in lines})
        return Result("FAIL", detail + f"; marked: {', '.join(worst)}")
    return Result("PASS", detail)


# ─── AIS 31 ───────────────────────────────────────────────────────────────────────────────────
def t_ais31(c):
    proc, need = ("B", AIS31_B_BITS) if c.tap == "raw" else ("A", AIS31_A_BITS)
    if c.nbits < need:
        return Result("SKIPPED", f"procedure {proc}: {fmt(c.nbits)} bits, needs {fmt(need)} "
                      f"({fmt(-(-need // 32))} words)", need=-(-need // 32))
    exe = c.tool("ais31")
    if not exe:
        return not_built("ais31")
    w = c.work / "ais31"
    w.mkdir(exist_ok=True)
    if not (w / "data.bin").exists():
        (w / "data.bin").symlink_to(c.bits_file)
    t = c.run("ais31.log", [exe, "data.bin", proc], cwd=w)
    m = re.search(rf"EG2_AIS31 {proc} (PASS|FAIL)", t)
    if not m:
        return Result("FAIL", "ais31 gave no verdict (see ais31.log)")
    failed = [ln.strip() for ln in t.splitlines() if "nicht bestanden" in ln]
    tests = sorted({re.search(r"(T\d[ab]?|Durchlauf \d+)", x).group(1) for x in failed
                    if re.search(r"(T\d[ab]?|Durchlauf \d+)", x)})
    detail = f"procedure {proc} (BSI V1 reference)" + (f": failed {', '.join(tests[:8])}" if failed else "")
    return Result(m.group(1), detail)


# ─── rngtest ──────────────────────────────────────────────────────────────────────────────────
def t_rngtest(c):
    if c.nbits < RNGTEST_BLOCK:
        return Result("SKIPPED", f"{fmt(c.nbits)} bits, needs {fmt(RNGTEST_BLOCK)}", need=RNGTEST_BLOCK // 32)
    exe = c.tool("rngtest")
    if not exe:
        return not_built("rngtest")
    t = c.run("rngtest.log", [exe], stdin=c.bits_file)
    ok = re.search(r"successes: (\d+)", t)
    bad = re.search(r"failures: (\d+)", t)
    if not ok or not bad:
        return Result("FAIL", "rngtest gave no counts (see rngtest.log)")
    n, k = int(ok.group(1)) + int(bad.group(1)), int(bad.group(1))
    allowed = chance_max([RNGTEST_Q] * n)
    kinds = ", ".join(f"{a} {b}" for a, b in re.findall(r"\) (Monobit|Poker|Runs|Long run|Continuous run): (\d+)", t)
                      if b != "0")
    detail = f"{k} of {n} blocks failed (chance allows {allowed})" + (f": {kinds}" if kinds else "")
    return Result("FAIL" if k > allowed else "PASS", detail)


# ─── Dieharder ────────────────────────────────────────────────────────────────────────────────
DH_LINE = re.compile(r"^\s*(\S+)\|\s*(\d+)\|\s*(\d+)\|\s*(\d+)\|\s*([0-9.]+)\|\s*(PASSED|WEAK|FAILED)", re.M)
DH_REWOUND = re.compile(r"rewound (\d+) times")


def t_dieharder(c):
    if c.nbits < DIEHARDER_BITS:
        return Result("SKIPPED", f"{fmt(c.nbits)} bits, needs {fmt(DIEHARDER_BITS)} "
                      f"({fmt(DIEHARDER_BITS // 32)} words)", need=DIEHARDER_BITS // 32)
    exe = c.tool("dieharder")
    if not exe:
        return not_built("dieharder")
    ran, skipped, failed, weak = 0, [], [], 0
    for d, n in DIEHARDER_TESTS:
        base = [exe, "-g", "201", "-f", str(c.bits_file), "-d", str(d)] + (["-n", str(n)] if n is not None else [])
        log = f"dieharder_{d}" + (f"_n{n}" if n is not None else "") + ".log"
        d = f"{d}" + (f"/{n}" if n is not None else "")
        t = c.run(log, base)
        res = DH_LINE.findall(t)
        # A result counts only from a run that never rewound the file. Rewound r times, the run is
        # repeated with psamples / (r + 1) p-values, until it fits (the count with an explicit -p
        # differs from the default's: measured 2026-10-26, birthdays rewound 4 times without -p,
        # 32 times with -p 100, so the first estimate is refined).
        for _ in range(4):
            r = DH_REWOUND.search(t)
            if not (r and int(r.group(1)) > 0 and res):
                break
            p = int(res[0][3]) // (int(r.group(1)) + 1)
            if p < DIEHARDER_MIN_PSAMPLES:
                break
            t = c.run(log, base + ["-p", str(p)])
            res = DH_LINE.findall(t)
        r = DH_REWOUND.search(t)
        if r and int(r.group(1)) > 0:
            skipped.append(str(d))
            continue
        if not res:
            skipped.append(f"{d}(no result)")
            continue
        ran += 1
        failed += [f"{x[0]}:{x[1]}" for x in res if x[5] == "FAILED"]
        weak += sum(1 for x in res if x[5] == "WEAK")
    if ran == 0:
        return Result("SKIPPED", "every test would rewind the file", need=None)
    sk = " ".join(skipped[:10]) + (" ..." if len(skipped) > 10 else "")
    detail = (f"{ran} of {len(DIEHARDER_TESTS)} test/ntuple runs, {weak} WEAK"
              + (f"; {len(skipped)} skipped for too little data (-d {sk})" if skipped else ""))
    if failed:
        return Result("FAIL", detail + f"; FAILED: {', '.join(failed[:8])}")
    return Result("PASS", detail)


# ─── TestU01 ──────────────────────────────────────────────────────────────────────────────────
def t_testu01(c, battery):
    need_words = SMALLCRUSH_WORDS if battery == "smallcrush" else TESTU01_FILE_BITS // 32
    if c.nbits // 32 < need_words:
        return Result("SKIPPED", f"{fmt(c.nbits // 32)} words, needs {fmt(need_words)}", need=need_words)
    exe = c.tool("eg2-testu01")
    if not exe:
        return not_built("eg2-testu01")
    t = c.run(f"testu01_{battery}.log", [exe, battery, str(c.bits_file)])
    m = re.search(r"EG2_TESTU01 words (\d+) of (\d+) exhausted (\d)", t)
    if not m:
        return Result("FAIL", f"eg2-testu01 gave no result (see testu01_{battery}.log)")
    if m.group(3) == "1":
        return Result("SKIPPED", f"the battery drew {fmt(int(m.group(1)))} words of {fmt(int(m.group(2)))}",
                      need=int(m.group(1)))
    pv = [(float(p), name) for p, name in re.findall(r"EG2_TESTU01 PVAL \d+ (\S+) (.*)", t)]
    done = [(p, n) for p, n in pv if p >= 0]
    bad = [f"{n.strip()} p={p:.2g}" for p, n in done if p < TESTU01_FAIL_P or p > 1 - TESTU01_FAIL_P]
    sus = sum(1 for p, _ in done if p < 1e-3 or p > 1 - 1e-3)
    detail = f"{len(done)} p-values, {sus} outside [0.001, 0.999]"
    if not done:
        return Result("FAIL", "no p-values")
    return Result("FAIL", detail + f"; failed: {'; '.join(bad[:6])}") if bad else Result("PASS", detail)


# ─── PractRand ────────────────────────────────────────────────────────────────────────────────
def practrand_verdict(t):
    """(verdict or None if RNG_test aborted, detail)."""
    lengths = re.findall(r"length= .*?\(2\^(\d+) bytes\)", t)
    if re.search(r"^error:|an error occurred", t, re.M):
        return None, (re.search(r"^error: (.*)", t, re.M) or [None, "aborted"])[1]
    if not lengths:
        return "FAIL", "RNG_test reported no length (see practrand.log)"
    fails = [ln.split()[0] for ln in t.splitlines() if re.search(r"\bFAIL\b", ln)]
    if fails:
        first = re.search(r"length= .*?\(2\^(\d+) bytes\)(?:(?!length=).)*?\bFAIL\b", t, re.S)
        at = f" at 2^{first.group(1)} bytes" if first else ""
        return "FAIL", f"FAIL{at}: {', '.join(sorted(set(fails))[:6])}"
    unusual = sum(1 for ln in t.splitlines() if re.search(r"unusual|suspicious", ln))
    return "PASS", f"no failure up to 2^{lengths[-1]} bytes ({unusual} unusual/suspicious results)"


def t_practrand(c):
    if c.nbytes < PRACTRAND_MIN:
        return Result("SKIPPED", f"{fmt(c.nbytes)} bytes, needs {fmt(PRACTRAND_MIN)}", need=PRACTRAND_MIN // 4)
    exe = c.tool("RNG_test")
    if not exe:
        return not_built("RNG_test")
    tlmax = min(2 ** int(math.log2(c.nbytes)), PRACTRAND_MAX)
    cmd = [exe, "stdin32", "-tlmin", "1MB", "-tlmax", str(int(math.log2(tlmax))), "-tf", "2"]
    v, d = practrand_verdict(c.run("practrand.log", cmd + ["-te", "1"], stdin=c.bits_file))
    if v is not None:
        return Result(v, d)
    # The expanded set (-te 1) stops on data far from random ("garbage p-value" in DC7, seen on
    # every bad self-test stream, 2026-10-26); the core set then gives the evaluations. An abort
    # there too is a failure, never a pass.
    v, d2 = practrand_verdict(c.run("practrand_core.log", cmd + ["-te", "0"], stdin=c.bits_file))
    if v is None:
        return Result("FAIL", f"RNG_test aborted with both test sets: {d}; {d2}")
    return Result(v, f"expanded set aborted ({d}); core set: {d2}")


# ─── ENT ──────────────────────────────────────────────────────────────────────────────────────
def t_ent(c):
    exe = c.tool("ent")
    if not exe:
        return not_built("ent")
    t = c.run("ent.log", [exe, str(c.bits_file)])
    pats = {"entropy": r"Entropy = ([0-9.]+)", "chi_pct": r"would exceed this value (?:(?:less|more) than (?:than )?)?([0-9.]+) percent",
            "mean": r"data bytes is ([0-9.]+)", "pi_err": r"error ([0-9.]+) percent",
            "serial": r"coefficient is ([-0-9.]+)"}
    d = {k: float(m.group(1)) for k, r in pats.items() if (m := re.search(r, t, re.S))}
    if len(d) < len(pats):
        return Result("INFO", "ent gave no figures (see ent.log)")
    return Result("INFO", f"{d['entropy']:.6f} bits/byte, chi-square exceeded {d['chi_pct']:.2f}% of the "
                  f"time, mean {d['mean']:.4f}, pi error {d['pi_err']:.2f}%, serial correlation "
                  f"{d['serial']:.6f}", data=d)


FUNCS = {"sp800-90b": t_sp800_90b, "sp800-90b-restart": t_restart, "sp800-22": t_sp800_22,
         "ais31": t_ais31, "rngtest": t_rngtest, "dieharder": t_dieharder,
         "testu01-rabbit": lambda c: t_testu01(c, "rabbit"),
         "testu01-alphabit": lambda c: t_testu01(c, "alphabit"),
         "testu01-blockalphabit": lambda c: t_testu01(c, "blockalphabit"),
         "testu01-smallcrush": lambda c: t_testu01(c, "smallcrush"),
         "practrand": t_practrand, "ent": t_ent}


# ─── Runs ─────────────────────────────────────────────────────────────────────────────────────
def find_runs(path):
    """Capture runs under path: the path itself if it holds EG2_RNG lines, else the capture runs
    of a scratch root (dvsim and model-tests layouts)."""
    if path.is_file() or (path / "run.log").is_file() or any(path.glob("rng_capture.json")):
        return [path]
    # A capture run directory (dvsim: <n>.<test>/latest; eg2sim: <image>/) at depth 1 to 3, so
    # RUN may be the scratch root, a branch's or tool's directory, or model-tests/rng-model.
    pats = [f"{'*/' * d}*rng_capture*{tail}" for d in range(3) for tail in ("/latest", "")]
    seen, runs = set(), []
    for p in pats:
        for d in sorted(path.glob(p)):
            if d.is_dir() and (d / "run.log").is_file() and d.resolve() not in seen:
                seen.add(d.resolve())
                runs.append(d)
    return runs


def run_name(path):
    p = path.parent if path.is_file() or path.name == "latest" else path
    if p.name == "latest":
        p = p.parent
    tool = next((x.split("-sim-")[1] for x in path.parts if "-sim-" in x), None)
    return p.name + (f".{tool}" if tool else "")


def expectations(meta):
    """{tool: must fail} for a must-fail run, else {}."""
    must = {}
    if meta.get("inject_fail") or meta.get("expect") == "inject":
        # The spec's tool, and rngtest, which sees the repeated words at the chip's capture size
        # (its continuous-run test compares consecutive 32-bit words).
        must["sp800-22"] = must["rngtest"] = True
    if str(meta.get("source", "")).startswith("pattern:"):
        must["sp800-90b"] = True
    return must


def grade(verdict, expect_fail):
    if not expect_fail:
        return verdict
    return {"FAIL": "CAUGHT", "PASS": "MISSED"}.get(verdict, verdict)


RANK = ["FAIL", "MISSED", "CAUGHT", "PASS", "INFO", "SKIPPED", "NOT RUN"]


def aggregate(tool, rows, must):
    """One line per tool over the streams it applies to."""
    if not rows:
        return None
    vs = [r[1] for r in rows]
    top = min(vs, key=RANK.index)
    names = lambda v: " ".join(n for n, x, _ in rows if x == v)  # noqa: E731
    if top in ("SKIPPED", "NOT RUN"):
        need = max((r[2].need or 0 for r in rows), default=0)
        detail = rows[0][2].detail if len(rows) == 1 else f"{len(rows)} streams: {rows[0][2].detail}"
        verdict = "SKIPPED-too-little-data" if top == "SKIPPED" and need else top
        return verdict, detail
    parts = [f"{v} {names(v)}" for v in RANK if v in vs and v not in (top,)]
    if top == "INFO":
        ds = [r.data for _, _, r in rows if r.data]
        if not ds:
            return "INFO", rows[0][2].detail
        return "INFO", (f"{len(ds)} streams: entropy {min(d['entropy'] for d in ds):.6f}-"
                        f"{max(d['entropy'] for d in ds):.6f} bits/byte, chi-square exceeded "
                        f"{min(d['chi_pct'] for d in ds):.2f}-{max(d['chi_pct'] for d in ds):.2f}% of the "
                        f"time, |serial correlation| <= {max(abs(d['serial']) for d in ds):.6f}")
    first = next(r for n, v, r in rows if v == top)
    head = f"{vs.count(top)} of {len(rows)} streams ({names(top)})"
    return top, head + (f"; {'; '.join(parts)}" if parts else "") + f" -- {first.detail}"


def assess_run(path, out, args, meta_override=None):
    name = run_name(path) if meta_override is None else path.name
    rd = out / name
    if meta_override is None:
        meta = rng_extract.extract(path, rd)
        if meta is None or not meta["streams"]:
            return name, None, []
    else:
        meta = meta_override
    must = expectations(meta)
    jobs = []
    for sname, s in meta["streams"].items():
        c = Ctx(rd, sname, s, args.tools, args.claim, args.timeout)
        for t in TOOLS_BY_TAP[s["tap"]]:
            if t in args.skip:
                continue
            jobs.append((sname, t, c))
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(FUNCS[t], c): (sname, t) for sname, t, c in jobs}
        for f in concurrent.futures.as_completed(futs):
            sname, t = futs[f]
            try:
                r = f.result()
            except Exception as e:  # a tool's crash is a finding, never a pass
                r = Result("FAIL", f"rng_assess error: {e!r}")
            results[(sname, t)] = r
    details, lines = [], []
    for t in TOOL_ORDER:
        rows = []
        for sname in meta["streams"]:
            if (sname, t) in results:
                r = results[(sname, t)]
                v = grade(r.verdict, must.get(t, False))
                rows.append((sname, v, r))
                details.append(f"{v:<10} {t:<22} {sname:<12} {r.detail}")
        agg = aggregate(t, rows, must)
        if agg:
            kind = "criterion" if t in CRITERIA else "supplementary"
            lines.append((t, kind, agg[0], agg[1], must.get(t, False)))
    (rd / "details.txt").write_text("\n".join(details) + "\n")
    return name, meta, lines


def header(name, meta):
    ws = sorted({s["words"] for s in meta["streams"].values()})
    return (f"== {name}  ({meta.get('test')}, {meta.get('cpu_mode')} mode, source {meta.get('source')}"
            f"{', inject_fail' if meta.get('inject_fail') else ''}; {len(meta['streams'])} streams of "
            f"{'/'.join(fmt(w) for w in ws)} words; {len(meta.get('rng_fail', []))} RNG FAIL lines)")


def write_summary(out, runs, title):
    text, js, bad = [title, ""], [], 0
    for name, meta, lines in runs:
        if meta is None:
            text += [f"== {name}: no EG2_RNG lines", ""]
            continue
        text.append(header(name, meta))
        must_fail_run = bool(expectations(meta))
        for t, kind, v, d, must in lines:
            label = {"CAUGHT": "pass (caught)", "MISSED": "fail (not caught)"}.get(v, v)
            text.append(f"  {label:<24} {t:<22} {'(P) ' if kind == 'criterion' else ''}{d}")
            js.append({"run": name, "tool": t, "kind": kind, "verdict": v, "detail": d})
            # A must-fail run fails as it should everywhere; only its must-fail tool is graded.
            if (kind == "criterion" and v == "FAIL" and not must_fail_run) or (must and v == "MISSED"):
                bad += 1
        text.append("")
    text.append(f"{bad} pass-criterion failure(s) or missed must-fail result(s). (P) = pass criterion "
                "of REQ_PBF_24; details: <run>/details.txt, tool logs: <run>/<stream>/")
    (out / "summary.txt").write_text("\n".join(text) + "\n")
    (out / "summary.json").write_text(json.dumps(js, indent=1) + "\n")
    print("\n".join(text))
    return bad


# ─── Self-test: can each tool fail? ───────────────────────────────────────────────────────────
def lfsr_bits(nbits, seed=0x1234567):
    """x^31 + x^28 + 1 (maximal), s[k] = s[k-31] ^ s[k-28], 28 bits per step (big-int slices)."""
    s, have, mask = seed | 1, 31, (1 << 28) - 1
    out = bytearray()
    acc, nacc = 0, 0
    while len(out) * 8 < nbits:
        new = ((s >> 3) ^ s) & mask  # bits k-31 .. k-4 xor k-28 .. k-1 (oldest at bit 0 side)
        s = (s >> 28) | (new << (have - 28))
        acc = (acc << 28) | int(f"{new:028b}"[::-1], 2)
        nacc += 28
        while nacc >= 8:
            nacc -= 8
            out.append((acc >> nacc) & 0xFF)
        acc &= (1 << nacc) - 1
    return bytes(out[:nbits // 8])


def biased_bytes(nbytes, p_one):
    """Bytes whose bits are 1 with probability round(256 p)/256 each, independently."""
    cut = round(256 * p_one)
    table = bytes(1 if b < cut else 0 for b in range(256))
    r = os.urandom(8 * nbytes).translate(table)
    mul = 0x0102040810204080
    return bytes(((int.from_bytes(r[i:i + 8], "big") * mul) >> 56) & 0xFF for i in range(0, len(r), 8))


LO = bytes(b & 15 for b in range(256))
HI = bytes(b >> 4 for b in range(256))


def nibbles(data, low_first=True):
    """Each byte as two 4-bit symbols, one per byte."""
    lo, hi = data.translate(LO), data.translate(HI)
    out = bytearray(2 * len(data))
    out[0::2], out[1::2] = (lo, hi) if low_first else (hi, lo)
    return bytes(out)


def pack_symbols(sym):
    """Two symbols per byte, the earlier in the high nibble (rng_extract's raw .bin)."""
    n = len(sym) // 2
    hi = int.from_bytes(sym[0:2 * n:2], "big") << 4
    return (hi | int.from_bytes(sym[1:2 * n:2], "big")).to_bytes(n, "big")


def clear_bit0_of_words(data):
    out = bytearray(data)
    out[0::4] = bytes(out[0::4]).translate(bytes(b & 0xFE for b in range(256)))
    return bytes(out)


def selftest_cases(words, smallcrush, only=None):
    """(case, raw symbols or None, word-stream bytes), one case at a time, made only when asked
    for (only: a set of case names, empty for all). Raw streams: one 4-bit symbol per byte,
    2,000,000 of them (SP 800-90B takes the first 10^6, AIS 31 procedure B needs 7.2 Mbit).
    --smallcrush: word streams of SmallCrush's size, and only the cases cheap to make at that size."""
    want = lambda c: not only or c in only  # noqa: E731
    nraw = 2 * EA_SAMPLES
    w = SMALLCRUSH_WORDS + 1024 if smallcrush else words
    good_sym = nibbles(os.urandom(nraw // 2))
    good_w = os.urandom(4 * w)
    if want("good"):
        yield "good", good_sym, good_w
    # Raw: lanes 1-3 stuck at 0 (at most 0.25 bits/bit); words: bit 0 of every word stuck at 0.
    if want("stuck"):
        yield "stuck", bytes(b & 1 for b in os.urandom(nraw)), clear_bit0_of_words(good_w)
    # A 64-byte random block repeated (raw: 128 symbols repeated).
    if want("repeat"):
        rep = os.urandom(64)
        yield "repeat", nibbles((rep * (nraw // 128 + 1))[:nraw // 2]), (rep * (w // 16 + 1))[:4 * w]
    # The spec's inject: every 32-bit word twice (raw: every 8 symbols, one observe-FIFO word, twice).
    if want("dup"):
        half = array.array("I", good_w[:2 * w])  # the first half of good's words, each twice
        twice = array.array("I", bytes(4 * w))
        twice[0::2], twice[1::2] = half, half
        yield ("dup", b"".join(good_sym[i:i + 8] * 2 for i in range(0, nraw // 2, 8)), twice.tobytes())
        del half, twice
    if smallcrush:
        return
    # Raw: every lane 1 with probability 0.898 (0.155 bits/bit < 0.375); words: P(1) = 0.512.
    if want("biased"):
        yield "biased", nibbles(biased_bytes(nraw // 2, 0.9)), biased_bytes(4 * w, 0.512)
    if want("lfsr"):
        yield "lfsr", nibbles(lfsr_bits(4 * nraw), low_first=False), lfsr_bits(32 * w)


def selftest(args):
    out = args.out / "selftest"
    out.mkdir(parents=True, exist_ok=True)
    runs = []
    for case, sym, wbytes in selftest_cases(args.words, args.smallcrush, args.cases):
        rd = out / case
        rd.mkdir(exist_ok=True)
        streams = {}
        if sym is not None:
            (rd / "raw_fips.sym4").write_bytes(sym)
            (rd / "raw_fips.bin").write_bytes(pack_symbols(sym))
            streams["raw_fips"] = {"tap": "raw", "mode": "fips", "words": len(sym) // 8, "bits": 4 * len(sym),
                                   "symbols": len(sym), "files": {"bits": "raw_fips.bin", "sym4": "raw_fips.sym4"}}
        (rd / "drbg_fips.bin").write_bytes(wbytes)
        streams["drbg_fips"] = {"tap": "drbg", "mode": "fips", "words": len(wbytes) // 4, "bits": 8 * len(wbytes),
                                "files": {"bits": "drbg_fips.bin"}}
        meta = {"test": f"selftest {case}", "cpu_mode": "host", "source": "selftest", "inject_fail": False,
                "rng_fail": [], "streams": streams}
        (rd / "rng_capture.json").write_text(json.dumps(meta, indent=2) + "\n")
        print(f"selftest: {case}: assessing", flush=True)
        name, meta, lines = assess_run(rd, out, args, meta_override=meta)
        runs.append((name, meta, lines))
    write_summary(out, runs, f"EG2 RNG tool self-test {time.strftime('%Y-%m-%d %H:%M')}: good data must "
                  "pass, each tool must fail some bad data")
    # Grade: per tool, PASS on good and FAIL on at least one bad case.
    verdict = {(n, t): v for n, _, ls in runs for t, _, v, _, _ in ls}
    grade_lines, bad = [], 0
    for t in TOOL_ORDER:
        g = verdict.get(("good", t))
        fails = [n for n, _, _ in runs if n != "good" and verdict.get((n, t)) == "FAIL"]
        if g in (None, "INFO") or g.startswith("SKIPPED") or g == "NOT RUN":
            grade_lines.append(f"  {'n/a':<8} {t:<22} good: {g}; fails on: {' '.join(fails) or '-'}")
            continue
        ok = g == "PASS" and bool(fails)
        bad += not ok
        grade_lines.append(f"  {'OK' if ok else 'ERROR':<8} {t:<22} good: {g}; fails on: {' '.join(fails) or 'nothing'}")
    text = "Self-test grade (a tool is OK when it passes good data and fails at least one bad stream):\n" + \
        "\n".join(grade_lines) + "\n"
    (out / "grade.txt").write_text(text)
    print(text)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run", nargs="?", type=Path, help="run directory, log, or scratch root")
    ap.add_argument("--out", type=Path, default=Path("scratch/rng-assess"))
    ap.add_argument("--tools", type=Path, default=None, help="bin directory of `nix build .#rng-tools` (default: PATH)")
    ap.add_argument("--jobs", type=int, default=2, help="tools run at once (laptop: at most 3)")
    ap.add_argument("--claim", type=float, default=0.375, help="raw min-entropy per bit to reach")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="seconds per tool run")
    ap.add_argument("--skip", default="", help="comma-separated tools not to run")
    ap.add_argument("--only", default="", help="comma-separated tools to run (default: all)")
    ap.add_argument("--selftest", action="store_true", help="synthetic good and bad streams instead of captures")
    ap.add_argument("--words", type=int, default=3_200_000, help="selftest: words per word stream")
    ap.add_argument("--smallcrush", action="store_true", help="selftest: word streams big enough for SmallCrush")
    ap.add_argument("--cases", default="", help="selftest: comma-separated cases (default: all)")
    args = ap.parse_args()
    args.skip = {s for s in args.skip.split(",") if s}
    only = {s for s in args.only.split(",") if s}
    if only - set(TOOL_ORDER) or args.skip - set(TOOL_ORDER):
        print(f"rng_assess: unknown tool in --only/--skip; tools: {','.join(TOOL_ORDER)}", file=sys.stderr)
        return 2
    if only:
        args.skip |= set(TOOL_ORDER) - only
    args.cases = {s for s in args.cases.split(",") if s}
    if args.tools and not args.tools.is_dir():
        print(f"rng_assess: no tool directory {args.tools} (nix build .#rng-tools)", file=sys.stderr)
        return 2
    if args.tools:
        args.tools = args.tools.resolve()  # the tools run in their own work directories
    if args.selftest:
        return selftest(args)
    if args.run is None or not args.run.exists():
        print(f"rng_assess: give a run directory, log or scratch root (got {args.run})", file=sys.stderr)
        return 2
    runs_found = find_runs(args.run)
    if not runs_found:
        print(f"rng_assess: no chip_sw_eg2_rng_capture run under {args.run}", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    runs = [assess_run(p, args.out, args) for p in runs_found]
    bad = write_summary(args.out, runs, f"EG2 RNG assessment {time.strftime('%Y-%m-%d %H:%M')} of {args.run}: "
                        f"{len(runs)} capture run(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
