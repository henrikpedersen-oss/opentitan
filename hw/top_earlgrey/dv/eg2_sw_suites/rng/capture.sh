#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# The SystemC model's capture runs for the statistical assessment (EG2 spec "Random Number
# Generation Test Suite" > "Taps and assessment": the full SP 800-90B and SP 800-22 volumes come
# from eg2sim; `make sw-suite-rng` runs this, then rng_assess.py on the output).
#
# Each image (default: chip_sw_eg2_rng_capture in RISC-V and CHERIoT mode, and their
# _inject_fail builds, which must make SP 800-22 fail) runs in eg2sim with the noise source fed
# from the host (--rng host), every tap and mode, and capture_words words per tap: the default
# 3,200,000 gives 102.4 Mbit per stream, 100 SP 800-22 sequences of 10^6 bits and Dieharder's
# 10^8 bits. One directory per image under -o, like `make model-tests`. The biased-pattern run is
# the SP 800-90B must-fail check from the specification.
#
# Usage: capture.sh [-o outdir] [-j jobs] [-w words] [image ...]
#   jobs: eg2sim runs at once (default 2, the laptop's limit). Exit 1 if a run did not do what it
#   must (plain: exit 0; _inject_fail: exit 3, the test's own checks).

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../env.sh"   # EG2_ROOT, EG2SIM, SW_SUITES_OUT
OUT="${SW_SUITES_OUT}/rng/captures"
JOBS=2
WORDS=3200000
while getopts "o:j:w:" opt; do
  case $opt in
    o) OUT=$OPTARG ;;
    j) JOBS=$OPTARG ;;
    w) WORDS=$OPTARG ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
RUNS=("$@")
[ ${#RUNS[@]} -eq 0 ] && RUNS=(eg2_rng_capture eg2_rng_capture_cheriot eg2_rng_capture_inject_fail
                               eg2_rng_capture_cheriot_inject_fail eg2_rng_capture_pattern_biased)

first() { find -L "${EG2_ROOT}/bazel-out/" -path "$1" -not -path '*runfiles*' -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-; }
ROM=$(first '*bin/sw/device/lib/testing/test_rom/test_rom_sim_dv.elf')
OTP=$(first '*bin/hw/top_earlgrey/data/otp/img_rma.24.vmem')
[ -x "$EG2SIM" ] || { echo "rng_model_capture: no $EG2SIM (make eg2sim)"; exit 2; }
[ -n "$ROM" ] && [ -n "$OTP" ] || { echo "rng_model_capture: test ROM or OTP image not built"; exit 2; }

run_one() {
  local name=$1 image=$1 source=host img dir rc verdict
  if [[ ${name} == *_pattern_biased ]]; then
    image=${name%_pattern_biased}
    # EG2 specification: bias above 77% has less than the claimed 0.375 bits/bit min-entropy.
    source=pattern:biased:80
  fi
  img=$(first "*bin/sw/device/tests/eg2_rng/${image}_sim_dv.elf")
  [ -n "$img" ] || { echo "FAIL $name (no image: build //sw/device/tests/eg2_rng:${image})"; return; }
  dir=$OUT/$name
  mkdir -p "$dir"
  # No instruction limit to speak of: a capture of 3.2 M words per tap runs billions of
  # instructions. The model's own library path, as in model_tests.sh (no inherited LD_*).
  (ulimit -v 4000000; env -u LD_LIBRARY_PATH -u LD_PRELOAD "$EG2SIM" --rom "$ROM" --test "$img" --otp "$OTP" \
    --rng "$source" --param capture_words="$WORDS" --param model_gaps=0 --max-insns 1000000000000 \
    --log-dir "$dir" > "$dir/run.log" 2>&1)
  rc=$?
  if [[ $name == *_inject_fail* ]]; then
    [ $rc -eq 3 ] && verdict="PASS (failed as it must)" || verdict="FAIL (must fail, rc $rc)"
  elif [[ $name == *_pattern_biased ]]; then
    # This deliberately defective source can also trip the capture program's consecutive-word
    # check. Accept that software failure only after all nine applicable streams were completed
    # and only for that check; SP 800-90B still independently has to report CAUGHT.
    streams=$(grep -a -c 'RNG capture .*: [0-9][0-9]* words logged' "$dir/run.log")
    if [[ $rc -eq 0 ]]; then
      verdict="PASS"
    elif [[ $rc -eq 3 && $streams -eq 9 ]] &&
         grep -a -q 'RNG FAIL rng_capture: no two consecutive captured words equal' "$dir/run.log"; then
      verdict="PASS (biased source caught by software after all streams)"
    else
      verdict="FAIL (biased must-fail capture incomplete, rc $rc, $streams streams)"
    fi
  else
    [ $rc -eq 0 ] && verdict="PASS" || verdict="FAIL (rc $rc: $(grep -a -m1 'RNG FAIL\|test failed\|limit' "$dir/run.log" | cut -c1-120))"
  fi
  echo "$verdict $name"
}
export -f run_one first
export EG2_ROOT EG2SIM ROM OTP OUT WORDS
mkdir -p "$OUT"
printf '%s\n' "${RUNS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {} | tee "$OUT/summary.txt"
bad=$(grep -c '^FAIL' "$OUT/summary.txt")
echo "rng_model_capture: ${#RUNS[@]} runs, $bad not as they must be (logs in $OUT/)"
[ "$bad" -eq 0 ]
