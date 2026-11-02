#!/usr/bin/env python3
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""The captured random bits of one `chip_sw_eg2_rng_capture` run, as binary files the assessment
tools read (EG2 spec "Random Number Generation Test Suite" > "Taps and assessment", REQ_PBF_24;
`make sw-suite-rng` runs it through rng_assess.py).

Input: a run directory (dvsim's <n>.<test>/latest/, or an eg2sim --log-dir) or one log file. The
capture test writes its words through the DV log as
    EG2_RNG <tap>_<mode> w0 w1 w2 w3 w4 w5 w6 w7      (8 words of 32 bits, hex, in read order)
(sw/device/tests/eg2_rng/eg2_rng.c, eg2_rng_log_line), which the chip bench prints in run.log
(sw_logger_if) and eg2sim in run.log (`sw: ...`) and sw.model.log. Only one file is read, so no
line counts twice: <dir>/run.log, else the first *.log in the directory with an EG2_RNG line.

Output, in --out, per stream <tap>_<mode> (tap raw, cond, drbg, edn; mode boot, fips, bypass):
  <stream>.bin    cond, drbg, edn: the words in read order, each little-endian (byte 0 = bits
                  7:0), the order the chip's memory holds them and SHA-3 and CTR_DRBG give their
                  bytes. Tools that read bits take each byte most significant bit first.
                  raw: the noise symbols as a bit stream in time order, two 4-bit symbols per
                  byte, the earlier in the high nibble, each symbol's bit 3 (RNG lane 3) first.
  <stream>.sym4   raw only: one 4-bit symbol per byte (SP 800-90B's sample format), in time order.
                  The observe FIFO packs the first symbol into bits 3:0 of a word
                  (prim_packer_fifo in entropy_src_core.sv, the shift by depth x InW), so word w
                  gives (w >> 4i) & 0xf for i = 0..7.
  rng_capture.json  the run: log, test, CPU mode, inject_fail, noise source, seeds, the test's
                  parameters, `RNG FAIL` lines, and per stream its tap, mode, words, bits,
                  symbols, files and SHA-256.

Usage: rng_extract.py <run dir or log> --out <dir>
Exit status: 0 with at least one stream, 1 with none, 2 on bad input.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

DATA = re.compile(r"EG2_RNG ((raw|cond|drbg|edn)_(boot|fips|bypass))((?:\s+[0-9a-fA-F]{8}){8})\s*$")
# Run facts, from any log of the run (the chip prints the UART's lines elsewhere than the DV log).
TEST = re.compile(r"RNG test (\S+): (\S+) mode")
INJECT = re.compile(r"expected value corrupted: EG2_RNG_INJECT_FAIL")
PARAM = re.compile(r"EG2 param (\w+) = (\d+)")
SEED = re.compile(r"EG2 seed[^=]*= (\d+)")
SIM_SEED = re.compile(r"(?:ntb_random_seed|svseed|-seed)[= ](\d+)", re.I)
SOURCE_CHIP = re.compile(r"EG2_RNG source (\S+) selected")  # chip_sw_eg2_rng_vseq.sv
SOURCE_MODEL = re.compile(r"eg2sim: noise source (\S+)")  # eg2sim.cc --rng
RNG_FAIL = re.compile(r"RNG FAIL .*")


def data_log(path):
    """The one file the words are read from."""
    if path.is_file():
        return path
    if (path / "run.log").is_file():
        return path / "run.log"
    for f in sorted(path.glob("*.log")):
        with open(f, errors="replace") as fh:
            if any("EG2_RNG " in line for line in fh):
                return f
    return None


def run_facts(path, log):
    """Metadata from every log of the run directory (or the one file)."""
    files = [log] if path.is_file() else sorted(path.glob("*.log"))
    facts = {"test": None, "cpu_mode": None, "inject_fail": False, "params": {}, "eg2_seed": None,
             "sim_seed": None, "source": None, "rng_fail": []}
    for f in files:
        with open(f, errors="replace") as fh:
            for line in fh:
                if "EG2_RNG " in line and DATA.search(line):
                    continue
                if (m := TEST.search(line)) and not facts["test"]:
                    facts["test"], facts["cpu_mode"] = m.group(1), m.group(2)
                if INJECT.search(line):
                    facts["inject_fail"] = True
                if m := PARAM.search(line):
                    facts["params"][m.group(1)] = int(m.group(2))
                if (m := SEED.search(line)) and facts["eg2_seed"] is None:
                    facts["eg2_seed"] = int(m.group(1))
                if (m := SIM_SEED.search(line)) and facts["sim_seed"] is None:
                    facts["sim_seed"] = int(m.group(1))
                if (m := SOURCE_CHIP.search(line) or SOURCE_MODEL.search(line)) and not facts["source"]:
                    facts["source"] = m.group(1)
                if (m := RNG_FAIL.search(line)) and m.group(0) not in facts["rng_fail"]:
                    facts["rng_fail"].append(m.group(0)[:200])
    # Neither the bench nor eg2sim named a source: AST's behavioural model, or the SystemC
    # model's own generator (spec "Sources": `model`, the default).
    facts["source"] = facts["source"] or "model"
    return facts


def extract(path, out):
    log = data_log(path)
    if log is None:
        print(f"rng_extract: no log with EG2_RNG lines in {path}", file=sys.stderr)
        return None
    words = {}
    with open(log, errors="replace") as fh:
        for line in fh:
            if "EG2_RNG " not in line:
                continue
            m = DATA.search(line)
            if m:
                words.setdefault(m.group(1), []).extend(int(w, 16) for w in m.group(4).split())
    meta = {"log": str(log), **run_facts(path, log), "streams": {}}
    out.mkdir(parents=True, exist_ok=True)
    for name, ws in sorted(words.items()):
        tap, mode = name.split("_")
        s = {"tap": tap, "mode": mode, "words": len(ws), "bits": 32 * len(ws), "files": {}}
        if tap == "raw":
            syms = bytes((w >> (4 * i)) & 0xF for w in ws for i in range(8))
            packed = bytes((syms[i] << 4) | syms[i + 1] for i in range(0, len(syms), 2))
            files = {"bits": (f"{name}.bin", packed), "sym4": (f"{name}.sym4", syms)}
            s["symbols"] = len(syms)
        else:
            files = {"bits": (f"{name}.bin", b"".join(w.to_bytes(4, "little") for w in ws))}
        for kind, (fname, blob) in files.items():
            (out / fname).write_bytes(blob)
            s["files"][kind] = fname
            s.setdefault("sha256", {})[kind] = hashlib.sha256(blob).hexdigest()
        meta["streams"][name] = s
    (out / "rng_capture.json").write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run", type=Path, help="run directory or log file")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    if not args.run.exists():
        print(f"rng_extract: {args.run} does not exist", file=sys.stderr)
        return 2
    meta = extract(args.run, args.out)
    if meta is None:
        return 2
    for name, s in meta["streams"].items():
        print(f"{name:12s} {s['words']:9d} words  {s['bits']:11d} bits  -> {args.out / s['files']['bits']}")
    print(f"test {meta['test']} ({meta['cpu_mode']}), source {meta['source']}, "
          f"inject_fail {meta['inject_fail']}, {len(meta['rng_fail'])} RNG FAIL line(s); "
          f"{args.out / 'rng_capture.json'}")
    return 0 if meta["streams"] else 1


if __name__ == "__main__":
    sys.exit(main())
