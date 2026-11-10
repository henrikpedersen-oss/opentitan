#!/usr/bin/env bash
# CHERIoT memory subsystem bench: every run of cms_regress.list (build first unless -k).
#
# Usage: ./cms_regress.sh [-c] [-k] [-m <1|2>] [-b <seed base>] [-f <name regex>]
#   -c  coverage: build instrumented, collect every run, merge into xlm_out/coverage_merged
#   -m  the C reference model beside the scoreboard in every run (+cms_cmodel: 1 reports, 2 fails)
#   -k  keep the existing build
#   -b  first seed (default 1)
#   -f  only the list entries whose name matches the regex
# Goes on after a failing run; the verdict is scripts/cms_results.py's
# (cms_results.py), which also requires the _corrupt_ runs to fail. Exit status: 0
# iff every ordinary run passed, every @xfail run failed exactly as cms_expected_failures.hjson
# declares (a known gap: not a pass, but not a regression failure either), and every _corrupt_ run
# failed from its injected corruption.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_TOP="$(realpath "${SCRIPT_DIR}/../../../../..")"
if ! command -v xrun >/dev/null 2>&1; then
  echo "$(basename "$0"): xrun is not on PATH; run it in the EDA environment" >&2
  exit 1
fi

COV=0; KEEP=0; BASE=1; FILTER="."; CMODEL=""
while getopts "ckm:b:f:" opt; do
  case $opt in
    c) COV=1 ;;
    k) KEEP=1 ;;
    m) CMODEL="+cms_cmodel=$OPTARG" ;;
    b) BASE="$OPTARG" ;;
    f) FILTER="$OPTARG" ;;
    *) exit 2 ;;
  esac
done
cov_flag=$([[ ${COV} -eq 1 ]] && echo "-c" || true)

if [[ ${KEEP} -eq 0 ]]; then
  "${SCRIPT_DIR}/cms_xlm_build.sh" ${cov_flag} || { echo "[cms_regress] build failed" >&2; exit 1; }
fi
[[ ${COV} -eq 1 ]] && rm -rf "${SCRIPT_DIR}/xlm_out/cov_work"

# Every run of the list, up to JOBS at a time (scripts/run_parallel.py; the runs share the
# build read-only, and each has its own log, result and coverage test <name>.<seed>). A _corrupt_
# run's job is inverted: it succeeds only when the run fails. Per-job output in xlm_out/parallel/.
JOBFILE="${SCRIPT_DIR}/xlm_out/parallel/jobs.txt"
mkdir -p "$(dirname "${JOBFILE}")"
: > "${JOBFILE}"
declare -A job_class
while read -r name test seeds plusargs; do
  [[ -z "${name}" || "${name}" == \#* ]] && continue
  [[ "${name}" =~ ${FILTER} ]] || continue
  xfail=""
  run_args=(${CMODEL})
  for arg in ${plusargs}; do
    if [[ "${arg}" == @xfail=* ]]; then xfail="${arg#@xfail=}"; else run_args+=("${arg}"); fi
  done
  for ((s = BASE; s < BASE + seeds; s++)); do
    # </dev/null: xmsim reads stdin as Tcl.
    run="'${SCRIPT_DIR}/cms_xlm_run.sh' -t ${test} -s ${s} -n ${name} ${cov_flag} ${xfail:+-x ${xfail}} -- ${run_args[*]-} </dev/null"
    job_class["${name}.${s}"]=$([[ -n "${xfail}" ]] && echo DECLARED || echo PASS)
    if [[ "${name}" == *_corrupt_* ]]; then
      run="if ${run}; then echo '[cms_regress] ${name}.${s} passed; must fail'; exit 1; fi"
      job_class["${name}.${s}"]=CORRUPT
    fi
    printf '%s\t%s\n' "${name}.${s}" "${run}" >> "${JOBFILE}"
  done
done < "${SCRIPT_DIR}/cms_regress.list"
expected=$(grep -c . "${JOBFILE}")
python3 "${REPO_TOP}/hw/ip/cheriot_mem_sys/dv/cms/scripts/run_parallel.py" "${JOBFILE}" \
  --jobs "${JOBS:-32}" --log-dir "${SCRIPT_DIR}/xlm_out/parallel" --label cms_regress </dev/null \
  | tee "${SCRIPT_DIR}/xlm_out/parallel/summary.log"
pass=0; fail=0; normal_pass=0; xfail_pass=0; corrupt_pass=0; bad=()
while read -r job status _; do
  if [[ "${status}" == PASS ]]; then
    pass=$((pass + 1))
    case "${job_class[${job}]:-PASS}" in
      DECLARED) xfail_pass=$((xfail_pass + 1)) ;;
      CORRUPT) corrupt_pass=$((corrupt_pass + 1)) ;;
      *) normal_pass=$((normal_pass + 1)) ;;
    esac
  else
    fail=$((fail + 1)); bad+=("${job}")
  fi
done < <(sed -n '/\] summary$/,$p' "${SCRIPT_DIR}/xlm_out/parallel/summary.log" | grep -E '^  [^ ]+ +(PASS|FAIL)')

if [[ ${COV} -eq 1 ]]; then
  IMC_TCL=$(mktemp /tmp/cms_imc_XXXXXX.tcl)
  # Then the per-covergroup report the testplan report reads (xlm_out/cg_detail.txt), and the code
  # coverage of the subsystem: -covdut cheriot_mem_sys roots the model at the subsystem, so 'cheriot_mem_sys' is
  # the whole of it (cov_summary.txt) and its children give the per-block split (cov_hier.txt).
  # Until 2026-10-10 the merge collected code coverage but no report ever showed it.
  printf 'merge %s/xlm_out/cov_work/cms/* -out %s/xlm_out/coverage_merged -overwrite -initial_model union_all\nload -run %s/xlm_out/coverage_merged\ncatch {report -detail -type -all -metrics covergroup -source off -out %s/xlm_out/cg_detail.txt -overwrite}\ncatch {report -summary -inst cheriot_mem_sys -metrics all -all -cumulative on -local off -grading covered -out %s/xlm_out/cov_summary.txt}\ncatch {report -summary -inst {cheriot_mem_sys cheriot_mem_sys/*} -metrics all -all -cumulative on -local off -grading covered -out %s/xlm_out/cov_hier.txt}\nexit\n' \
    "${SCRIPT_DIR}" "${SCRIPT_DIR}" "${SCRIPT_DIR}" "${SCRIPT_DIR}" "${SCRIPT_DIR}" "${SCRIPT_DIR}" > "${IMC_TCL}"
  imc -64bit -licqueue -exec "${IMC_TCL}" -logfile "${SCRIPT_DIR}/xlm_out/coverage_merge.log" \
    || echo "[cms_regress] WARNING: coverage merge failed; see xlm_out/coverage_merge.log" >&2
  rm -f "${IMC_TCL}"
fi

echo "[cms_regress] ${pass} as expected (${normal_pass} PASS, ${xfail_pass} FAIL (known gap) as declared, ${corrupt_pass} injected failures caught), ${fail} not"
if [[ $((pass + fail)) -ne ${expected} ]]; then
  echo "[cms_regress] ERROR: $((pass + fail)) runs, ${expected} listed: entries were skipped" >&2
  fail=$((fail + 1))
fi
for b in "${bad[@]+"${bad[@]}"}"; do echo "  FAILED: ${b}"; done
echo "[cms_regress] Results: ${SCRIPT_DIR}/xlm_out/results/*.result (graded by scripts/cms_results.py)"
[[ ${fail} -eq 0 ]]
