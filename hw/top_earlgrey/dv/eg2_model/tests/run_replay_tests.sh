#!/usr/bin/env bash
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

set -eu
replay=${1:?usage: run_replay_tests.sh <replay binary> <output directory>}
out=${2:?output directory required}
mkdir -p "$out"
cd "$out"
# The loader only needs the ELF32 identification and an empty program table.
printf '\177ELF\001' > empty.elf
dd if=/dev/zero bs=1 count=47 status=none >> empty.elf
: > empty.trace
printf 'core   0: 3 0x00040000 (0x00000013)\n' > no-loads.trace
# Read UART0.INTR_ENABLE at its reset value. This is an independent comparison.
printf 'core   0: 3 0x00040000 (0x00002083) x1 0x00000000 mem 0x40000004\n' > good.trace
printf 'core   0: 3 0x00040000 (0x00002083) x1 0x00000001 mem 0x40000004\n' > bad.trace
cp good.trace bus-error.trace
printf 'core   0: 3 0x00040004 (0x00102023) mem 0xdead0000 0x12345678\n' >> bus-error.trace

run_case() {
  local name=$1 expected=$2 trace=$3 result=0
  shift 3
  "$replay" --rom empty.elf --trace "$trace" "$@" > "$name.log" 2>&1 || result=$?
  if [[ $result != "$expected" ]]; then
    echo "FAIL: $name exited $result, expected $expected" >&2
    cat "$name.log"
    exit 1
  fi
  printf 'PASS replay %s\n' "$name"
}

run_case known-register 0 good.trace
run_case corrupted-value 1 bad.trace
run_case bus-error 1 bus-error.trace
run_case empty-trace 1 empty.trace
run_case no-comparisons 1 no-loads.trace
run_case missing-expected-output 1 good.trace --expect uart0=does-not-exist.log
