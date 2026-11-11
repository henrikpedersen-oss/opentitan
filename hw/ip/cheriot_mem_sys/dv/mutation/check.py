#!/usr/bin/env python3
# Copyright lowRISC contributors. Licensed under Apache-2.0.
"""Grade RTL mutation evidence separately from ordinary regression verdicts.

A caught mutation needs an activation diagnostic AND a failure in the black-box
scoreboard/reference model/feature sequence. An RTL assertion alone, timeout,
missing log, or inactive force cannot establish checker detection. Normal tests
and coverage must be run separately without the mutation build/plusarg.
"""
import argparse
import json
import re
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scratch", type=Path, help="DVSim cheriot_mem_sys-sim-xcelium directory")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--retry-scratch", type=Path, action="append", default=[],
                    help="Use completed rerun logs from this scratch tree where present")
    args = ap.parse_args()
    manifest = json.loads(Path(__file__).with_name("manifest.json").read_text())
    results = []
    for spec in manifest["runs"]:
        log = args.scratch / ("0." + spec["test"]) / "latest/run.log"
        for retry in args.retry_scratch:
            candidate = retry / ("0." + spec["test"]) / "latest/run.log"
            if candidate.exists():
                log = candidate
        text = log.read_text(errors="replace") if log.exists() else ""
        active = f'MUTATION {spec["mutation"]} active' in text
        errors = [line for line in text.splitlines() if re.search(r"UVM_(ERROR|FATAL) @", line)]
        checker = [line for line in errors if re.search(
            r"\(cheriot_mem_sys_(?:scoreboard|ref_model|\w+_vseq)\.sv:", line)
            and not re.search(r"timeout|watchdog|Cannot randomize|ASSERT FAILED", line, re.I)]
        if not text:
            verdict = "NO LOG"
        elif "UVM Report Summary" not in text:
            verdict = "INCOMPLETE"
        elif not active:
            verdict = "INACTIVE"
        elif checker:
            verdict = "CAUGHT"
        elif errors:
            verdict = "ASSERTION/INFRASTRUCTURE ONLY"
        else:
            verdict = "MISSED"
        results.append({**spec, "classification": verdict, "active": active,
                        "checker_error": checker[0] if checker else "",
                        "first_error": errors[0] if errors else "", "log": str(log)})
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    lines = ["# RTL mutation evidence", "", "Origin: `cde6b0a7e0`; 16 mutations, 41 original test/mutation pairs.",
             "Activity is an injection diagnostic, never an input to the black-box verdict.", "",
             "| Mutation | Test | Classification | Checker evidence |", "|---|---|---|---|"]
    for r in results:
        msg = r["checker_error"].replace("|", "\\|")
        lines.append(f'| {r["mutation"]} | {r["baseline"]} | {r["classification"]} | {msg} |')
    caught = {r["mutation"] for r in results if r["classification"] == "CAUGHT"}
    lines += ["", f"{len(caught)}/16 mutations caught by at least one independent check."]
    (args.out / "results.md").write_text("\n".join(lines) + "\n")
    print(lines[-1])
    for r in results:
        if r["classification"] != "CAUGHT":
            print(r["mutation"], r["baseline"], r["classification"])
    return 0 if all(r["classification"] == "CAUGHT" for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
