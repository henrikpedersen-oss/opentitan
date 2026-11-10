#!/usr/bin/env bash
# CHERIoT memory subsystem bench: Xcelium BUILD (elaborate only).
#
# Usage: ./cms_xlm_build.sh [-c] [-e] [-o <outdir>]
#   -c           instrument for coverage (cms_cover.ccf: the subsystem only)
#   -e           Earl Grey address map (192 KiB SRAM, 2 MiB NVM) instead of the small default
#   -o <outdir>  output directory, relative to this directory (default xlm_out)
#
# Runs xrun in the repository root (cms_xlm.f's paths are relative to it), in the EDA environment.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_TOP="$(realpath "${SCRIPT_DIR}/../../../../..")"
if ! command -v xrun >/dev/null 2>&1; then
  echo "$(basename "$0"): xrun is not on PATH; run it in the EDA environment" >&2
  exit 1
fi
export IBEX_ROOT_DIR="$(realpath "${SCRIPT_DIR}/../..")"
export dv_root="${IBEX_ROOT_DIR}/vendor/lowrisc_ip/dv"

OUT_DIR_NAME="xlm_out"
COVERAGE=0
MAP_ARG=""
while getopts "ceo:" opt; do
  case $opt in
    c) COVERAGE=1 ;;
    e) MAP_ARG="+define+CMS_EARLGREY_MAP" ;;
    o) OUT_DIR_NAME="$OPTARG" ;;
    *) echo "[cms_xlm_build] unknown option" >&2; exit 2 ;;
  esac
done
OUT_DIR="${SCRIPT_DIR}/${OUT_DIR_NAME}"

coverage_arg=""
if [[ ${COVERAGE} -eq 1 ]]; then
  coverage_arg="-coverage all -nowarn COVDEF -covfile ${SCRIPT_DIR}/cms_cover.ccf -covdut cheriot_mem_sys"
fi

mkdir -p "${OUT_DIR}"
# A stale incremental snapshot is a stale build.
rm -rf "${OUT_DIR}/xcelium.d"
rm -f "${OUT_DIR}/.elab_ok" "${OUT_DIR}/.coverage_enabled"

cd "${REPO_TOP}"
echo "[cms_xlm_build] Elaborating cms_tb (coverage=${COVERAGE}${MAP_ARG:+, Earl Grey map}) in ${OUT_DIR}"
xrun \
  -64bit \
  -sv \
  -f hw/ip/cheriot_mem_sys/dv/cms/cms_xlm.f \
  hw/ip/cheriot_mem_sys/dv/cms/cmodel/cms_dpi.cc \
  -CFLAGS "-std=c++17 -O2 -I${REPO_TOP}/hw/ip/cheriot_mem_sys/dv/cms/cmodel" \
  -top cms_tb \
  -l "${OUT_DIR}/build.log" \
  -xmlibdirname "${OUT_DIR}/xcelium.d" \
  -timescale 1ns/10ps \
  -licqueue \
  -elaborate \
  -access rwc \
  ${MAP_ARG} \
  ${coverage_arg}

touch "${OUT_DIR}/.elab_ok"
[[ ${COVERAGE} -eq 1 ]] && touch "${OUT_DIR}/.coverage_enabled"
echo "[cms_xlm_build] Build complete: ${OUT_DIR}"
