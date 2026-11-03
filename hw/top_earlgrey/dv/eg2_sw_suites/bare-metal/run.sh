#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# The bare-metal software suite (doc/sw_test_suites.md): every chip test image on the EG2 SystemC
# model, each result compared with expected.txt. make sw-suite-bare-metal.
#
# Each run ends in one of six outcomes, from eg2sim's exit status or the suite policy:
#   pass        the test passed (status word 0x900d; exit 0)
#   must-fail   an _inject_fail variant failed, as it must (exit 3), on the check its build broke
#               (EG2_INJECT_CAUGHT; one failing elsewhere first is a fail); reports show it as
#               "pass (must fail)"
#   fail        the test finished and reported a failure (exit 3; or an _inject_fail that passed)
#   model-stop  the model stopped the run: the test needs something it does not model, most often a
#               wake-up from WFI or low power that only a testbench gives (exit 2; the reason is in
#               reasons.txt and the report)
#   no-end      the test did not finish within the instruction limit, typically waiting for
#               stimulus from a testbench (exit 4)
#   on-hold     the test cannot run in the suite; on_hold.txt gives the specific reason (for
#               example an unavailable production-ROM flow or one unsplittable run over the time
#               limit), and the test is not run
# expected.txt holds "<test> <outcome>" per test. The suite fails when a test's outcome differs from
# its line; a test with no line is reported as new. --update rewrites expected.txt from this run
# (review the diff before committing it).
#
#   run.sh [--update] [-j JOBS] [PATTERN]
#     PATTERN    run only tests whose name contains it
#
# Images: the newest *_sim_dv.elf of each test in the EG2 fork's Bazel output (EG2_ROOT/bazel-out),
# until this repository builds its own software: the chip tests (sw/device/tests, the generated
# hw/top_earlgrey/sw/autogen/tests), the libraries' own tests that run from the test ROM
# (sw/device/lib, sw/device/silicon_creator/lib) and CoreMark. A test is named after its image; when
# two packages build an image of the same name, the one in sw/device/tests/sim_dv (the chip
# simulation's own copy), else in sw/device/tests or the generated tests, keeps the name and the
# other is named <name>@<package, with dots>. variants.txt (gen_variants.sh) adds a run of an image
# for each chip test that sets it up differently, named <image>@<chip test>. bench.txt may name
# another image of that output, {image:<path under bin/>} (an SRAM program the debugger loads): a
# test whose image is not built is reported as model-stop, not run.
# The model: EG2SIM (../env.sh; eg2_top_verification: make eg2sim).
# Output: build/sw-suites/bare-metal/ (a log directory per test, results.txt, report.txt).

set -uo pipefail
# sort and join must collate alike (join compares results.txt with expected.txt).
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, EG2SIM, SW_SUITES_OUT
OUT="${SW_SUITES_OUT}/bare-metal"
EXPECTED="${HERE}/expected.txt"
UPDATE=0
JOBS=3
while [[ $# -gt 0 ]]; do
  case "$1" in
    --update) UPDATE=1; shift ;;
    -j) JOBS="$2"; shift 2 ;;
    *) break ;;
  esac
done
PATTERN="${1:-}"

[[ -x "${EG2SIM}" ]] || { echo "run.sh: no ${EG2SIM} (eg2_top_verification: make eg2sim)" >&2; exit 2; }
newest() { find -L "${EG2_ROOT}/bazel-out/" -path "$1" -not -path '*runfiles*' -printf '%T@ %p\n' 2>/dev/null \
  | sort -rn | head -1 | cut -d' ' -f2-; }
ROM=$(newest '*bin/sw/device/lib/testing/test_rom/test_rom_sim_dv.elf')
OTP=$(newest '*bin/hw/top_earlgrey/data/otp/img_rma.24.vmem')
[[ -n "${ROM}" && -n "${OTP}" ]] || { echo "run.sh: test ROM or OTP image not built in ${EG2_ROOT}" >&2; exit 2; }

# The tests that bring their own OTP image (otp.txt), and the life-cycle images variants.txt names,
# resolved once: <test or image> <file>, or <test or image> MISSING:<image> when it is not built.
mkdir -p "${OUT}"
{
  while read -r t pat; do
    [[ -z "${t}" || "${t}" == \#* ]] && continue
    img=$(newest "*bin/${pat}")
    echo "${t} ${img:-MISSING:${pat}}"
  done < "${HERE}/otp.txt"
  for pat in $(grep -o 'otp=[^ ]*' "${HERE}/variants.txt" | sort -u); do
    img=$(newest "*bin/${pat#otp=}")
    echo "${pat} ${img:-MISSING:${pat#otp=}}"
  done
} > "${OUT}/otp_images.txt"

# The other images bench.txt names, {image:<path under bazel-out's bin/>} (a program the debugger
# loads), resolved once: <token> <image>, or <token> MISSING when it is not built.
grep -o '{image:[^}]*}' "${HERE}/bench.txt" | sort -u | while read -r tok; do
  p=${tok#\{image:}; p=${p%\}}
  img=$(newest "*bin/${p}")
  echo "${tok} ${img:-MISSING}"
done > "${OUT}/bench_images.txt"

# The newest image of each test in each package that builds one, then the runs: <run> <image file>
# <image name>. A name built in several packages keeps its plain form for the first package in
# this order: sw/device/tests/sim_dv, the rest of sw/device/tests and the generated tests, the
# others; each other package's image is <name>@<package with dots>. Not tests: the test ROM, and
# the signed ROM_EXT images the eg2_boot tests boot (rom_ext_key_*).
declare -A NEWEST
while read -r img; do
  pkg=${img#*/bin/}
  key="${pkg%/*} $(basename "${img}" _sim_dv.elf)"
  [[ -z "${NEWEST[${key}]:-}" ]] && NEWEST[${key}]=${img}
done < <(find -L "${EG2_ROOT}/bazel-out/" \( -path '*bin/sw/device/tests/*_sim_dv.elf' \
           -o -path '*bin/hw/top_earlgrey/sw/autogen/tests/*_sim_dv.elf' \
           -o -path '*bin/sw/device/lib/*_sim_dv.elf' \
           -o -path '*bin/sw/device/silicon_creator/lib/*_sim_dv.elf' \
           -o -path '*bin/third_party/coremark/*_sim_dv.elf' \) -not -path '*runfiles*' \
           -not -name test_rom_sim_dv.elf -not -name 'rom_ext_key_*' -printf '%T@ %p\n' 2>/dev/null \
           | sort -rn | cut -d' ' -f2-)
for key in "${!NEWEST[@]}"; do
  pkg=${key% *}
  case "${pkg}" in
    sw/device/tests/sim_dv) r=0 ;;
    sw/device/tests | sw/device/tests/* | hw/top_earlgrey/sw/autogen/tests) r=1 ;;
    *) r=2 ;;
  esac
  echo "${key#* } ${r} ${pkg} ${NEWEST[${key}]}"
done | sort -k1,1 -k2,2n -k3,3 | awk '{
    run = $1
    if ($1 == last) { pkg = $3; gsub(/\//, ".", pkg); run = $1 "@" pkg }
    last = $1
    print run, $4, run
  }' > "${OUT}/all_runs.txt"
# A variant runs when its image is built.
awk 'NR == FNR { img[$1] = $2; next } $1 !~ /^#/ && NF {
       i = $1; sub(/@.*/, "", i); if (i in img) print $1, img[i], i }' \
  "${OUT}/all_runs.txt" "${HERE}/variants.txt" >> "${OUT}/all_runs.txt"
awk -v p="${PATTERN}" 'index($1, p)' "${OUT}/all_runs.txt" | sort > "${OUT}/runs.txt"
echo "== $(wc -l < "${OUT}/runs.txt") tests on $(basename "${EG2SIM}") (${EG2SIM}), ${JOBS} at a time"

run_one() {
  local name=$1 img=$2 iname=$3 dir rc outcome last insns sim wall said t0 knobs=()
  dir="${OUT}/${name}"
  mkdir -p "${dir}"
  # Tests on hold are a separate outcome from a model limitation (on_hold.txt, matched like
  # needs_bench.txt: by run or image name). The reason says whether the unavailable piece is an
  # image flow or a run that cannot fit the suite limit.
  local hold
  hold=$(awk -v t="${name}" -v i="${iname}" '$1 !~ /^#/ && NF {
           if (t ~ ("^" $1 "$") || i ~ ("^" $1 "$")) { $1 = ""; sub(/^ +/, ""); print; exit } }' \
         "${HERE}/on_hold.txt")
  if [[ -n "${hold}" ]]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" on-hold 0.00 - - \
      "on hold: ${hold}" "${iname}"
    return
  fi
  # A test only the chip bench can run (needs_bench.txt): reported, not run. A variant of such an
  # image is one too.
  local why
  why=$(awk -v t="${name}" -v i="${iname}" '$1 !~ /^#/ && NF {
          if (t ~ ("^" $1 "$") || i ~ ("^" $1 "$")) { $1 = ""; sub(/^ +/, ""); print; exit } }' \
        "${HERE}/needs_bench.txt")
  if [[ -n "${why}" ]]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" model-stop 0.00 - - "needs the chip bench: ${why}" "${iname}"
    return
  fi
  # Checks a test marks as not evaluable by the model are skipped. Monte Carlo is not enabled
  # implicitly: variants.txt gives each weekly Monte Carlo setting a separately reported run.
  grep -q -a kEg2Param_model_gaps "${img}" && knobs+=(--param model_gaps=1)
  # A must-fail image stops at its first failed check, as the chip bench runs it
  # (chip_sw_eg2_base_vseq): the check it broke must be the first to fail.
  [[ ${name} == *_inject_fail* ]] && grep -q -a kEg2StopOnFail "${img}" && knobs+=(--sym kEg2StopOnFail=1)
  # A variant's settings (variants.txt), then what the chip bench would do for this run (bench.txt:
  # the run's own lines, else, for a variant, its image's lines); an {image:...} in them becomes the
  # image's file.
  local bench own="" otp=${OTP} a tok other
  read -ra bench <<< "$(awk -v t="${name}" -v i="${iname}" -v vf="${HERE}/variants.txt" '
      FILENAME == vf { if ($1 == t) { $1 = ""; printf "%s ", $0 }; next }
      $1 == t { $1 = ""; mine = mine $0 " " }
      $1 == i && t != i { $1 = ""; img = img $0 " " }
      END { printf "%s", mine != "" ? mine : img }' "${HERE}/variants.txt" "${HERE}/bench.txt")"
  for a in "${bench[@]}"; do
    if [[ "${a}" == otp=* ]]; then
      own=$(awk -v t="${a}" '$1 == t {print $2}' "${OUT}/otp_images.txt")
      continue
    fi
    while [[ "${a}" =~ \{image:[^}]*\} ]]; do
      tok=${BASH_REMATCH[0]}
      other=$(awk -v t="${tok}" '$1 == t {print $2}' "${OUT}/bench_images.txt")
      if [[ -z "${other}" || "${other}" == MISSING ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" model-stop 0.00 - - \
          "the image bench.txt names, ${tok}, is not built" "${iname}"
        return
      fi
      a=${a//"${tok}"/"${other}"}
    done
    knobs+=("${a}")
  done
  # Its own OTP image: the variant's, else the test's (otp.txt).
  local base=${iname%_inject_fail}
  [[ -z "${own}" ]] && own=$(awk -v t="${base%_cheriot}" '$1 == t {print $2}' "${OUT}/otp_images.txt")
  if [[ "${own}" == MISSING:* ]]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" model-stop 0.00 - - \
      "its life-cycle OTP image ${own#MISSING:} is not built; img_rma was not substituted" \
      "${iname}"
    return
  fi
  [[ -n "${own}" && "${own}" != MISSING:* ]] && otp=${own}
  t0=${EPOCHREALTIME}
  # The log is cut at 100 MB, which ends the run (2026-10-29: a model logging every instruction
  # wrote 10 GB).
  # The exact run, to repeat by hand: <dir>/rerun.sh [eg2sim options], logging to <dir>/rerun/.
  # Options given are added after the suite's; a later --max-insns wins (e.g. --max-insns
  # 2000000000 for a no-end test).
  {
    echo '#!/usr/bin/env bash'
    echo "# ${name} as the bare-metal suite ran it; extra eg2sim options are added after these."
    printf 'mkdir -p %q && exec env -u LD_LIBRARY_PATH -u LD_PRELOAD' "${dir}/rerun"
    printf ' %q' "${EG2SIM}" --rom "${ROM}" --test "${img}" --otp "${otp}" --max-insns 200000000 \
      "${knobs[@]}" --log-dir "${dir}/rerun"
    printf ' "$@"\n'
  } > "${dir}/rerun.sh"
  chmod +x "${dir}/rerun.sh"
  (ulimit -v 4000000; timeout 1800 env -u LD_LIBRARY_PATH -u LD_PRELOAD "${EG2SIM}" --rom "${ROM}" \
    --test "${img}" --otp "${otp}" --max-insns 200000000 "${knobs[@]}" --log-dir "${dir}" 2>&1 \
    | head -c 100000000 > "${dir}/run.log"; exit "${PIPESTATUS[0]}")
  rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b - a}')
  case "${rc}" in
    0) [[ ${name} == *_inject_fail* ]] && outcome=fail || outcome=pass ;;
    3) [[ ${name} == *_inject_fail* ]] && outcome=must-fail || outcome=fail ;;
    2) outcome=model-stop ;;
    4) outcome=no-end ;;
    *) outcome=fail ;;
  esac
  # What happened: the model's last word on the run (its end, stop or failure reason, with the
  # instructions and simulated time), and the test's own first error message, when it gave one.
  last=$(grep -a "after [0-9]* instructions" "${dir}/run.log" | tail -1)
  insns=$(sed -n 's/.* after \([0-9]*\) instructions.*/\1/p' <<< "${last}")
  sim=$(sed -n 's/.* instructions, \([0-9.]*\) us.*/\1/p' <<< "${last}")
  said=$(sed "s/^eg2sim: //; s/ after [0-9]* instructions.*//" <<< "${last}" | cut -c1-110)
  # A test that ends on purpose with a message of its own (exit_strings.txt) passes on that message.
  local exit_ok
  exit_ok=$(awk -v t="${iname}" '$1 == t { $1 = ""; sub(/^ +/, ""); print; exit }' "${HERE}/exit_strings.txt")
  if [[ -n "${exit_ok}" ]] && grep -a -q -F -- "${exit_ok}" "${dir}/run.log"; then
    outcome=pass
    said="ended with the line its BUILD target expects: ${exit_ok}"
  fi
  # The log limit ends the run, so it is a failure even if eg2sim reaches its instruction limit
  # while head is closing the pipe.  Do not let host timing change the reported outcome.
  if [[ $(stat -c %s "${dir}/run.log") -ge 100000000 ]]; then
    outcome=fail
    said="log over 100 MB: run cut"
  fi
  # A NIST image may return success after reporting every selected vector skipped. That is not a
  # pass: no cryptographic result was compared. The inject-fail image has its own deliberate
  # "no vector ran" failure, so retain its must-fail grade.
  if [[ "${name}" == eg2_nist_* && "${name}" != *_inject_fail* && "${outcome}" == pass ]] &&
     ! grep -a -q 'NIST pass .*: [1-9][0-9]* passed' "${dir}/run.log"; then
    outcome=model-stop
    said="NIST test compared no vector; every selected vector was skipped"
  fi
  # The first check that failed: OpenTitan's CHECK and DIF macros log at info or error level.
  err=$(grep -a -m1 "CHECK-fail\|CHECK-STATUS-fail\|DIF-fail\|^sw: E \|^sw: W \|^uart0| FAIL! MISMATCH" \
    "${dir}/run.log" | sed 's/^sw: [IEW] //; s/^uart0| //' | cut -c1-140)
  [[ -n "${err}" ]] && said="${said}; the test: ${err}"
  # A must-fail run counts only when the check its build broke is the one that failed
  # (eg2_result.h eg2_inject_armed/eg2_inject_failed; Codex review 2 Nov: any exit 3 was accepted).
  # An image built before the markers says neither line: still graded on the exit, marked so.
  if [[ ${outcome} == must-fail ]]; then
    local armed caught
    armed=$(grep -a -m1 -o "EG2_INJECT_ARMED .*" "${dir}/run.log" | cut -c18- | tr -d '\r')
    caught=$(grep -a -m1 -o "EG2_INJECT_CAUGHT .*" "${dir}/run.log" | cut -c19- | tr -d '\r')
    local line
    line=$(awk -v t="${name}" '$1 == t { $1 = ""; sub(/^ /, ""); print; exit }' "${HERE}/inject_lines.txt")
    if [[ -n "${caught}" ]]; then
      said="caught by the check it broke (${caught}); ${said}"
    elif [[ -n "${armed}" ]]; then
      outcome=fail
      said="failed, but not on the check it broke (${armed}); ${said}"
    elif [[ -n "${line}" ]] && grep -a -q -F -- "${line}" "${dir}/run.log"; then
      said="caught by the check it broke (${line}, inject_lines.txt); ${said}"
    elif [[ -n "${line}" ]]; then
      outcome=fail
      said="failed, but without its line \"${line}\" (inject_lines.txt); ${said}"
    else
      said="cause not checked (image without EG2_INJECT lines); ${said}"
    fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" "${outcome}" "${wall}" "${insns:--}" "${sim:--}" "${said}" \
    "${iname}"
}
export -f run_one
export EG2SIM ROM OTP OUT HERE
start=$(date +%s)
# raw.txt: test, outcome, wall seconds, instructions, simulated us, what happened, image (tab-separated).
xargs -a "${OUT}/runs.txt" -P "${JOBS}" -L1 bash -c 'run_one "$@"' _ | sort > "${OUT}/raw.txt"
cut -f1,2 "${OUT}/raw.txt" | tr '\t' ' ' > "${OUT}/results.txt"
awk -F'\t' '$2 != "pass" && $2 != "must-fail" {print $1 ": " $6}' "${OUT}/raw.txt" > "${OUT}/reasons.txt"
secs=$(( $(date +%s) - start ))

# The tests expected.txt lists that a full run did not run (their image is not built in EG2_ROOT).
: > "${OUT}/not_run.txt"
if [[ -z "${PATTERN}" && -f "${EXPECTED}" ]]; then
  awk 'NR == FNR {ran[$1] = 1; next} !($1 in ran)' "${OUT}/results.txt" "${EXPECTED}" > "${OUT}/not_run.txt"
fi

if [[ ${UPDATE} -eq 1 ]]; then
  # Only the tests that ran get new lines; a test that did not run (a partial run, or an image not
  # built) keeps its line. A test removed for good is removed from expected.txt by hand.
  if [[ -f "${EXPECTED}" ]]; then
    { awk 'NR == FNR {ran[$1] = 1; next} !($1 in ran)' "${OUT}/results.txt" "${EXPECTED}"
      cat "${OUT}/results.txt"; } | sort > "${EXPECTED}.new"
    mv "${EXPECTED}.new" "${EXPECTED}"
  else
    cp "${OUT}/results.txt" "${EXPECTED}"
  fi
  echo "== expected.txt updated from this run; review the diff"
fi

# The report: outcomes and every difference from expected.txt; then each category (categories.txt)
# with its counts, and every test under it: outcome, wall time, instructions, simulated time, and
# what happened.
{
  echo "Bare-metal suite on $(basename "${EG2SIM}"), $(date -u +%FT%TZ), ${secs} s for $(wc -l < "${OUT}/results.txt") tests"
  awk '{n[$2]++} END {for (o in n) printf "  %-16s %d\n", o == "must-fail" ? "pass (must fail)" :
       o == "on-hold" ? "on hold" : o, n[o]}' "${OUT}/results.txt" | sort
  [[ -f "${EXPECTED}" ]] || touch "${EXPECTED}"
  join -a1 -e none -o 0,1.2,2.2 "${OUT}/results.txt" <(sort "${EXPECTED}") \
    | awk '$3 == "none" {print "  NEW      " $1 " " $2; next} $2 != $3 {print "  CHANGED  " $1 ": " $3 " -> " $2}'
  awk '{print "  NOT RUN  " $1 ": expected " $2 ", no image built in EG2_ROOT"}' "${OUT}/not_run.txt"
  awk -F'\t' -v cats="${HERE}/categories.txt" -v known="${HERE}/known.txt" \
      -v bench="${HERE}/bench_checks.txt" -v bench_run="${HERE}/bench_checks_run.txt" \
      -v outdir="${OUT}" '
    BEGIN {
      while ((getline line < known) > 0) {
        if (line ~ /^#/ || line !~ /[^ ]/) continue
        split(line, f, " "); sub(/^[^ ]+ +/, "", line); why[f[1]] = line
      }
      # The tests whose chip-bench sequence checks something itself (gen_bench_checks.sh).
      while ((getline line < bench) > 0) {
        if (line ~ /^#/ || line !~ /[^ ]/) continue
        split(line, f, " "); bseq[f[1]] = f[2]
      }
      while ((getline line < bench_run) > 0) {
        if (line ~ /^#/ || line !~ /[^ ]/) continue
        split(line, f, " "); delete bseq[f[1]]
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
      n[c, $2]++; total[c]++; wall[c] += $3
      # The outcome column holds only the outcome; notes go in "what happened".
      out = $2 == "must-fail" ? "pass (must fail)" : $2 == "on-hold" ? "on hold" : $2
      said = $6
      # A variant runs the bench sequence of its image.
      b = ($1 in bseq) ? $1 : $7
      if ($2 == "pass" && (b in bseq)) {
        nb[c]++; nball++
        said = said "; chip-bench checks not run here (in " bseq[b] ")"
      }
      if ($1 in why) said = "known: " why[$1] "; " said
      list[c] = list[c] sprintf("    %-16s %7.2f s %10s %12s  %-58s %s\n", out, $3, $4, $5, $1, said)
      if ($2 == "no-end") noend[++nne] = $1
    }
    END {
      printf "  of the passes, %d with bench checks not run (bench_checks.txt)\n", nball
      printf "\nBy category (\"bench not run\": passes whose chip-bench checks the model does not run):\n"
      printf "  %-58s %5s %5s %13s %16s %5s %10s %8s %6s %8s\n", "", "tests", "pass", "bench not run",
             "pass (must fail)", "fail", "model-stop", "on hold", "no-end", "time"
      for (k = 1; k <= nc; k++) if (total[o = order[k]])
        printf "  %-58s %5d %5d %13d %16d %5d %10d %8d %6d %6.0f s\n", o, total[o], n[o, "pass"], nb[o],
               n[o, "must-fail"], n[o, "fail"], n[o, "model-stop"], n[o, "on-hold"], n[o, "no-end"], wall[o]
      print "\nEvery test by category"
      printf "    %-16s %9s %10s %12s  %-58s %s\n", "outcome", "time", "insns", "sim. us", "test",
             "what happened"
      for (k = 1; k <= nc; k++) if (total[o = order[k]]) printf "\n  == %s\n%s", o, list[o]
      printf "\nTo rerun a test by hand (it logs to <test>/rerun/; eg2sim options given are added):\n"
      printf "  %s/<test>/rerun.sh\n", outdir
      if (nne) {
        printf "A no-end test stopped at the suite'\''s instruction limit (200 million): rerun it with a\n"
        printf "higher limit, e.g. ten times:\n"
        printf "  %s/<test>/rerun.sh --max-insns 2000000000\n", outdir
        printf "The no-end tests:\n"
        for (k = 1; k <= nne; k++) printf "  %s\n", noend[k]
      }
    }' "${OUT}/raw.txt"
} | tee "${OUT}/report.txt"
grep -q "^  CHANGED\|^  NOT RUN" "${OUT}/report.txt" && exit 1
exit 0
