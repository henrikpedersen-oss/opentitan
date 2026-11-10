#!/usr/bin/env bash
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

set -eu
test_dir=${1:?usage: run_core_tests.sh <test binary directory>}

for binary in "$test_dir"/*_test; do
  name=${binary##*/}
  "$binary" > "$binary.log" 2>&1 || {
    cat "$binary.log"
    exit 1
  }

  case "$name" in
    eg2_alert_dpi_test) fault=EG2_ALERT_TEST_BREAK ;;
    eg2_clkmgr_dpi_test) fault=EG2_CLK_TEST_BREAK ;;
    eg2_dm_dpi_test) fault=EG2_DM_TEST_BREAK ;;
    eg2_lc_ctrl_core_test) fault=EG2_LC_TEST_BREAK ;;
    eg2_otp_ctrl_core_test) fault=EG2_OTP_TEST_BREAK ;;
    eg2_pwrmgr_dpi_test) fault=EG2_PWR_TEST_BREAK ;;
    eg2_rram_ctrl_rdwr_dpi_test) fault=EG2_RRAM_RDWR_TEST_BREAK ;;
    eg2_rram_ctrl_top_dpi_test) fault=EG2_RRAM_TOP_TEST_BREAK ;;
    eg2_rram_dpi_test) fault=EG2_RRAM_TEST_BREAK ;;
    eg2_rram_macro_test) fault=EG2_RRAM_MACRO_TEST_BREAK ;;
    eg2_rram_phy_dpi_test) fault=EG2_RRAM_PHY_TEST_BREAK ;;
    eg2_rstmgr_dpi_test) fault=EG2_RST_TEST_BREAK ;;
    eg2_sram_ctrl_core_test) fault=EG2_SRAM_TEST_BREAK ;;
    eg2_rram_lcmgr_dpi_test|eg2_rram_otp_dpi_test) fault= ;;
    *) echo "No negative test registered for $name" >&2; exit 1 ;;
  esac

  if [[ $name == eg2_rram_lcmgr_dpi_test ]]; then
    command=("${binary}_fail")
  elif [[ $name == eg2_rram_otp_dpi_test ]]; then
    command=("$binary" --inject)
  else
    command=(env "$fault=1" "$binary")
  fi
  result=0
  "${command[@]}" > "$binary.negative.log" 2>&1 || result=$?
  if [[ $result != 1 ]]; then
    echo "FAIL: $name negative test exited $result, expected 1" >&2
    cat "$binary.negative.log"
    exit 1
  fi
  printf 'PASS %s (normal and negative checks)\n' "$name"
done
