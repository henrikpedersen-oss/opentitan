#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# The CHERIoT RTOS software suite (doc/sw_test_suites.md): the CHERIoT RTOS test suite, the
# revocation, Juliet CWE, CHERI-C, debug host and performance firmware and the cheriot-rtos
# tests.extra programs on the EG2 SystemC model's RTOS test SoC board (eg2sim --board rtos_soc),
# each result compared with expected.txt. make sw-suite-rtos.
#
# runs.txt lists the runs, one firmware image each. A run gives many tests: one per check the
# firmware reports on its UART ("Test runner: <test> finished in <n> cycles" or "... failed",
# "<suite>: PASS|FAIL <check>: ...", "PASS|FAIL: CWE-<n>", "PASS|FAIL: clang_purecap_<test>" or
# "libc_<test>", "PERF <workload> cycles"), named <run>.<check>, and one more, <run>.verdict: the
# run as the RTL flow judges it (ibex_cheriot_verification dv/scripts/cheriot_rtos_verdict.sh: the
# run ended on the firmware's exit string, no failure line on the UART, "All tests finished" or
# the lines runs.txt says it must print). Each test ends in one of five outcomes:
#   pass        the check passed
#   must-fail   a check of a mutation run failed, as it must; reports show "pass (must fail)"
#   fail        the check failed (or passed under a mutation that should have broken it), or the
#               run ended without reporting it
#   model-stop  the model stopped the run before the check: the firmware needs something it does
#               not model (the reason is in reasons.txt and the report)
#   no-end      the run did not end before the check: the UART fell silent (200M cycles), or the
#               cycle or instruction limit
# expected.txt holds "<test> <outcome>" per test. The suite fails when a test's outcome differs
# from its line; a test with no line is reported as new, and a test of a run that ran which the
# run never reported keeps its line and is reported with how the run ended. --update rewrites
# expected.txt from this run (review the diff before committing it).
#
#   run.sh [--update] [--build] [-j JOBS] [PATTERN]
#     --build    build the firmware first (make rtos-firmware in eg2_top_verification)
#     PATTERN    run only the runs whose name contains it
#
# Firmware: eg2_top_verification's build (RTOS_TOP, default the checkout above eg2/: make rtos-firmware
# builds cheriot-rtos-test-suite/ since 2026-11-06), and for an image named nix:<package>/<path> its
# flake's package (RTOS_FLAKE, default RTOS_TOP; nix build). The model: build/systemc-model/eg2sim (make eg2sim).
# Output: build/sw-suites/cheriot-rtos/ (a directory per run: run.log, uart0.log; results.txt,
# report.txt).

set -uo pipefail
# sort and join must collate alike (join compares results.txt with expected.txt).
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, EG2SIM, SW_SUITES_OUT
RTOS_TOP="${RTOS_TOP:-$(cd "${EG2_ROOT}/.." && pwd)}"
OUT="${SW_SUITES_OUT}/cheriot-rtos"
EXPECTED="${HERE}/expected.txt"
RUNS="${HERE}/runs.txt"
UPDATE=0
BUILD=0
JOBS=3
while [[ $# -gt 0 ]]; do
  case "$1" in
    --update) UPDATE=1; shift ;;
    --build) BUILD=1; shift ;;
    -j) JOBS="$2"; shift 2 ;;
    *) break ;;
  esac
done
PATTERN="${1:-}"

[[ -x "${EG2SIM}" ]] || { echo "run.sh: no ${EG2SIM} (make eg2sim)" >&2; exit 2; }
if [[ ${BUILD} -eq 1 ]]; then
  make -C "${RTOS_TOP}" rtos-firmware || { echo "run.sh: the firmware build failed" >&2; exit 2; }
fi
# The boot stub every run loads: it owns the reset vector and jumps to the code RAM.
BOOT="${RTOS_TOP}/cheriot-rtos-test-suite/sw/boot/sim_boot_stub/sim_boot_stub"
[[ -f "${BOOT}" ]] || { echo "run.sh: no boot stub ${BOOT} (make sim-boot-stub in ${RTOS_TOP})" >&2; exit 2; }

mkdir -p "${OUT}"
awk '!/^#/ && NF >= 2 && $2 != "must-print" && $2 != "must-fail" {print $1}' "${RUNS}" \
  | grep -F -- "${PATTERN}" > "${OUT}/runs.txt"
# Each run's image, as a path: a path in the checkout, or nix:<package>/<path in it>, a package of
# this repository's flake (RTOS_FLAKE, default RTOS_TOP), built (or found in the nix store) here.
missing=0
: > "${OUT}/images.txt"
declare -A NIXOUT
while read -r run img _; do
  grep -qxF "${run}" "${OUT}/runs.txt" || continue
  if [[ ${img} == nix:* ]]; then
    pkg=${img#nix:}; pkg=${pkg%%/*}
    [[ -n "${NIXOUT[${pkg}]:-}" ]] || NIXOUT[${pkg}]=$(nix build --no-link --print-out-paths \
      "${RTOS_FLAKE:-${RTOS_TOP}}#${pkg}" 2>/dev/null | head -1)
    path="${NIXOUT[${pkg}]}/${img#nix:${pkg}/}"
  else
    path="${RTOS_TOP}/${img}"
  fi
  [[ -f "${path}" ]] || { echo "run.sh: ${run}: no image ${path} (${img})" >&2; missing=1; }
  printf '%s %s\n' "${run}" "${path}" >> "${OUT}/images.txt"
done < <(awk '!/^#/ && NF >= 2 && $2 != "must-print" && $2 != "must-fail"' "${RUNS}")
[[ ${missing} -eq 0 ]] || { echo "run.sh: build the firmware (--build, or make rtos-firmware in ${RTOS_TOP})" >&2; exit 2; }
echo "== $(wc -l < "${OUT}/runs.txt") runs on $(basename "${EG2SIM}") --board rtos_soc (${EG2SIM}), ${JOBS} at a time"

run_one() {
  local run=$1 dir img args t0 rc wall last why insns sim endc unmapped
  local -a extra
  dir="${OUT}/${run}"
  mkdir -p "${dir}"
  rm -f "${dir}/run.log" "${dir}/uart0.log" "${dir}/uart.txt"
  read -r _ args <<< "$(awk -v r="${run}" '$1 == r && $2 != "must-print" && $2 != "must-fail" {
    $1 = ""; sub(/^ +/, ""); print; exit}' "${RUNS}")"
  read -ra extra <<< "${args}"
  img=$(awk -v r="${run}" '$1 == r {print $2; exit}' "${OUT}/images.txt")
  t0=${EPOCHREALTIME}
  # The log is cut at 100 MB, which ends the run (every unmapped access is a line).
  (ulimit -v 4000000; timeout 1800 env -u LD_LIBRARY_PATH -u LD_PRELOAD "${EG2SIM}" --board rtos_soc \
    --boot "${BOOT}" --test "${img}" "${extra[@]}" --max-cycles 400000000 \
    --max-insns 200000000 --log-dir "${dir}" 2>&1 | head -c 100000000 > "${dir}/run.log"
   exit "${PIPESTATUS[0]}")
  rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b - a}')
  # The model's last word: how the run ended, the instructions and the simulated time (40 MHz).
  last=$(grep -a "^eg2sim: .* after [0-9]* instructions" "${dir}/run.log" | tail -1)
  why=$(sed 's/^eg2sim: //; s/ after [0-9]* instructions.*//' <<< "${last}")
  insns=$(sed -n 's/.* after \([0-9]*\) instructions.*/\1/p' <<< "${last}")
  sim=$(sed -n 's/.* (\([0-9.]*\) ms at .*/\1/p' <<< "${last}" | awk '{printf "%.0f", $1 * 1000}')
  case "${why}" in
    "exit string"|"firmware done"|"firmware failure") endc=ended ;;
    "UART idle"|"cycle limit reached"|"instruction limit reached") endc=no-end ;;
    "") endc=fail; why="eg2sim ended with exit status ${rc} and no last line" ;;
    *) endc=model-stop ;;
  esac
  [[ $(stat -c %s "${dir}/run.log") -ge 100000000 ]] && { endc=fail; why="log over 100 MB: run cut"; }
  unmapped=$(sed -n 's/^\[cheriot_rtos_default_rsp\] \([0-9]*\) read(s), \([0-9]*\) write(s).*/\1 \2/p' \
    "${dir}/run.log" | awk '{print $1 + $2}')
  # The UART's lines without the firmware's colour codes.
  [[ -f "${dir}/uart0.log" ]] && sed -e 's/\x1b\[[0-9;]*m//g' -e 's/\r$//' "${dir}/uart0.log" > "${dir}/uart.txt"
  touch "${dir}/uart.txt"
  awk -v run="${run}" -v runs="${RUNS}" -v expected="${EXPECTED}" -v endc="${endc}" -v why="${why}" \
      -v wall="${wall}" -v insns="${insns:--}" -v sim="${sim:--}" -v unmapped="${unmapped:-0}" '
    function key(s) {
      s = tolower(s); gsub(/\(c\+\+\)/, "cxx", s); gsub(/[^a-z0-9]+/, "_", s); gsub(/^_+|_+$/, "", s)
      return s
    }
    # A check, as the UART reports it: a FAIL outranks a PASS of the same check.
    function put(c, ok, text) {
      if (!(c in st)) { order[++n] = c; st[c] = ok; tx[c] = text }
      else if (st[c] && !ok) { st[c] = 0; tx[c] = text }
    }
    function row(test, out, text) {
      printf "%s\t%s\t%s\t%s\t%s\t%s\n", test, out, wall, insns, sim, substr(text, 1, 160)
    }
    BEGIN {
      while ((getline l < runs) > 0) {
        if (l ~ /^#/) continue
        split(l, f, " ")
        if (f[1] != run) continue
        if (f[2] == "must-fail") { mutation = 1; for (i = 3; i in f; i++) mustfail[f[i]] = 1 }
        if (f[2] == "must-print") {
          re = l; sub(/^[^ ]+ +must-print +[0-9]+ +/, "", re); sub(/ +$/, "", re)
          pre[++np] = re; pcount[np] = f[3]
        }
      }
      # The checks expected.txt has for this run: one the run never reported is not lost.
      while ((getline l < expected) > 0) {
        split(l, f, " ")
        if (index(f[1], run ".") == 1) {
          c = substr(f[1], length(run) + 2)
          if (c !~ /\./ && c != "verdict") want[c] = 1
        }
      }
    }
    {
      line = $0
      if (line ~ /[^ ]/) lines++
      for (i = 1; i <= np; i++) if (line ~ pre[i]) seen[i]++
      # The failure lines of cheriot_rtos_verdict.sh.
      if (failline == "" && line ~ /Invariant failure|One or more tests failed|Test runner: .* failed|: FAIL[: ]|Test\(s\) Failed/)
        failline = line
      if (line ~ /All tests finished/) alldone = 1
      if (line ~ /^Test runner: .* finished in [0-9]+ cycles$/) {
        c = line; sub(/^Test runner: /, "", c); t = c
        sub(/ finished in [0-9]+ cycles$/, "", c); sub(/^.* finished in /, "finished in ", t)
        if (c != "All tests") put(key(c), 1, t)
      } else if (line ~ /^Test runner: .* failed$/) {
        c = line; sub(/^Test runner: /, "", c); sub(/ failed$/, "", c)
        put(key(c), 0, "failed")
      } else if (line ~ /: (PASS|FAIL): CWE-[0-9]+/) {
        ok = line ~ /: PASS: CWE-/
        c = line; sub(/^.*: (PASS|FAIL): CWE-/, "", c); t = c
        sub(/[^0-9].*$/, "", c); sub(/^[0-9]+ */, "", t)
        put("cwe" c, ok, t)
      } else if (line ~ /: (PASS|FAIL): (clang_purecap|libc)_[A-Za-z0-9_]+/) {
        # CHERI-C: clang_purecap_<test> is named <test>, libc_<test> keeps its prefix.
        ok = line ~ /: PASS: (clang_purecap|libc)_/
        c = line; sub(/^.*: (PASS|FAIL): (clang_purecap_)?/, "", c); t = c
        sub(/[^A-Za-z0-9_].*$/, "", c); t = substr(t, length(c) + 1); sub(/^[^A-Za-z0-9]+/, "", t)
        put(c, ok, t)
      } else if (line ~ /^[^:]+: (PASS|FAIL) [^:]+:/) {
        ok = line ~ /^[^:]+: PASS /
        c = line; sub(/^[^:]+: (PASS|FAIL) /, "", c); t = c
        sub(/:.*$/, "", c); sub(/^[^:]*: */, "", t)
        put(key(c), ok, t)
      } else if (line ~ /^[^:]+: PERF [^ ]+ cycles /) {
        c = line; sub(/^[^:]+: PERF /, "", c); t = c; sub(/ .*$/, "", c)
        put(c, 1, "PERF " t)
      }
    }
    END {
      # A check the run never reported: how the run ended.
      gone = endc == "ended" ? "fail" : endc
      gonewhy = "not reported: the run ended (" why ")"
      note = unmapped > 0 ? "; " unmapped " unmapped accesses" : ""
      if (mutation) {
        for (c in mustfail) {
          if (!(c in st)) row(run "." c, gone, gonewhy)
          else if (st[c]) row(run "." c, "fail", "the mutation was not noticed: " tx[c])
          else row(run "." c, "must-fail", tx[c])
        }
        exit
      }
      for (k = 1; k <= n; k++) row(run "." order[k], st[order[k]] ? "pass" : "fail", tx[order[k]])
      for (c in want) if (!(c in st)) row(run "." c, gone, gonewhy)
      # The run as a whole (cheriot_rtos_verdict.sh).
      if (lines == 0) { out = endc == "ended" ? "fail" : endc; text = "the UART said nothing; " why }
      else if (endc != "ended") { out = endc; text = why }
      else if (why == "firmware failure" || failline != "") { out = "fail"; text = why "; " failline }
      else {
        out = "pass"; text = why
        for (i = 1; i <= np; i++) if (seen[i] + 0 != pcount[i]) {
          out = "fail"; text = (seen[i] + 0) " line(s) matching \"" pre[i] "\", expected " pcount[i]
          break
        }
        if (out == "pass" && np == 0 && !alldone) { out = "fail"; text = why "; no \"All tests finished\"" }
      }
      row(run ".verdict", out, text note)
    }' "${dir}/uart.txt"
}
export -f run_one
export EG2SIM BOOT RTOS_TOP OUT RUNS EXPECTED
start=$(date +%s)
# raw.txt: test, outcome, wall seconds, instructions, simulated us, what happened (tab-separated);
# the times are the run's, shared by its tests.
xargs -a "${OUT}/runs.txt" -P "${JOBS}" -I{} bash -c 'run_one "$@"' _ {} | sort > "${OUT}/raw.txt"
cut -f1,2 "${OUT}/raw.txt" | tr '\t' ' ' > "${OUT}/results.txt"
awk -F'\t' '$2 != "pass" && $2 != "must-fail" {print $1 ": " $6}' "${OUT}/raw.txt" > "${OUT}/reasons.txt"
secs=$(( $(date +%s) - start ))

if [[ ${UPDATE} -eq 1 ]]; then
  if [[ -n "${PATTERN}" && -f "${EXPECTED}" ]]; then
    # A partial run updates only the lines of its own runs.
    { awk 'NR == FNR {ran[$1] = 1; next} {r = $1; sub(/\.[^.]*$/, "", r)} !(r in ran)' \
        "${OUT}/runs.txt" "${EXPECTED}"
      cat "${OUT}/results.txt"; } | sort > "${EXPECTED}.new"
    mv "${EXPECTED}.new" "${EXPECTED}"
  else
    cp "${OUT}/results.txt" "${EXPECTED}"
  fi
  echo "== expected.txt updated from this run; review the diff"
fi

# The report: outcomes and every difference from expected.txt; each category (categories.txt) with
# its counts and the time of the runs its tests come from; then every test under its category:
# outcome, its run's wall time, instructions and simulated time, and what happened.
{
  echo "CHERIoT RTOS suite on $(basename "${EG2SIM}") --board rtos_soc, $(date -u +%FT%TZ), ${secs} s for" \
       "$(wc -l < "${OUT}/results.txt") tests in $(wc -l < "${OUT}/runs.txt") runs"
  awk '{n[$2]++} END {for (o in n) printf "  %-16s %d\n", o == "must-fail" ? "pass (must fail)" : o,
       n[o]}' "${OUT}/results.txt" | sort
  [[ -f "${EXPECTED}" ]] || touch "${EXPECTED}"
  join -a1 -e none -o 0,1.2,2.2 "${OUT}/results.txt" <(sort "${EXPECTED}") \
    | awk '$3 == "none" {print "  NEW      " $1 " " $2; next} $2 != $3 {print "  CHANGED  " $1 ": " $3 " -> " $2}'
  awk -F'\t' -v cats="${HERE}/categories.txt" -v known="${HERE}/known.txt" '
    BEGIN {
      while ((getline line < known) > 0) {
        if (line ~ /^#/ || line !~ /[^ ]/) continue
        split(line, f, " "); sub(/^[^ ]+ +/, "", line); why[f[1]] = line
      }
      while ((getline line < cats) > 0) {
        if (line ~ /^#/ || line !~ /[^ ]/) continue
        split(line, f, " "); pat[++np] = f[1]
        sub(/^[^ ]+ +/, "", line); cat[np] = line
        if (!(line in seen)) { seen[line] = 1; order[++nc] = line }
      }
      order[++nc] = "Other"
    }
    {
      c = "Other"
      for (i = 1; i <= np; i++) if ($1 ~ pat[i]) { c = cat[i]; break }
      n[c, $2]++; total[c]++
      r = $1; sub(/\.[^.]*$/, "", r)
      if (!((c, r) in ran)) { ran[c, r] = 1; wall[c] += $3 }
      out = $2 == "must-fail" ? "pass (must fail)" : $2
      said = $6
      if ($1 in why) { out = out " (known)"; said = why[$1] "; " said }
      list[c] = list[c] sprintf("    %-16s %7.2f s %10s %10s  %-44s %s\n", out, $3, $4, $5, $1, said)
    }
    END {
      printf "\nBy category:\n  %-60s %5s %5s %16s %5s %10s %6s %8s\n", "", "tests", "pass",
             "pass (must fail)", "fail", "model-stop", "no-end", "time"
      for (k = 1; k <= nc; k++) if (total[o = order[k]])
        printf "  %-60s %5d %5d %16d %5d %10d %6d %6.1f s\n", o, total[o], n[o, "pass"],
               n[o, "must-fail"], n[o, "fail"], n[o, "model-stop"], n[o, "no-end"], wall[o]
      print "  (time: the wall time of the runs the tests come from; one run gives many tests)"
      print "\nEvery test by category (time, instructions and simulated time are its run'"'"'s)"
      printf "    %-16s %9s %10s %10s  %-44s %s\n", "outcome", "time", "insns", "sim. us", "test",
             "what happened"
      for (k = 1; k <= nc; k++) if (total[o = order[k]]) printf "\n  == %s\n%s", o, list[o]
    }' "${OUT}/raw.txt"
} | tee "${OUT}/report.txt"
grep -q "CHANGED" "${OUT}/report.txt" && exit 1
exit 0
