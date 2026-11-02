#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, EG2_SCRATCH, SW_SUITES_OUT
OUT="${SW_SUITES_OUT}/nist-ref"
# Built by eg2_top_verification: make nist-cavp (the archives the fork pins), nist-cavp-check (the
# C references), nist-otbn-elfs (the three OTBN programs).
VECTORS="${NIST_CAVP:-${EG2_SCRATCH}/nist_cavp}"
CHECK="${NIST_CAVP_CHECK:-${EG2_SCRATCH}/nist/nist_cavp_check}"
ELFS="${NIST_OTBN_ELFS:-${EG2_SCRATCH}/nist_otbn}"
OTBN_CHECK="${EG2_ROOT}/hw/top_earlgrey/dv/eg2_model/nist/nist_otbn_check.py"
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
mkdir -p "${OUT}"

# The inputs, built by eg2_top_verification's targets (make sw-suite-nist-ref builds them first).
[[ -d "${VECTORS}" ]] || { echo "run.sh: no NIST archives in ${VECTORS} (make nist-cavp)" >&2; exit 2; }
[[ -x "${CHECK}" ]] || { echo "run.sh: no ${CHECK} (make nist-cavp-check)" >&2; exit 2; }
for e in run_p256 run_p384 run_rsa; do
  [[ -f "${ELFS}/${e}.elf" ]] || { echo "run.sh: no ${ELFS}/${e}.elf (make nist-otbn-elfs)" >&2; exit 2; }
done

start=$(date +%s)
: > "${OUT}/raw.txt"

# A stable report key from the checker's human-readable file and implementation name.
parse_cavp() {
  local log=$1 suffix=$2 expect=$3 wall=$4
  awk -v suffix="${suffix}" -v expect="${expect}" -v wall="${wall}" '
    /^PASS |^FAIL / {
      verdict = $1; sub(/^(PASS|FAIL) +/, "")
      if (!match($0, / +[0-9]+ passed +[0-9]+ failed/)) next
      name = substr($0, 1, RSTART - 1); detail = substr($0, RSTART + 1)
      gsub(/[^A-Za-z0-9]+/, ".", name); sub(/^\.+/, "", name); sub(/\.+$/, "", name)
      out = verdict == "PASS" ? expect : (expect == "CAUGHT" ? "MISSED" : "fail")
      printf "cavp.%s%s\t%s\t0\t-\tbatch wall %s s; %s\n", name, suffix, out, wall, detail
    }' "${log}"
}

if [[ -z "${PATTERN}" || "cavp" == *"${PATTERN}"* ]]; then
  t0=${EPOCHREALTIME}
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "${CHECK}" "${VECTORS}" > "${OUT}/cavp.log" 2>&1
  plain_rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b-a}')
  parse_cavp "${OUT}/cavp.log" "" pass "${wall}" >> "${OUT}/raw.txt"
  [[ ${plain_rc} -eq 0 ]] || echo "cavp.infrastructure\tfail\t${wall}\t-\tchecker exit ${plain_rc}" >> "${OUT}/raw.txt"

  t0=${EPOCHREALTIME}
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "${CHECK}" --corrupt "${VECTORS}" \
    > "${OUT}/cavp-corrupt.log" 2>&1
  corrupt_rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b-a}')
  parse_cavp "${OUT}/cavp-corrupt.log" ".corrupt" CAUGHT "${wall}" >> "${OUT}/raw.txt"
  [[ ${corrupt_rc} -eq 0 ]] || echo "cavp.corrupt.infrastructure\tMISSED\t${wall}\t-\tchecker exit ${corrupt_rc}" >> "${OUT}/raw.txt"
fi

# Expand the OTBN vector-file lines and one RSA-2048 signing vector per supported hash into
# independently timed test cases. This is the reference checker's NIST_OTBN_RSA_SIGN=1 coverage.
: > "${OUT}/cases.txt"
while IFS='|' read -r name only rsa slice; do
  [[ "${name}" == \#* || -z "${name}" ]] && continue
  for kind in plain corrupt; do
    test_name="${name}"; [[ "${kind}" == corrupt ]] && test_name+=".corrupt"
    [[ -z "${PATTERN}" || "${test_name}" == *"${PATTERN}"* ]] || continue
    printf '%s|%s|%s|%s|%s\n' "${test_name}" "${kind}" "${only}" "${rsa}" "${slice}" >> "${OUT}/cases.txt"
  done
done < "${HERE}/runs.txt"
for bits in 2048; do
  for i in $(seq 0 2); do
    name=$(printf 'otbn.RSA-SigGen15.%s.vector-%02d' "${bits}" "${i}")
    slice="${i}:$((i + 1))"
    for kind in plain corrupt; do
      test_name="${name}"; [[ "${kind}" == corrupt ]] && test_name+=".corrupt"
      [[ -z "${PATTERN}" || "${test_name}" == *"${PATTERN}"* ]] || continue
    printf '%s|%s|%s|1|%s\n' "${test_name}" "${kind}" "SigGen15_186-3.txt ${bits}" "${slice}" >> "${OUT}/cases.txt"
    done
  done
done

run_otbn() {
  local rec=$1 name kind only rsa slice dir log t0 wall rc out line budget
  IFS='|' read -r name kind only rsa slice <<< "${rec}"
  dir="${OUT}/${name}"; mkdir -p "${dir}"
  log="${dir}/run.log"; budget=0
  [[ "${kind}" == corrupt ]] && budget=1
  local -a args=(--eg2 "${EG2_ROOT}" --jobs 1 --elf-dir "${ELFS}" --budget "${budget}"
                 --rsa-sign "${rsa}" --only "${only}")
  [[ -n "${slice}" ]] && args+=(--slice "${slice}")
  [[ "${kind}" == corrupt ]] && args+=(--corrupt)
  args+=("${VECTORS}")
  {
    echo '#!/usr/bin/env bash'
    echo '# This exact NIST OTBN case, from the repository root.'
    printf 'ulimit -v 8388608\nexec env -u LD_LIBRARY_PATH -u LD_PRELOAD nix run %q -- python3 -I %q' \
      "${EG2_ROOT}#eda" "${OTBN_CHECK}"
    printf ' %q' "${args[@]}"
    printf '\n'
  } > "${dir}/rerun.sh"
  chmod +x "${dir}/rerun.sh"
  t0=${EPOCHREALTIME}
  (ulimit -v 8388608; timeout 1800 "${dir}/rerun.sh") > "${log}" 2>&1
  rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b-a}')
  line=$(grep -a -m1 '^PASS \|^FAIL ' "${log}" | cut -c1-180)
  if [[ ${rc} -eq 0 && "${line}" == PASS\ * ]]; then
    [[ "${kind}" == corrupt ]] && out=CAUGHT || out=pass
  elif [[ ${rc} -eq 124 ]]; then
    out=on-hold; line="over the 1,800 s suite timeout; unsplittable only if this is a one-vector slice"
  else
    [[ "${kind}" == corrupt ]] && out=MISSED || out=fail
    [[ -n "${line}" ]] || line="checker exit ${rc}"
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "${name}" "${out}" "${wall}" \
    "$(sed -n 's/.*(INSN_CNT \([^)]*\)).*/\1/p' <<< "${line}")" "${line}"
}
export -f run_otbn
export OUT EG2_ROOT ELFS VECTORS OTBN_CHECK
if [[ -s "${OUT}/cases.txt" ]]; then
  tr '\n' '\0' < "${OUT}/cases.txt" | xargs -0 -P "${JOBS}" -I{} bash -c 'run_otbn "$1"' _ {} \
    >> "${OUT}/raw.txt"
fi

# RSA-3072 signing cannot be divided below one vector. Generate its exact hand command and report
# it without spending 30 minutes to rediscover the measured limit on every suite run.
for i in $(seq 0 2); do
  for kind in plain corrupt; do
    name=$(printf 'otbn.RSA-SigGen15.3072.vector-%02d' "${i}")
    [[ "${kind}" == corrupt ]] && name+=".corrupt"
    [[ -z "${PATTERN}" || "${name}" == *"${PATTERN}"* ]] || continue
    dir="${OUT}/${name}"; mkdir -p "${dir}"
    slice="${i}:$((i + 1))"
    corrupt=(); [[ "${kind}" == corrupt ]] && corrupt=(--corrupt)
    {
      echo '#!/usr/bin/env bash'
      echo '# One unsplittable RSA-3072 signing vector, held out of the 30-minute suite.'
      printf 'ulimit -v 8388608\nexec env -u LD_LIBRARY_PATH -u LD_PRELOAD nix run %q -- python3 -I %q' \
        "${EG2_ROOT}#eda" "${OTBN_CHECK}"
      printf ' %q' --eg2 "${EG2_ROOT}" --jobs 1 --elf-dir "${ELFS}" --budget 0 --rsa-sign 1 \
        --only 'SigGen15_186-3.txt 3072' --slice "${slice}" "${corrupt[@]}" "${VECTORS}"
      printf '\n'
    } > "${dir}/rerun.sh"
    chmod +x "${dir}/rerun.sh"
    printf '%s\ton-hold\t0\t52000000\tunsplittable RSA-3072 signature: 52,000,000 OTBN instructions, 1,818 s extrapolated at the measured 28,600 instructions/s\n' \
      "${name}" >> "${OUT}/raw.txt"
  done
done

sort -o "${OUT}/raw.txt" "${OUT}/raw.txt"
cut -f1,2 "${OUT}/raw.txt" | tr '\t' ' ' > "${OUT}/results.txt"
secs=$(( $(date +%s) - start ))
if [[ ${UPDATE} -eq 1 ]]; then
  if [[ -n "${PATTERN}" && -f "${EXPECTED}" ]]; then
    { awk 'NR == FNR {ran[$1] = 1; next} !($1 in ran)' "${OUT}/results.txt" "${EXPECTED}"
      cat "${OUT}/results.txt"; } | sort > "${EXPECTED}.new"
    mv "${EXPECTED}.new" "${EXPECTED}"
  else
    cp "${OUT}/results.txt" "${EXPECTED}"
  fi
  echo "== expected.txt updated from this run; review the diff"
fi

{
  echo "NIST host-reference suite, $(date -u +%FT%TZ), ${secs} s for $(wc -l < "${OUT}/results.txt") tests"
  # The report says "pass (caught)" for CAUGHT and "fail (not caught)" for MISSED; results.txt and
  # expected.txt keep the one-word outcomes.
  awk '{n[$2]++} END {for (o in n) printf "  %-17s %d\n", o == "CAUGHT" ? "pass (caught)" : o == "MISSED" ? "fail (not caught)" : o == "on-hold" ? "on hold" : o, n[o]}' "${OUT}/results.txt" | sort
  [[ -f "${EXPECTED}" ]] || touch "${EXPECTED}"
  join -a1 -e none -o 0,1.2,2.2 "${OUT}/results.txt" <(sort "${EXPECTED}") \
    | awk '$3 == "none" {print "  NEW      " $1 " " $2; next} $2 != $3 {print "  CHANGED  " $1 ": " $3 " -> " $2}'
  awk -F'\t' -v cats="${HERE}/categories.txt" -v outdir="${OUT}" '
    BEGIN {
      while ((getline line < cats) > 0) {
        if (line ~ /^#/ || line !~ /[^ ]/) continue
        split(line, f, " "); pat[++np] = f[1]; sub(/^[^ ]+ +/, "", line); cat[np] = line
        if (!(line in seen)) { seen[line] = 1; order[++nc] = line }
      }
      order[++nc] = "Other"
    }
    {
      c = "Other"; for (i = 1; i <= np; i++) if ($1 ~ pat[i]) { c = cat[i]; break }
      n[c, $2]++; total[c]++; wall[c] += $3
      out = $2 == "CAUGHT" ? "pass (caught)" : $2 == "MISSED" ? "fail (not caught)" : $2 == "on-hold" ? "on hold" : $2
      list[c] = list[c] sprintf("    %-17s %8.2f s %12s  %-64s %s\n", out, $3, $4, $1, substr($5, 1, 180))
      if ($2 == "on-hold") hold[++nh] = $1
    }
    END {
      printf "\nBy category:\n  %-58s %5s %5s %13s %5s %17s %8s %8s\n", "", "tests", "pass", "pass (caught)", "fail", "fail (not caught)", "on hold", "time"
      for (k = 1; k <= nc; k++) if (total[o = order[k]])
        printf "  %-58s %5d %5d %13d %5d %17d %8d %6.0f s\n", o, total[o], n[o,"pass"], n[o,"CAUGHT"], n[o,"fail"], n[o,"MISSED"], n[o,"on-hold"], wall[o]
      print "\nEvery test by category"
      printf "    %-17s %10s %12s  %-64s %s\n", "outcome", "time", "insns", "test", "what happened"
      for (k = 1; k <= nc; k++) if (total[o = order[k]]) printf "\n  == %s\n%s", o, list[o]
      if (nh) {
        print "\nOn-hold runs (one vector alone exceeds 30 minutes); run by hand:"
        for (k = 1; k <= nh; k++) printf "  %s/%s/rerun.sh\n", outdir, hold[k]
      }
    }' "${OUT}/raw.txt"
} | tee "${OUT}/report.txt"
grep -q '^  CHANGED\|^  NEW' "${OUT}/report.txt" && exit 1
exit 0
