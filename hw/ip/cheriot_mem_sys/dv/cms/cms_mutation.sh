#!/usr/bin/env bash
# CHERIoT memory subsystem bench: the mutation check. One build, then every run of
# hw/ip/cheriot_mem_sys/dv/cms/scripts/cms_mutation_check.py --list-runs with +cms_mutate=<mutation>, then the grade.
#
# Usage: ./cms_mutation.sh [-k] [-m <mutation>]
#   -k  keep the existing build in xlm_mutation_out
#   -m  this mutation only
# Everything goes to xlm_mutation_out/: the build, and results/<mutation>/<name>.<seed>.{log,result}.
# The regression's xlm_out/ (its build, results and coverage, which the testplan report reads) is
# never touched. No coverage: a mutant's coverage must never reach a merged database.
# Every run is expected to fail, so a failing run does not stop the loop. Exit status: the
# grader's (non-zero on any MISSED, NO LOG or INACTIVE, or a mutation no test caught).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_TOP="$(realpath "${SCRIPT_DIR}/../../../../..")"
if ! command -v xrun >/dev/null 2>&1; then
  echo "$(basename "$0"): xrun is not on PATH; run it in the EDA environment" >&2
  exit 1
fi

KEEP=0; ONLY=""
while getopts "km:" opt; do
  case $opt in
    k) KEEP=1 ;;
    m) ONLY="$OPTARG" ;;
    *) exit 2 ;;
  esac
done

OUT=xlm_mutation_out
PY=python3
CHECK="${REPO_TOP}/hw/ip/cheriot_mem_sys/dv/cms/scripts/cms_mutation_check.py"
only_arg=()
[[ -n "${ONLY}" ]] && only_arg=(--only "${ONLY}")

# Read the whole list first: xmsim reads stdin, and the grader must not be half-read either.
mapfile -t runs < <("${PY}" "${CHECK}" --list-runs "${only_arg[@]+"${only_arg[@]}"}")
[[ ${#runs[@]} -gt 0 ]] || { echo "[cms_mutation] no runs listed" >&2; exit 1; }

if [[ ${KEEP} -eq 0 ]]; then
  "${SCRIPT_DIR}/cms_xlm_build.sh" -o "${OUT}" || { echo "[cms_mutation] build failed" >&2; exit 1; }
fi
# No log of an earlier round may stand in for a run of this one.
if [[ -n "${ONLY}" ]]; then
  rm -rf "${SCRIPT_DIR:?}/${OUT}/results/${ONLY}"
else
  rm -rf "${SCRIPT_DIR:?}/${OUT}/results"
fi

# Every run up to JOBS at a time (scripts/run_parallel.py), each with its own log and result
# under results/<mutation>/; their exit codes are not the verdict (a mutant run should fail), the
# grader below is. Per-job output in xlm_mutation_out/parallel/.
JOBFILE="${SCRIPT_DIR}/${OUT}/parallel/jobs.txt"
mkdir -p "$(dirname "${JOBFILE}")"
: > "${JOBFILE}"
for r in "${runs[@]}"; do
  IFS='|' read -r mut test name seed plusargs <<< "${r}"
  printf '%s\t%s\n' "${mut}.${name}.${seed}" \
    "'${SCRIPT_DIR}/cms_xlm_run.sh' -o ${OUT} -r results/${mut} -t ${test} -s ${seed} -n ${name} -- +cms_mutate=${mut} ${plusargs} </dev/null" \
    >> "${JOBFILE}"
done
"${PY}" "${REPO_TOP}/hw/ip/cheriot_mem_sys/dv/cms/scripts/run_parallel.py" "${JOBFILE}" --jobs "${JOBS:-32}" \
  --log-dir "${SCRIPT_DIR}/${OUT}/parallel" --ignore-rc --label cms_mutation </dev/null

"${PY}" "${CHECK}" --check --results "${SCRIPT_DIR}/${OUT}/results" "${only_arg[@]+"${only_arg[@]}"}"
