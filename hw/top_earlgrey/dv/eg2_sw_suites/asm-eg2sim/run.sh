#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# The assembler suite on the Earl Grey 2 SystemC model. The same riscv-tests and architecture
# tests as the Simple System runner are built at the chip's RRAM address, with a local reset
# handoff and verdict environment, then run on eg2sim without ROM or test images from Bazel; the
# OTP image is the EG2 fork's RMA image (EG2_ROOT), so that the life cycle lets the CPU run.
#
#   run.sh [--update] [-j JOBS] [PATTERN]

set -uo pipefail
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, EG2SIM, SW_SUITES_OUT, IBEX_CV
IBEX="${IBEX:-${IBEX_CV}/ibex}"
OUT="${SW_SUITES_OUT}/asm-eg2sim"
RAND="${EG2_ROOT}/sw/device/lib/testing/eg2_rand"
EXPECTED="${HERE}/expected.txt"
UPDATE=0
JOBS=4
MAX_INSNS=2000000
while [[ $# -gt 0 ]]; do
  case "$1" in
    --update) UPDATE=1; shift ;;
    -j) JOBS="$2"; shift 2 ;;
    *) break ;;
  esac
done
PATTERN="${1:-}"

command -v riscv32-unknown-elf-gcc >/dev/null || {
  echo "run.sh: riscv32-unknown-elf-gcc is not on PATH (make sw-suite-asm-eg2sim uses the Ibex Nix shell)" >&2
  exit 2
}
[[ -x "${EG2SIM}" ]] || { echo "run.sh: no ${EG2SIM} (make eg2sim)" >&2; exit 2; }
# The chip starts in the life-cycle state its OTP image holds; without one it is RAW, where lc_ctrl
# keeps the CPU off (lc_cpu_en). Use the RMA image, as the bare-metal suite does.
OTP=$(find -L "${EG2_ROOT}/bazel-out/" -path "*bin/hw/top_earlgrey/data/otp/img_rma.24.vmem" -printf "%T@ %p\n" 2>/dev/null | sort -rn | head -1 | cut -d" " -f2-)
[[ -n "${OTP}" ]] || { echo "run.sh: OTP image img_rma.24.vmem not built in ${EG2_ROOT}" >&2; exit 2; }
mkdir -p "${OUT}/tests"

# Keep the inventory identical to the Simple System runner. Directed tests remain listed while
# their UVM stimulus and, for CHERIoT tests, mode-specific start-up are not available here.
RVT="${IBEX}/vendor/riscv-tests/isa"
ARCH="${IBEX}/vendor/riscv-arch-tests/riscv-test-suite"
DIRECTED="${IBEX}/dv/uvm/core_ibex/directed_tests"
# The upstream physical environment writes its raw test number to tohost after an unexpected trap.
# On a host simulator that raw value stops the run; the chip's DV status word recognises only its
# documented terminal values. Make that one path report kTestStatusFailed, while env/riscv_test.h
# supplies the normal PASS and FAIL paths. This generated copy is kept in the build directory.
mkdir -p "${OUT}/env"
awk '{
  if ($0 ~ /sw TESTNUM, tohost, t5;/) {
    sub(/sw TESTNUM, tohost, t5;/,
        "li t5, 0xbaad; li t6, 0x411f0080; sw t5, 0(t6);")
  }
  print
}' "${IBEX}/vendor/riscv-test-env/p/riscv_test.h" > "${OUT}/env/riscv_test.h"
{
  for d in rv32ui rv32um rv32uc rv32mi; do
    for f in "${RVT}/${d}"/*.S; do echo "rvt.${d}.$(basename "${f}" .S) rvt ${f}"; done
  done
  for d in I M C B Zifencei privilege; do
    for f in "${ARCH}/rv32i_m/${d}/src"/*.S; do echo "arch.${d}.$(basename "${f}" .S) arch ${f}"; done
  done
  echo "rand.eg2.framework rand ${RAND}/tests/assembler_test.c"
  # Not listed here: the architecture tests of extensions Ibex does not implement (P, packed SIMD;
  # K, scalar crypto), and the Ibex directed tests, which need the Ibex UVM bench and run in the
  # Ibex UVM regression (ibex_cheriot_verification).
} | grep -F -- "${PATTERN}" | sort > "${OUT}/tests.txt"
nrun=$(awk '$2 != "not-run"' "${OUT}/tests.txt" | wc -l)
echo "== $(wc -l < "${OUT}/tests.txt") tests (${nrun} to run) on eg2sim, ${JOBS} at a time"

run_one() {
  local name=$1 kind=$2 src=$3 dir t0 rc wall out why last insns sim
  local -a flags sim_args
  dir="${OUT}/tests/${name}"
  mkdir -p "${dir}"
  rm -f "${dir}"/{test.elf,compile.log,run.log}
  flags=(-static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles
         -T "${HERE}/env/link.ld" -I "${HERE}/env" -I "${OUT}/env")
  case "${kind}" in
    rvt)  flags+=(-I "${IBEX}/vendor/riscv-test-env/p" -I "${IBEX}/vendor/riscv-test-env"
                  -I "${RVT}/macros/scalar" -I "${DIRECTED}") ;;
    # b: the toolchain (GCC 10.2, binutils 2.35) knows rol/ror only under the draft B name, not
    # under zbb (arch.B.rol-01 did not build).
    arch) flags+=(-march=rv32imcb_zba_zbb_zbc_zbs -I "${ARCH}/env")
          # The test's own settings for case 1, as in ../asm/run.sh (rvtest_mtrap_routine for the
          # tests that trap).
          while read -r def; do flags+=("-D${def}"); done < <(grep 'RVTEST_CASE' "${src}" |
            grep 'def TEST_CASE_1=True' | grep -o 'def [A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_]*' |
            sed 's/^def //' | sort -u) ;;
    rand) flags+=(-march=rv32imc -mabi=ilp32 -ffreestanding -I "${RAND}") ;;
  esac
  t0=${EPOCHREALTIME}
  local -a sources=("${HERE}/env/eg2_boot.S" "${src}")
  if [[ "${kind}" == rand ]]; then
    sources=("${HERE}/env/eg2_boot.S" "${HERE}/env/eg2_rand_start.S"
             "${RAND}/tests/assembler_trap.S"
             "${RAND}/eg2_rand.c" "${src}")
  fi
  if ! riscv32-unknown-elf-gcc "${flags[@]}" -o "${dir}/test.elf" \
       "${sources[@]}" > "${dir}/compile.log" 2>&1; then
    why=$(grep -m1 -i "error" "${dir}/compile.log" | sed 's/^.*: \(Error\|Fatal error\|error\): //')
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" no-build 0 - - "${why:-compiler failed}"
    return
  fi
  sim_args=(--rom "${dir}/test.elf" --otp "${OTP}")
  # eg2sim takes parameter symbols from its test image. This stand-alone ELF is both the reset
  # image and the test image, so load it in both roles when the framework exports parameters.
  if [[ "${kind}" == rand ]]; then
    sim_args+=(--test "${dir}/test.elf")
  fi
  (ulimit -v 4000000; timeout 600 env -u LD_LIBRARY_PATH -u LD_PRELOAD "${EG2SIM}" \
     "${sim_args[@]}" --max-insns "${MAX_INSNS}" --log-dir "${dir}" > "${dir}/run.log" 2>&1)
  rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b - a}')
  last=$(grep -a "after [0-9]* instructions" "${dir}/run.log" | tail -1)
  insns=$(sed -n 's/.* after \([0-9]*\) instructions.*/\1/p' <<< "${last}")
  sim=$(sed -n 's/.* instructions, \([0-9.]*\) us.*/\1/p' <<< "${last}")
  why=$(sed 's/^eg2sim: //; s/ after [0-9]* instructions.*//' <<< "${last}")
  case "${rc}" in
    0) out=pass ;;
    3) out=fail ;;
    2) out=model-stop ;;
    4) out=no-end ;;
    *) out=fail; why="eg2sim exited ${rc}${why:+: ${why}}" ;;
  esac
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "${name}" "${out}" "${wall}" "${insns:--}" "${sim:--}" "${why:-no final status}"
}
export -f run_one
export OUT HERE RAND IBEX RVT ARCH DIRECTED EG2SIM MAX_INSNS OTP
start=$(date +%s)
{
  awk '$2 != "not-run" {print $1, $2, $3}' "${OUT}/tests.txt" \
    | xargs -r -P "${JOBS}" -L1 bash -c 'run_one "$@"' _
  awk '$2 == "not-run" {n = $1; $1 = ""; $2 = ""; sub(/^ +/, ""); printf "%s\tnot-run\t0\t-\t-\t%s\n", n, $0}' \
    "${OUT}/tests.txt"
} | sort > "${OUT}/raw.txt"
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
  echo "Assembler suite on eg2sim, $(date -u +%FT%TZ), ${secs} s for $(wc -l < "${OUT}/results.txt") tests"
  awk '{n[$2]++} END {for (o in n) printf "  %-16s %d\n", o, n[o]}' "${OUT}/results.txt" | sort
  [[ -f "${EXPECTED}" ]] || touch "${EXPECTED}"
  join -a1 -e none -o 0,1.2,2.2 "${OUT}/results.txt" <(sort "${EXPECTED}") \
    | awk '$3 == "none" {print "  NEW      " $1 " " $2; next} $2 != $3 {print "  CHANGED  " $1 ": " $3 " -> " $2}'
  awk -F'\t' -v cats="${HERE}/categories.txt" '
    BEGIN {
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
      list[c] = list[c] sprintf("    %-10s %7.2f s %9s %10s  %-40s %s\n", $2, $3, $4, $5, $1, substr($6, 1, 160))
    }
    END {
      printf "\nBy category:\n  %-58s %5s %5s %5s %6s %10s %8s %7s %8s\n", "", "tests", "pass", "fail",
             "no-end", "model-stop", "no-build", "not-run", "time"
      for (k = 1; k <= nc; k++) if (total[o = order[k]])
        printf "  %-58s %5d %5d %5d %6d %10d %8d %7d %6.1f s\n", o, total[o], n[o, "pass"], n[o, "fail"],
               n[o, "no-end"], n[o, "model-stop"], n[o, "no-build"], n[o, "not-run"], wall[o]
      print "\nEvery test by category"
      printf "    %-10s %9s %9s %10s  %-40s %s\n", "outcome", "time", "insns", "sim. us", "test", "what happened"
      for (k = 1; k <= nc; k++) if (total[o = order[k]]) printf "\n  == %s\n%s", o, list[o]
    }' "${OUT}/raw.txt"
} | tee "${OUT}/report.txt"
grep -q "CHANGED" "${OUT}/report.txt" && exit 1
exit 0
