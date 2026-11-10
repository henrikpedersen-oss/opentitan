#!/usr/bin/env python3
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Run a batch of simulation jobs in parallel: up to --jobs at a time, each in its own process group,
with its output in <log-dir>/<name>.log.

The job file has one job per line, "<name><TAB><shell command>"; blank lines and lines starting with
# are ignored. Every job gets in its environment:
  PAR_JOB_INDEX      0, 1, 2, ... in file order
  PAR_PORT           --port-base + PAR_JOB_INDEX (when --port-base is given): a TCP port of its own
  RUN_ALL_NO_REPORT  1, so run_all_tests.sh leaves the HTML report to the batch

Memory guard: a job starts only while MemAvailable is at least --reserve-gb + --job-gb, and the guard
re-reads it before every start. JasperGold can take 90 GB of nyx's 125 (2026-10-09, OOM), so the
batch waits rather than start the job that would push the machine into the OOM killer.

Slot pool (--slots-dir, default $PAR_SLOTS_DIR): --jobs lock files in that directory, one held per
running job (flock). Batches given the same directory share the --jobs limit between them, so the
stages regr-xlm.sh runs side by side never run more than JOBS simulations together.

Exit status: 0 if every job exited 0, 1 otherwise. Jobs whose name matches --graded-elsewhere do not
count (fault-injection and mutation runs must fail; their caller grades them afterwards); with
--ignore-rc none do. A summary table of every job's status and time is printed at the end.
"""
import argparse
import fcntl
import os
import re
import signal
import subprocess
import sys
import time


def mem_available_gb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / (1024 * 1024)
    except OSError:
        pass
    return float("inf")


def take_slot(slots_dir, n):
    """A held lock on one of slots_dir/slot.<0..n-1>, or None if all are held."""
    for i in range(n):
        fd = os.open(os.path.join(slots_dir, f"slot.{i}"), os.O_CREAT | os.O_RDWR, 0o666)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return fd
        except BlockingIOError:
            os.close(fd)
    return None


def read_jobs(path):
    jobs = []
    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if "\t" not in line:
                sys.exit(f"run_parallel: no TAB between name and command: {line!r}")
            name, cmd = line.split("\t", 1)
            jobs.append((name.strip(), cmd))
    names = [n for n, _ in jobs]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        sys.exit(f"run_parallel: duplicate job names: {sorted(dup)}")
    return jobs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jobfile")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "32")))
    ap.add_argument("--log-dir", required=True)
    ap.add_argument("--port-base", type=int)
    ap.add_argument("--reserve-gb", type=float, default=float(os.environ.get("PAR_RESERVE_GB", "8")))
    ap.add_argument("--job-gb", type=float, default=float(os.environ.get("PAR_JOB_GB", "3")))
    ap.add_argument("--slots-dir", default=os.environ.get("PAR_SLOTS_DIR"))
    ap.add_argument("--graded-elsewhere", metavar="REGEX")
    ap.add_argument("--ignore-rc", action="store_true")
    ap.add_argument("--label", default="batch")
    args = ap.parse_args()

    jobs = read_jobs(args.jobfile)
    os.makedirs(args.log_dir, exist_ok=True)
    jobs_max = max(1, args.jobs)
    if args.slots_dir:
        os.makedirs(args.slots_dir, exist_ok=True)
    pool = f", sharing {args.slots_dir}" if args.slots_dir else ""
    print(f"[{args.label}] {len(jobs)} job(s), up to {jobs_max} at a time{pool}, logs in {args.log_dir}",
          flush=True)
    elsewhere = re.compile(args.graded_elsewhere) if args.graded_elsewhere else None

    pending = list(enumerate(jobs))
    running = {}          # pid -> (index, name, Popen, start, logfile, slot fd)
    results = {}          # name -> (rc, seconds)

    def stop_all(signum, _frame):
        for _, _, proc, _, _, _ in running.values():
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        sys.exit(128 + signum)
    signal.signal(signal.SIGINT, stop_all)
    signal.signal(signal.SIGTERM, stop_all)

    waited_for = None     # "memory" / "slot": said once per wait
    while pending or running:
        # Start as many as the job limit and the memory guard allow.
        while pending and len(running) < jobs_max:
            # A job started in the last 30 s has not mapped its snapshot yet, so MemAvailable does
            # not show it: count it against the guard until it has.
            now = time.time()
            young = sum(1 for _, _, _, t, _, _ in running.values() if now - t < 30)
            need = args.reserve_gb + args.job_gb * (1 + young)
            avail = mem_available_gb()
            if running and avail < need:
                if waited_for != "memory":
                    print(f"[{args.label}] waiting for memory: {avail:.1f} GB available, "
                          f"{need:.1f} GB needed per start ({len(running)} running)", flush=True)
                    waited_for = "memory"
                break
            slot = None
            if args.slots_dir:
                slot = take_slot(args.slots_dir, jobs_max)
                if slot is None:
                    if waited_for != "slot":
                        print(f"[{args.label}] waiting for a slot: all {jobs_max} in {args.slots_dir} "
                              f"held ({len(running)} by this batch)", flush=True)
                        waited_for = "slot"
                    break
            waited_for = None
            idx, (name, cmd) = pending.pop(0)
            env = dict(os.environ, PAR_JOB_INDEX=str(idx), RUN_ALL_NO_REPORT="1")
            if args.port_base is not None:
                env["PAR_PORT"] = str(args.port_base + idx)
            logfile = open(os.path.join(args.log_dir, f"{name}.log"), "w")
            proc = subprocess.Popen(["bash", "-c", cmd], stdout=logfile, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            running[proc.pid] = (idx, name, proc, time.time(), logfile, slot)
            port = f" port {env['PAR_PORT']}" if "PAR_PORT" in env else ""
            print(f"[{args.label}] start {name}{port} ({len(running)} running, "
                  f"{len(pending)} waiting)", flush=True)
            # Stagger starts a little: dozens of xrun processes starting in the same second all
            # check out a licence and map the snapshot at once.
            time.sleep(1)
        # Reap finished jobs.
        done = [pid for pid, (_, _, p, _, _, _) in running.items() if p.poll() is not None]
        if not done:
            time.sleep(2)
            continue
        for pid in done:
            idx, name, proc, start, logfile, slot = running.pop(pid)
            logfile.close()
            if slot is not None:
                os.close(slot)
            secs = time.time() - start
            results[name] = (proc.returncode, secs)
            verdict = "ok" if proc.returncode == 0 else f"rc={proc.returncode}"
            print(f"[{args.label}] done  {name}: {verdict} after {secs / 60:.1f} min", flush=True)

    print(f"\n[{args.label}] summary")
    print(f"  {'job':40} {'status':10} {'minutes':>7}")
    failed = graded = 0
    for name, _ in jobs:
        rc, secs = results[name]
        status = "PASS" if rc == 0 else f"FAIL({rc})"
        if elsewhere and elsewhere.search(name):
            graded += 1
            status += " *"
        elif rc != 0:
            failed += 1
        print(f"  {name:40} {status:10} {secs / 60:7.1f}")
    counted = len(jobs) - graded
    print(f"  {counted - failed} of {counted} exited 0" + (" (exit codes ignored)" if args.ignore_rc else ""))
    if graded:
        print(f"  * {graded} graded by the caller, not by exit code (expected to fail)")
    return 0 if args.ignore_rc or failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
