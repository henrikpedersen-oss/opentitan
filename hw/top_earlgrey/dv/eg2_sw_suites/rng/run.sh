#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# EG2 random-number capture and published statistical batteries (REQ_PBF_24).
# Usage: run.sh [--update] [-j JOBS] [PATTERN]

set -uo pipefail
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, EG2_SCRATCH, EG2SIM, SW_SUITES_OUT
OUT="${SW_SUITES_OUT}/rng"
CAPTURES="${OUT}/captures"
ASSESS="${OUT}/assessment"
# The published batteries, built by eg2_top_verification: make rng-tools.
TOOLS="${RNG_TOOLS:-${EG2_SCRATCH}/rng-tools}"
EXPECTED="${HERE}/expected.txt"
JOBS=2
UPDATE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --update) UPDATE=1; shift ;;
    -j) JOBS=$2; shift 2 ;;
    *) break ;;
  esac
done
PATTERN=${1:-}
WORDS=${RNG_WORDS:-3200000}
REUSE=${RNG_REUSE:-0}
RUNS=(eg2_rng_capture eg2_rng_capture_cheriot eg2_rng_capture_inject_fail
      eg2_rng_capture_cheriot_inject_fail eg2_rng_capture_pattern_biased)
if [[ -n ${PATTERN} ]]; then
  selected=()
  for r in "${RUNS[@]}"; do [[ ${r} == *"${PATTERN}"* ]] && selected+=("${r}"); done
  RUNS=("${selected[@]}")
fi
[[ ${#RUNS[@]} -gt 0 ]] || { echo "rng: no run contains '${PATTERN}'" >&2; exit 2; }
[[ -x "${EG2SIM}" ]] || { echo "rng: no ${EG2SIM} (eg2_top_verification: make eg2sim)" >&2; exit 2; }
[[ -x "${TOOLS}/bin/ent" ]] || { echo "rng: no tools in ${TOOLS} (eg2_top_verification: make rng-tools)" >&2; exit 2; }

capture_rc=0
assess_rc=0
mkdir -p "${OUT}"
if [[ ${REUSE} == 1 ]]; then
  [[ -f "${ASSESS}/summary.txt" ]] || {
    echo "rng: RNG_REUSE=1 but no completed assessment exists" >&2
    exit 2
  }
  echo "== RNG suite: reusing completed captures and assessment"
else
  rm -rf "${CAPTURES}" "${ASSESS}"
  echo "== RNG suite: ${#RUNS[@]} captures, ${WORDS} words per tap, ${JOBS} jobs"
  "${HERE}/capture.sh" -o "${CAPTURES}" -j "${JOBS}" -w "${WORDS}" "${RUNS[@]}" || capture_rc=$?
  env PYTHONDONTWRITEBYTECODE=1 python3 -I "${HERE}/rng_assess.py" --tools "${TOOLS}/bin" --jobs "${JOBS}" \
    "${CAPTURES}" --out "${ASSESS}" || assess_rc=$?
fi

RESULTS="${OUT}/results.txt"
: > "${RESULTS}"
for detail in "${ASSESS}"/*/details.txt; do
  [[ -f ${detail} ]] || continue
  run=$(basename "$(dirname "${detail}")")
  awk -v run="${run}" 'NF >= 3 {
    if ($1 == "NOT" && $2 == "RUN") print run, $4, $3, "NOT-RUN"
    else print run, $3, $2, $1
  }' "${detail}" >> "${RESULTS}"
done
sort -o "${RESULTS}" "${RESULTS}"

printf '%s\n' "${RUNS[@]}" > "${OUT}/selected-runs.txt"
if [[ ${UPDATE} == 1 ]]; then
  if [[ -z ${PATTERN} ]]; then
    cp "${RESULTS}" "${EXPECTED}"
  else
    awk 'FILENAME == ARGV[1] {selected[$1] = 1; next}
         $1 ~ /^#/ || NF != 4 || !($1 in selected)' \
      "${OUT}/selected-runs.txt" "${EXPECTED}" > "${OUT}/expected-updated.txt"
    cat "${RESULTS}" >> "${OUT}/expected-updated.txt"
    sort -o "${OUT}/expected-updated.txt" "${OUT}/expected-updated.txt"
    mv "${OUT}/expected-updated.txt" "${EXPECTED}"
  fi
fi
awk 'FILENAME == ARGV[1] {selected[$1] = 1; next}
     $1 !~ /^#/ && NF == 4 && ($1 in selected)' \
  "${OUT}/selected-runs.txt" "${EXPECTED}" | sort > "${OUT}/expected-active.txt"
awk 'FILENAME == ARGV[1] {e[$1 FS $2 FS $3] = $4; next}
     {k=$1 FS $2 FS $3; if (!(k in e)) print; else if (e[k] != $4) print}' \
  "${OUT}/expected-active.txt" "${RESULTS}" > "${OUT}/changed.txt"
awk 'FILENAME == ARGV[1] {r[$1 FS $2 FS $3] = 1; next}
     {k=$1 FS $2 FS $3; if (!(k in r)) print}' \
  "${RESULTS}" "${OUT}/expected-active.txt" > "${OUT}/missing.txt"

# The outcome the report shows for a verdict. On deliberately bad data (the inject_fail and pattern
# runs) a tool spotting it is "pass (caught)"; CAUGHT and MISSED are the graded tools, any other
# tool that fails spotted it too, and one that passes was not required to. results.txt and
# expected.txt keep the one-word verdicts.
OUTCOME='function outcome(run, v) {
  if (v == "CAUGHT") return "pass (caught)"
  if (v == "MISSED") return "fail (not caught)"
  if (v == "NOT-RUN") return "not run"
  if (run ~ /inject_fail|pattern/ && v == "FAIL") return "pass (caught)"
  if (run ~ /inject_fail|pattern/ && v == "PASS") return "pass (not caught, not required)"
  return tolower(v)
}'

REPORT="${OUT}/report.txt"
elapsed=${RNG_ELAPSED_SECONDS:-${SECONDS}}
total=$(wc -l < "${RESULTS}")
{
  printf 'RNG statistical suite, %s, for %s tests from %s runs of %s words per tap in %s s\n' \
    "$(date -u +%FT%TZ)" "${total}" "${#RUNS[@]}" "${WORDS}" "${elapsed}"
  awk "${OUTCOME}"' {n[outcome($1, $4)]++} END {for (o in n) printf "  %-31s %d\n", o, n[o]}' "${RESULTS}" | sort
  echo "  new against expected.txt: $(wc -l < "${OUT}/missing.txt"), changed: $(wc -l < "${OUT}/changed.txt")"
  while read -r run stream tool verdict; do
    [[ -n ${run:-} ]] && echo "  CHANGED ${run} ${stream} ${tool}: ${verdict}"
  done < "${OUT}/changed.txt"
  while read -r run stream tool verdict; do
    [[ -n ${run:-} ]] && echo "  CHANGED ${run} ${stream} ${tool}: expected ${verdict}, not produced"
  done < "${OUT}/missing.txt"
  # The same layout as the other suites' reports: a table by category (one per capture run), then
  # every test (tap and tool) under its category with its outcome and what happened (details.txt).
  for detail in "${ASSESS}"/*/details.txt; do
    [[ -f ${detail} ]] || continue
    awk -v run="$(basename "$(dirname "${detail}")")" 'NF >= 3 {
      if ($1 == "NOT" && $2 == "RUN") { tool = $3; stream = $4; sub(/^NOT RUN +[^ ]+ +[^ ]+ */, "") }
      else { tool = $2; stream = $3; sub(/^[^ ]+ +[^ ]+ +[^ ]+ */, "") }
      print run "\t" stream "\t" tool "\t" $0
    }' "${detail}"
  done > "${OUT}/details-all.txt"
  # A skipped dieharder: how much data it had and how much its largest test needs (its rewinds).
  for detail in "${ASSESS}"/*/details.txt; do
    [[ -f ${detail} ]] || continue
    run=$(basename "$(dirname "${detail}")")
    while read -r verdict tool stream _; do
      [[ ${verdict} == SKIPPED && ${tool} == dieharder ]] || continue
      data="${ASSESS}/${run}/${stream}.bin"
      [[ -f ${data} ]] || continue
      supplied=$(( $(stat -c %s "${data}") / 4 ))
      rewinds=$(grep -a -h -o 'rewound [0-9][0-9]* times' \
        "${ASSESS}/${run}/${stream}"/dieharder_*.log 2>/dev/null \
        | awk '$2 > m {m=$2} END {print m+0}')
      printf '%s\t%s\t%s words supplied, the largest test needs at least %s\n' \
        "${run}" "${stream}" "${supplied}" "$(( supplied * (rewinds + 1) ))"
    done < "${detail}"
  done > "${OUT}/dieharder-size.txt"
  awk -v details="${OUT}/details-all.txt" -v sizes="${OUT}/dieharder-size.txt" "${OUTCOME}"'
    BEGIN {
      FS = "\t"
      while ((getline line < details) > 0) { split(line, f, "\t"); why[f[1] " " f[2] " " f[3]] = f[4] }
      while ((getline line < sizes) > 0) {
        split(line, f, "\t"); why[f[1] " " f[2] " dieharder"] = why[f[1] " " f[2] " dieharder"] ": " f[3]
      }
      FS = " "
      # The pass criteria of REQ_PBF_24 (rng_assess.py CRITERIA); the other tools are supplementary.
      crit["sp800-90b"] = crit["sp800-90b-restart"] = crit["sp800-22"] = 1
      name["eg2_rng_capture"] = "Normal capture"
      name["eg2_rng_capture_cheriot"] = "Normal capture, CHERIoT mode"
      name["eg2_rng_capture_inject_fail"] = "Broken data, every word duplicated"
      name["eg2_rng_capture_cheriot_inject_fail"] = "Broken data, CHERIoT mode"
      name["eg2_rng_capture_pattern_biased"] = "Biased raw data (pattern:biased)"
      # Normal runs first, then the deliberately bad data, in the order of RUNS.
      np = split("eg2_rng_capture eg2_rng_capture_cheriot eg2_rng_capture_inject_fail " \
                 "eg2_rng_capture_cheriot_inject_fail eg2_rng_capture_pattern_biased", ids, " ")
      for (k = 1; k <= np; k++) pos[ids[k]] = k
    }
    {
      c = ($1 in name) ? name[$1] " (" $1 ")" : $1
      if (!(c in total)) order[($1 in pos) ? pos[$1] : 100 + ++nx] = c
      out = outcome($1, $4); total[c]++; n[c, out]++
      said = why[$1 " " $2 " " $3]
      # On deliberately bad data only the tools that must catch it are graded (CAUGHT or MISSED).
      if ($1 ~ /inject_fail|pattern/ && ($4 == "PASS" || $4 == "FAIL"))
        said = "bad data, this tool not graded; " said
      if ($1 !~ /inject_fail|pattern/ && ($3 in crit)) said = "pass criterion; " said
      if ($4 == "CAUGHT" || $4 == "MISSED") said = "must catch the bad data; " said
      list[c] = list[c] sprintf("    %-31s %-12s %-22s %s\n", out, $2, $3, substr(said, 1, 160))
    }
    END {
      printf "\nBy category:\n  %-66s %5s %5s %13s %31s %5s %17s %5s %7s %7s\n", "", "tests", "pass",
             "pass (caught)", "pass (not caught, not required)", "fail", "fail (not caught)", "info",
             "skipped", "not run"
      for (k = 1; k <= 100 + nx; k++) if (k in order) {
        o = order[k]
        printf "  %-66s %5d %5d %13d %31d %5d %17d %5d %7d %7d\n", o, total[o], n[o, "pass"],
               n[o, "pass (caught)"], n[o, "pass (not caught, not required)"], n[o, "fail"],
               n[o, "fail (not caught)"], n[o, "info"], n[o, "skipped"], n[o, "not run"]
      }
      print "\nEvery test by category"
      printf "    %-31s %-12s %-22s %s\n", "outcome", "tap", "tool", "what happened"
      for (k = 1; k <= 100 + nx; k++) if (k in order) printf "\n  == %s\n%s", order[k], list[order[k]]
    }' "${RESULTS}"
  echo
  echo "Not tests: tap and mode combinations this capture image cannot produce"
  grep -a -h 'RNG gap ' "${CAPTURES}"/*/run.log 2>/dev/null \
    | sed 's/^.*RNG gap /  /' | sort -u
  echo
  echo "Each tool over all taps of a run: ${ASSESS}/summary.txt; each tool's log:"
  echo "${ASSESS}/<run>/<tap>/."
} > "${REPORT}"
cat "${REPORT}"

not_run=$(awk '$4 == "NOT-RUN" {n++} END {print n+0}' "${RESULTS}")
changed=$(wc -l < "${OUT}/changed.txt")
missing=$(wc -l < "${OUT}/missing.txt")
[[ ${capture_rc} == 0 && ${assess_rc} == 0 && ${not_run} == 0 && ${changed} == 0 && ${missing} == 0 ]]
