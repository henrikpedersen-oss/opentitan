#!/usr/bin/env python3
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Turn kept CHERIoT memory subsystem bench runs into a dvsim sim_results file.

Reads <bench>/xlm_out/results/<name>.<seed>.result and .log (written by cms_xlm_run.sh for every
run), the expected runs of <bench>/cms_regress.list and the expected failures of
<bench>/cms_expected_failures.hjson, grades each run, and writes
{timestamp, test_results: [{name, passing, total[, known_gaps]}]} for
cheriot_mem_sys_cms_testplan.hjson.

  ordinary run   PASS: status PASS on the CMS_RESULT line, errors=0, the scoreboard checked
                 something (core + revbm + csr transactions > 0 and at least one meta SRAM compare),
                 no Xcelium error (*E / *F) and no SVA failure in the log
  _corrupt_ run  scoreboard fault injection, PASS when: the run FAILED, the "CMS_CORRUPT ...
                 corrupted <kind>" line is present, and no CMS_ERROR precedes it (the injection is
                 the first error)
  @xfail=<id>    a declared expected failure (cms_expected_failures.hjson has the rules). It matches
                 when every CMS_ERROR line matches the entry's patterns, the test reached its end,
                 there is no simulator or SVA failure and the scoreboard checked something. Then:
                   kind known_gap   "FAIL (known gap <id>)": the RTL misses a requirement. Not a
                                    pass: the seed does not count as passing and the results file
                                    lists it under the test's known_gaps, so the testplan shows
                                    the requirement unmet. It does not fail cms_xlm_run.sh -x.
                   kind self_check  "PASS (must fail) <id>": counts as passing.
                 A run that does not match is FAIL. A run that passes is
                 "FAILED (known gap now passes, remove the declaration)" (or, for a self-check,
                 "FAILED (must fail, but passed)").

Per test, total is the number of seeds cms_regress.list asks for (at least 1) and passing the
number of those seeds graded PASS or PASS (must fail); a seed with no kept run counts as not
passing.

With --judge-log <log> --xfail <id>, grades that one run only (cms_xlm_run.sh -x): exit status 0
when it failed exactly as declared, else 1.
"""

import argparse
import datetime
import json
import re
import sys
from pathlib import Path

import hjson

RESULT = re.compile(r"CMS_RESULT test=(\S+) seed=(\d+) status=(\w+) errors=(\d+) cycles=(\d+) (.*)")
STAT = re.compile(r"(\w+)=(\d+)")
TOOL_ERR = re.compile(r"^\s*xmsim: \*[EF],|^\s*\*[EF],|Assertion .* has failed|ASSERT FAILED", re.M)
CMS_ERROR = re.compile(r"^CMS_ERROR.*$", re.M)

# The grades, spelt as in the UVM regression (ibex/dv/uvm/core_ibex/scripts/expected_failures.py)
# and the RTOS results (cheriot_rtos_results.py).
SELF_CHECK_GRADE = "PASS (must fail)"
SELF_CHECK_PASSED_GRADE = "FAILED (must fail, but passed)"
KNOWN_GAP_PASSED_GRADE = "FAILED (known gap now passes, remove the declaration)"
KINDS = ("self_check", "known_gap")


def known_gap_grade(ref):
    return f"FAIL (known gap {ref})"


def read_kv(path):
    fields = {}
    for line in path.read_text(errors="replace").splitlines():
        key, _, value = line.partition("=")
        fields[key] = value
    return fields


def read_list(path):
    runs = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        name, _test, seeds = fields[:3]
        xfail = next((x.removeprefix("@xfail=") for x in fields[3:]
                      if x.startswith("@xfail=")), "")
        runs[name] = (int(seeds), xfail)
    return runs


def read_expected_failures(path):
    """{id: declaration} from cms_expected_failures.hjson, each with its patterns compiled."""
    decls = {}
    for entry in hjson.loads(path.read_text()).get("expected_failures", []):
        for field in ("id", "kind", "requirement", "reason", "errors"):
            if not entry.get(field):
                raise SystemExit(f"{path}: expected failure {entry.get('id', entry)} has no {field}")
        if entry["kind"] not in KINDS:
            raise SystemExit(f"{path}: {entry['id']}: kind {entry['kind']!r} is not one of {KINDS}")
        if entry["id"] in decls:
            raise SystemExit(f"{path}: {entry['id']} is declared twice")
        try:
            entry["_errors"] = [re.compile(p) for p in entry["errors"]]
        except re.error as err:
            raise SystemExit(f"{path}: {entry['id']}: bad pattern: {err}")
        decls[entry["id"]] = entry
    return decls


def judge_declared(log, decl, status, errors, stats):
    """(grade, reason) for a run declared @xfail=<id>: grade decl's kind when the run failed
    exactly as declared, "unexpected_pass" when it passed, else "fail"."""
    xid = decl["id"]
    tool = TOOL_ERR.search(log)
    if tool:
        return "fail", f"simulator error, not {xid}: {tool.group(0).strip()[:100]}"
    lines = CMS_ERROR.findall(log)
    if status == "PASS" and errors == 0 and not lines:
        return "unexpected_pass", f"{xid} declared, but the run passed"
    if status != "FAIL" or not lines:
        return "fail", f"expected {xid}, got {status} with {errors} error(s), {len(lines)} CMS_ERROR line(s)"
    # Every error must be the declared one, and the test must have reached its end: a timeout (or
    # any other error after the first) is a different failure, not the gap.
    timed_out = [line for line in lines if "test timed out" in line]
    if timed_out:
        return "fail", f"did not reach its end: {timed_out[0][:120]}"
    if len(lines) != errors:
        return "fail", f"{len(lines)} CMS_ERROR line(s) but CMS_RESULT counts {errors}"
    other = [line for line in lines if not any(p.search(line) for p in decl["_errors"])]
    if other:
        return "fail", f"{len(other)} of {len(lines)} error(s) are not {xid}, first: {other[0][:160]}"
    checked = stats.get("core", 0) + stats.get("revbm", 0) + stats.get("csr", 0)
    if checked == 0 or stats.get("backdoor_compares", 0) == 0:
        return "fail", f"inactive {xid}: {checked} transactions, " \
                       f"{stats.get('backdoor_compares', 0)} meta SRAM compares"
    return decl["kind"], f"{len(lines)} error(s), every one {xid}: {lines[0][:120]}"


def judge(name, log, decl=None):
    """Return (grade, reason) for one kept run; grade is "pass", "fail", or for a declared run
    also "known_gap", "self_check" or "unexpected_pass" (judge_declared())."""
    m = None
    for m in RESULT.finditer(log):
        pass
    if m is None:
        fatal = TOOL_ERR.search(log)
        return "fail", "no CMS_RESULT line" + (f" ({fatal.group(0).strip()[:80]})" if fatal else "")
    status, errors = m.group(3), int(m.group(4))
    stats = {k: int(v) for k, v in STAT.findall(m.group(6))}
    if "_corrupt_" in name:
        kind = name.rsplit("_corrupt_", 1)[1]
        corrupt = re.search(rf"^CMS_CORRUPT .*corrupted {kind}\b", log, re.M)
        if status != "FAIL":
            return "fail", "fault-injection run did not fail"
        if not corrupt:
            return "fail", f"no 'corrupted {kind}' line (injection never applied)"
        first_err = re.search(r"^CMS_ERROR", log, re.M)
        if first_err and first_err.start() < corrupt.start():
            return "fail", "a CMS_ERROR precedes the injection"
        return "pass", f"corrupted {kind}, {errors} error(s) after it"
    if decl:
        return judge_declared(log, decl, status, errors, stats)
    if status != "PASS" or errors:
        first = CMS_ERROR.search(log)
        return "fail", f"FAIL, {errors} error(s)" + (f": {first.group(0)[:120]}" if first else "")
    tool = TOOL_ERR.search(log)
    if tool:
        return "fail", f"simulator error: {tool.group(0).strip()[:100]}"
    checked = stats.get("core", 0) + stats.get("revbm", 0) + stats.get("csr", 0)
    if checked == 0:
        return "fail", "the scoreboard checked nothing"
    if stats.get("backdoor_compares", 0) == 0:
        return "fail", "no meta SRAM compare"
    return "pass", f"{checked} transactions, {stats.get('tag_checks', 0)} tag checks, " \
                   f"{stats.get('sweeps', 0)} sweeps, {stats.get('swept_caps', 0)} swept capabilities"


def grade_label(grade, decl):
    """The grade as the reports spell it."""
    if grade == "known_gap":
        return known_gap_grade(decl["id"])
    if grade == "self_check":
        return f"{SELF_CHECK_GRADE} {decl['id']}"
    if grade == "unexpected_pass":
        return KNOWN_GAP_PASSED_GRADE if decl["kind"] == "known_gap" else SELF_CHECK_PASSED_GRADE
    return grade.upper()


def judge_one(args):
    """cms_xlm_run.sh -x: grade one run against its declaration; 0 iff it failed as declared."""
    decls = read_expected_failures(args.bench_dir / "cms_expected_failures.hjson")
    decl = decls.get(args.xfail)
    if decl is None:
        print(f"unknown expected failure '{args.xfail}' (not in cms_expected_failures.hjson)",
              file=sys.stderr)
        return 1
    grade, why = judge(args.name or "", args.judge_log.read_text(errors="replace"), decl)
    print(f"{grade_label(grade, decl)}: {why}")
    return 0 if grade == decl["kind"] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--testplan", type=Path)
    ap.add_argument("--bench-dir", required=True, type=Path,
                    help="hw/ip/cheriot_mem_sys/dv/cms (holds cms_regress.list, "
                         "cms_expected_failures.hjson and xlm_out/results/)")
    ap.add_argument("--seed-base", type=int, default=1)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--judge-log", type=Path, help="grade only this log (with --xfail)")
    ap.add_argument("--xfail", help="the run's declared expected failure (with --judge-log)")
    ap.add_argument("--name", help="the run's name (with --judge-log)")
    args = ap.parse_args()
    if args.judge_log:
        if not args.xfail:
            ap.error("--judge-log needs --xfail")
        return judge_one(args)
    if not args.testplan or not args.out:
        ap.error("--testplan and --out are required")

    in_plan = set()
    for tp in hjson.loads(args.testplan.read_text()).get("testpoints", []):
        in_plan.update(tp.get("tests", []))
    expected = read_list(args.bench_dir / "cms_regress.list")
    decls = read_expected_failures(args.bench_dir / "cms_expected_failures.hjson")
    for name, (_, xfail) in sorted(expected.items()):
        if xfail and xfail not in decls:
            raise SystemExit(f"cms_regress.list: {name} @xfail={xfail} is not declared in "
                             "cms_expected_failures.hjson")
    for name in sorted(in_plan - set(expected)):
        print(f"WARNING  testplan test '{name}' is not in cms_regress.list")
    for name in sorted(set(expected) - in_plan):
        print(f"WARNING  cms_regress.list run '{name}' is in no testpoint")

    rdir = args.bench_dir / "xlm_out" / "results"
    kept = sorted(rdir.glob("*.result")) if rdir.exists() else []
    print(f"{len(kept)} kept run(s) in {rdir}")
    grades = {}
    for rpath in kept:
        res = read_kv(rpath)
        name, seed = res.get("name", ""), int(res.get("seed", "0") or 0)
        decl = decls.get(expected.get(name, (0, ""))[1])
        log = rpath.with_suffix(".log")
        if not log.exists():
            grade, why = "fail", f"no kept log {log.name}"
        else:
            grade, why = judge(name, log.read_text(errors="replace"), decl)
        print(f"  {grade_label(grade, decl):53s} {name:32s} seed {seed:<4d} {res.get('date', '')}  {why}")
        grades[(name, seed)] = (grade, decl)

    results = []
    gaps_total = {}
    for name in sorted(in_plan | set(expected)):
        seeds = range(args.seed_base, args.seed_base + max(1, expected.get(name, (1, ""))[0]))
        passing = sum(1 for s in seeds if grades.get((name, s), ("",))[0] in ("pass", "self_check"))
        gaps = {}
        for s in seeds:
            grade, decl = grades.get((name, s), ("", None))
            if grade == "known_gap":
                gaps[decl["id"]] = gaps.get(decl["id"], 0) + 1
                gaps_total[decl["id"]] = gaps_total.get(decl["id"], 0) + 1
        missing = [s for s in seeds if (name, s) not in grades]
        if missing:
            print(f"NOT RUN  {name}: seed(s) {', '.join(map(str, missing))}")
        entry = {"name": name, "passing": passing, "total": len(seeds)}
        if gaps:
            # Read by testplan_report.py, to show the test as a known gap; dvsim ignores it.
            entry["known_gaps"] = gaps
        results.append(entry)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({
        "timestamp": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "test_results": results,
    }, indent=2) + "\n")
    total = sum(r["total"] for r in results)
    passing = sum(r["passing"] for r in results)
    must_fail = sum(1 for g, _ in grades.values() if g == "self_check")
    line = f"{len(results)} tests: {passing}/{total} test x seed results pass"
    if must_fail:
        line += f" ({must_fail} {SELF_CHECK_GRADE})"
    if gaps_total:
        line += f"; {sum(gaps_total.values())} FAIL (known gap " + \
                ", ".join(f"{k}: {v}" for k, v in sorted(gaps_total.items())) + ")"
    print(f"{line} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
