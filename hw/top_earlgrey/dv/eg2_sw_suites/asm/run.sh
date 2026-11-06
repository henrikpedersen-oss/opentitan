#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# The assembler software suite (doc/sw_test_suites.md, section 1): riscv-tests and the RISC-V
# architecture tests on Ibex's Simple System with Spike co-simulation (Verilator, no licence), each
# result compared with expected.txt. make sw-suite-asm.
#
# The tests are the Ibex submodule's own copies (IBEX, default ibex_cheriot_verification/ibex: the CHERIoT Ibex EG2 vendors): riscv-tests
# vendor/riscv-tests/isa/rv32{ui,um,uc,mi}, the architecture tests
# vendor/riscv-arch-tests/riscv-test-suite/rv32i_m/*, and the Ibex directed tests
# dv/uvm/core_ibex/directed_tests (listed only: they need the UVM bench). Each is built with the
# environment in env/ (Simple System's memory map; the verdict on its console) and run once.
# Each test ends in one of five outcomes:
#   pass      the test printed PASS, the simulator ended cleanly, and Spike matched every
#             instruction (no FAILURE in the log, at least one instruction matched)
#   fail      the test printed FAIL, Spike found a mismatch, or the run stopped without a verdict
#             (the environment's "unexpected trap" path writes tohost, which stops the run)
#   no-end    no verdict within the cycle limit
#   no-build  the test does not build with the Nix shell's compiler (GCC 10.2: no Zbk/Zk)
#   not-run   not run on this runner, with the reason (README.md)
# expected.txt holds "<test> <outcome>" per test. The suite fails when a test's outcome differs
# from its line; a test with no line is reported as new. --update rewrites expected.txt from this
# run (review the diff before committing it).
#
#   run.sh [--update] [--rebuild] [-j JOBS] [-c CONFIG] [PATTERN]
#     --rebuild  rebuild the simulator (it is otherwise built once and kept)
#     CONFIG     an Ibex configuration (${IBEX_TESTS}/ibex_configs.yaml), default maxperf-pmp-bmfull
#                (RV32IMCB with Zbkb/Zbkx, PMP: what the tests need)
#     PATTERN    run only the tests whose name contains it
#
# Run in the Ibex submodule's Nix shell (FuseSoC, Verilator, the RISC-V compiler, Spike):
# make sw-suite-asm does that. Output: $SW_SUITES_OUT/asm/ (eg2_top_verification: scratch/sw-suites/asm/) (a directory per test under tests/:
# compile.log, test.elf, sim.log, ibex_simple_system.log; results.txt, report.txt).

set -uo pipefail
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, SW_SUITES_OUT, IBEX_CV
IBEX="${IBEX:-${IBEX_CV}/ibex}"
OUT="${SW_SUITES_OUT}/asm"
RAND="${EG2_ROOT}/sw/device/lib/testing/eg2_rand"
EXPECTED="${HERE}/expected.txt"
UPDATE=0
REBUILD=0
JOBS=4
CONFIG=maxperf-pmp-bmfull
CYCLES=2000000
while [[ $# -gt 0 ]]; do
  case "$1" in
    --update) UPDATE=1; shift ;;
    --rebuild) REBUILD=1; shift ;;
    -j) JOBS="$2"; shift 2 ;;
    -c) CONFIG="$2"; shift 2 ;;
    *) break ;;
  esac
done
PATTERN="${1:-}"

for t in fusesoc verilator riscv32-unknown-elf-gcc pkg-config; do
  command -v "$t" >/dev/null || { echo "run.sh: $t is not on PATH (make sw-suite-asm runs it in the Ibex Nix shell, ibex#shell)" >&2; exit 2; }
done
mkdir -p "${OUT}/tests"

# The simulator: Simple System with co-simulation, built once per configuration.
SIMDIR="${OUT}/sim-${CONFIG}"
SIM=$(find "${SIMDIR}/fusesoc" -type f -name Vibex_simple_system -perm -u+x 2>/dev/null | head -1)
if [[ ${REBUILD} -eq 1 || -z "${SIM}" ]]; then
  echo "== Building Simple System with co-simulation, configuration ${CONFIG} (Verilator; several minutes)"
  mkdir -p "${SIMDIR}"
  OPTS=$(cd "${IBEX}" && ./util/ibex_config.py "${CONFIG}" fusesoc_opts) || exit 2
  # shellcheck disable=SC2086  # OPTS is a list of fusesoc options
  (cd "${IBEX}" && fusesoc --cores-root=. run --target=sim --setup --build --build-root "${SIMDIR}/fusesoc" \
     lowrisc:ibex:ibex_simple_system_cosim ${OPTS}) > "${SIMDIR}/build.log" 2>&1 \
    || { echo "run.sh: building Simple System failed, see ${SIMDIR}/build.log" >&2; exit 2; }
  SIM=$(find "${SIMDIR}/fusesoc" -type f -name Vibex_simple_system -perm -u+x | head -1)
  [[ -n "${SIM}" ]] || { echo "run.sh: no Vibex_simple_system in ${SIMDIR}/fusesoc" >&2; exit 2; }
fi

# The tests: "<name> <kind> <source>"; kind rvt (riscv-tests), arch (architecture tests), or
# "not-run <reason>" for the tests this runner does not run.
RVT="${IBEX_TESTS}/vendor/riscv-tests/isa"
ARCH="${IBEX_TESTS}/vendor/riscv-arch-tests/riscv-test-suite"
DIRECTED="${IBEX_TESTS}/dv/uvm/core_ibex/directed_tests"
{
  for d in rv32ui rv32um rv32uc rv32mi; do
    for f in "${RVT}/${d}"/*.S; do
      n="rvt.${d}.$(basename "${f}" .S)"
      case "${n}" in
        # It expects a hardware trigger to raise a breakpoint exception (tdata1.action 0); Ibex's
        # triggers only enter debug mode (action is read-only 1, ibex doc cs_registers.rst tdata1),
        # and Simple System has no debug module to enter.
        rvt.rv32mi.breakpoint) echo "${n} not-run Ibex triggers only enter debug mode (tdata1.action is 1, cs_registers.rst); the test expects a breakpoint exception" ;;
        *) echo "${n} rvt ${f}" ;;
      esac
    done
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
echo "== $(wc -l < "${OUT}/tests.txt") tests (${nrun} to run) on Simple System, configuration ${CONFIG}, ${JOBS} at a time"

run_one() {
  local name=$1 kind=$2 src=$3 dir t0 rc wall cyc matched why out
  local -a flags
  dir="${OUT}/tests/${name}"
  mkdir -p "${dir}"
  rm -f "${dir}"/{test.elf,sim.log,ibex_simple_system.log,compile.log}
  flags=(-static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles -T "${HERE}/env/link.ld"
         -I "${HERE}/env")
  case "${kind}" in
    rvt)  flags+=(-I "${IBEX_TESTS}/vendor/riscv-test-env/p" -I "${IBEX_TESTS}/vendor/riscv-test-env"
                  -I "${RVT}/macros/scalar" -I "${DIRECTED}") ;;
    # b: the toolchain (GCC 10.2, binutils 2.35) knows rol/ror only under the draft B name, not
    # under zbb (arch.B.rol-01 did not build).
    arch) flags+=(-march=rv32imcb_zba_zbb_zbc_zbs -I "${ARCH}/env")
          # The test's own settings for case 1, from its RVTEST_CASE lines ("def NAME=VALUE"), as
          # riscof passes them: TEST_CASE_1, and rvtest_mtrap_routine for the tests that trap.
          # Without it no trap handler was built: ebreak and cebreak-01 hung, and ecall defined
          # mtrap_sigptr twice (arch_test.h defines it unless rvtest_mtrap_routine is set).
          while read -r def; do flags+=("-D${def}"); done < <(grep 'RVTEST_CASE' "${src}" |
            grep 'def TEST_CASE_1=True' | grep -o 'def [A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_]*' |
            sed 's/^def //' | sort -u) ;;
    rand) flags+=(-march=rv32imc -mabi=ilp32 -ffreestanding -I "${RAND}") ;;
  esac
  t0=${EPOCHREALTIME}
  local -a sources=("${src}")
  if [[ "${kind}" == rand ]]; then
    sources=("${HERE}/env/eg2_rand_start.S" "${RAND}/tests/assembler_trap.S"
             "${RAND}/eg2_rand.c" "${src}")
  fi
  if ! riscv32-unknown-elf-gcc "${flags[@]}" -o "${dir}/test.elf" "${sources[@]}" > "${dir}/compile.log" 2>&1; then
    why=$(grep -m1 -i "error" "${dir}/compile.log" | sed 's/^.*: \(Error\|Fatal error\|error\): //')
    printf '%s\t%s\t%s\t%s\t%s\n' "${name}" no-build 0 - "${why:-compiler failed}"
    return
  fi
  (cd "${dir}" && timeout 600 "${SIM}" --meminit=ram,"${dir}/test.elf" -c "${CYCLES}" > sim.log 2>&1)
  rc=$?
  wall=$(awk -v a="${t0}" -v b="${EPOCHREALTIME}" 'BEGIN {printf "%.2f", b - a}')
  matched=$(sed -n 's/.*Co-simulation matched \([0-9]*\) instructions.*/\1/p' "${dir}/sim.log" | tail -1)
  cyc=$(grep -m1 -o "Simulation timeout of [0-9]* cycles" "${dir}/sim.log")
  touch "${dir}/ibex_simple_system.log"
  if grep -q "FAILURE" "${dir}/sim.log"; then
    out=fail; why="co-simulation: $(grep -A1 -m1 "FAILURE" "${dir}/sim.log" | tail -1 | cut -c1-140)"
  elif grep -qx "FAIL" "${dir}/ibex_simple_system.log"; then
    out=fail; why="the test printed FAIL"
  elif [[ -n "${cyc}" ]]; then
    out=no-end; why="no verdict: ${cyc}"
  elif ! grep -qx "PASS" "${dir}/ibex_simple_system.log"; then
    out=fail; why="the run stopped without a verdict (unexpected trap or ecall: tohost written), exit ${rc}"
  elif [[ ${rc} -ne 0 ]]; then
    out=fail; why="PASS, but the simulator exited ${rc}"
  elif [[ "${matched:-0}" -eq 0 ]]; then
    out=fail; why="PASS, but co-simulation matched no instructions"
  else
    out=pass; why="PASS; Spike matched ${matched} instructions"
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "${name}" "${out}" "${wall}" "${matched:--}" "${why}"
}
export -f run_one
export OUT HERE RAND IBEX RVT ARCH DIRECTED SIM CYCLES
start=$(date +%s)
# raw.txt: test, outcome, wall seconds, instructions Spike matched, what happened (tab-separated).
{
  awk '$2 != "not-run" {print $1, $2, $3}' "${OUT}/tests.txt" \
    | xargs -P "${JOBS}" -L1 bash -c 'run_one "$@"' _
  awk '$2 == "not-run" {n = $1; $1 = ""; $2 = ""; sub(/^ +/, ""); printf "%s\tnot-run\t0\t-\t%s\n", n, $0}' \
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

# The report: outcomes and every difference from expected.txt; each category (categories.txt) with
# its counts and time; then every test under its category: outcome, time, instructions Spike
# matched, and what happened.
{
  echo "Assembler suite on Simple System with Spike co-simulation (configuration ${CONFIG})," \
       "$(date -u +%FT%TZ), ${secs} s for $(wc -l < "${OUT}/results.txt") tests"
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
      list[c] = list[c] sprintf("    %-9s %7.2f s %9s  %-40s %s\n", $2, $3, $4, $1, substr($5, 1, 160))
    }
    END {
      printf "\nBy category:\n  %-58s %5s %5s %5s %6s %8s %7s %8s\n", "", "tests", "pass", "fail",
             "no-end", "no-build", "not-run", "time"
      for (k = 1; k <= nc; k++) if (total[o = order[k]])
        printf "  %-58s %5d %5d %5d %6d %8d %7d %6.1f s\n", o, total[o], n[o, "pass"], n[o, "fail"],
               n[o, "no-end"], n[o, "no-build"], n[o, "not-run"], wall[o]
      print "\nEvery test by category"
      printf "    %-9s %9s %9s  %-40s %s\n", "outcome", "time", "insns", "test", "what happened"
      for (k = 1; k <= nc; k++) if (total[o = order[k]]) printf "\n  == %s\n%s", o, list[o]
    }' "${OUT}/raw.txt"
} | tee "${OUT}/report.txt"
grep -q "CHANGED" "${OUT}/report.txt" && exit 1
exit 0
