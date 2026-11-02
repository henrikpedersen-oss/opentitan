#!/usr/bin/env bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Writes variants.txt: the chip tests that run a test image with settings of their own, each run on
# the model as a test of its own. On the chip bench one image often serves several tests that set
# it up differently: a test parameter pinned (+eg2_param_<name>=<value>, which the bench writes into
# the image's kEg2Param_<name>), another life-cycle state's OTP image (+use_otp_image), or another
# peripheral instance (+uart_idx, +i2c_idx). The suite runs each image once with its built-in
# values; this file adds one run per distinct setting.
#
#   gen_variants.sh      ->  variants.txt (review the diff before committing it)
#
# Reads the EG2 fork (EG2_ROOT): hw/top_earlgrey/dv/*.hjson. One line per run:
#   <image>@<chip test> <eg2sim arguments>
# <chip test> is the shortest name of the bench tests with these settings, without its chip_sw_ or
# chip_ prefix. otp=<file under bazel-out's bin/> is an argument for run.sh: the OTP image to load.
#
# Only tests that start from the test ROM with one test image are taken. A bench test that checks
# the bench itself is left out: a mutation (__mut__, +eg2_mutate), a corrupted or flipped
# scoreboard item (+sb_corrupt, +eg2_sb_flip, +eg2_model_corrupt, +eg2_tl_err_flip), a bench
# self-test (_bench_fail, +eg2_dm_bench_fail, +eg2_cap_halves_selftest). The settings taken, and
# what the model is given for them:
#   +eg2_param_<n>=<v>               --param <n>=<v> (as chip_sw_eg2_base_vseq's set_param)
#   +use_otp_image=OtpTypeLcSt<S>    otp=hw/top_earlgrey/data/otp/img_<s>.24.vmem, the life-cycle
#                                    state's image (OtpTypeCustom is the test's own image: otp.txt)
#   +uart_idx=<n>                    --sym kUartIdxDv=<n>:1 (chip_sw_uart_tx_rx_vseq only; the UART
#                                    stimulus for that instance is the run's bench.txt line)
#   +i2c_idx=<n>                     --sym kI2cIdx=<n>:1 (chip_sw_i2c_host_tx_rx_vseq only)
# Life-cycle settings are stimuli because the chip bench applies them outside the CPU:
#   +dest_dec_state                  --stim lc_ctrl.sw_destination=<state>@0us
#   +src_dec_state                   --stim lc_ctrl.otp_state=<state>@0us; a RAW or TEST_LOCKED
#                                    source also gets the JTAG SCRAP request software cannot issue
#   +lc_at_prod                      --stim lc_ctrl.otp_state=17@0us
#   +otp_clear_secret2               --stim lc_ctrl.clear_secret2=1@0us

set -euo pipefail
export LC_ALL=C
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"   # EG2_ROOT
DV="${EG2_ROOT}/hw/top_earlgrey/dv"
OUT="${HERE}/variants.txt"

# Every test entry: its text from a line opening with "{" to the line closing it with "}" (or one
# line holding both, as chip_eg2_cheriot_tests.hjson writes some). Prints
# <test> <image> <arguments...> for each entry with settings the model takes.
awk '
  # OtpTypeLcStTestUnlocked0 -> test_unlocked0, as the Bazel images are named (img_<state>).
  function lc_state(s) {
    if (!sub(/^OtpTypeLcSt/, "", s)) return ""
    if (s ~ /^TestUnlocked[0-9]$/) return "test_unlocked" substr(s, 13)
    if (s ~ /^TestLocked[0-9]$/) return "test_locked" substr(s, 11)
    if (s == "ProdEnd") return "prod_end"
    if (s ~ /^(Raw|Dev|Prod|Rma)$/) return tolower(s)
    return ""
  }
  function dec_state(s) {
    sub(/^DecLcSt/, "", s)
    if (s == "Raw") return 0
    if (s ~ /^TestUnlocked[0-7]$/) return 1 + 2 * substr(s, 13)
    if (s ~ /^TestUnlock[0-7]$/) return 1 + 2 * substr(s, 11)
    if (s ~ /^TestLocked[0-6]$/) return 2 + 2 * substr(s, 11)
    if (s == "Dev") return 16
    if (s == "Prod") return 17
    if (s == "ProdEnd") return 18
    if (s == "Rma") return 19
    if (s == "Scrap") return 20
    return -1
  }
  function field(b, key,    v) {
    if (!match(b, key ":[ \t]*\"?[A-Za-z0-9_]+")) return ""
    v = substr(b, RSTART, RLENGTH); sub(/^[^:]*:[ \t]*"?/, "", v)
    return v
  }
  function entry(b,    name, seq, img, nimg, skip, nopt, opt, s, o, k, v, st, i, line) {
    name = field(b, "(^|[{ \t\n])name"); seq = field(b, "uvm_test_seq")
    if (name == "" || b !~ /sw_test_mode_test_rom/ || name ~ /__mut__|_bench_fail$/) return
    s = b; nimg = 0
    while (match(s, /"\/\/(sw\/device|third_party|hw\/top_earlgrey\/sw\/autogen)\/[^"]*:[a-zA-Z0-9_]+:1[":]/)) {
      img = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
      sub(/:1[":]$/, "", img); sub(/.*:/, "", img); nimg++
    }
    if (nimg != 1) return
    s = b; nopt = 0
    while (match(s, /\+[a-zA-Z0-9_]+(=[^",\] \t\n]*)?/)) {
      o = substr(s, RSTART + 1, RLENGTH - 1); s = substr(s, RSTART + RLENGTH)
      k = o; v = ""
      if (index(o, "=")) { k = substr(o, 1, index(o, "=") - 1); v = substr(o, index(o, "=") + 1) }
      if (k ~ /^(eg2_mutate|eg2_mutate_targets|sb_corrupt|eg2_sb_flip|eg2_model_corrupt|eg2_tl_err_flip|eg2_dm_bench_fail|eg2_cap_halves_selftest)$/) return
      gsub(/_/, "", v)  # numbers written 300_000_000
      if (k ~ /^eg2_param_/) {
        opt[++nopt] = "--param " substr(k, 11) "=" v
      } else if (k == "use_otp_image" && (st = lc_state(v)) != "") {
        if (seq == "chip_sw_lc_volatile_raw_unlock_vseq") {
          opt[++nopt] = "--stim lc_ctrl.otp_state=0@0us"
          opt[++nopt] = "--stim lc_ctrl.otp_count=0@0us"
        } else if (seq == "chip_sw_csrng_lc_hw_debug_en_vseq") {
          opt[++nopt] = "--stim lc_ctrl.otp_state=1@0us"
        } else if (seq !~ /^chip_sw_lc_walkthrough/) {
          opt[++nopt] = "otp=hw/top_earlgrey/data/otp/img_" st ".24.vmem"
        }
      } else if (k == "uart_idx" && seq == "chip_sw_uart_tx_rx_vseq") {
        opt[++nopt] = "--sym kUartIdxDv=" v ":1"
      } else if (k == "i2c_idx" && seq == "chip_sw_i2c_host_tx_rx_vseq") {
        opt[++nopt] = "--sym kI2cIdx=" v ":1"
      } else if (k == "dest_dec_state" && dec_state(v) >= 0) {
        opt[++nopt] = "--stim lc_ctrl.sw_destination=" dec_state(v) "@1ns"
      } else if (k == "src_dec_state" && dec_state(v) >= 0) {
        st = dec_state(v)
        opt[++nopt] = "--stim lc_ctrl.otp_state=" st "@0us"
        if (seq == "chip_sw_lc_ctrl_scrap_vseq" &&
            (st == 0 || (st >= 2 && st <= 14 && st % 2 == 0))) {
          opt[++nopt] = "--stim lc_ctrl.jtag_transition=20@1us"
          opt[++nopt] = "--stim sym.kPerformTransitionBySW=0@1ns"
        }
      } else if (k == "lc_at_prod") {
        opt[++nopt] = "--stim lc_ctrl.otp_state=17@0us"
      } else if (k == "otp_clear_secret2" && v != "0") {
        opt[++nopt] = "--stim lc_ctrl.clear_secret2=1@0us"
      }
    }
    line = ""
    for (i = 1; i <= nopt; i++) line = line " " opt[i]
    if (line != "") print name, img line
  }
  /^[ \t]*\{.*\}[ \t]*,?[ \t]*$/ { entry($0); next }
  /^[ \t]*\{[ \t]*$/ { buf = ""; inside = 1; next }
  inside && /^[ \t]*\}[ \t]*,?[ \t]*$/ { inside = 0; entry(buf); next }
  inside { buf = buf "\n" $0 }
  ' "${DV}"/*.hjson > "${OUT}.raw"

# One run per image and distinct set of arguments (the arguments sorted, so the order on the bench
# command line does not matter), named after the shortest test name with that set.
awk '{
    t = $1; img = $2; $1 = $2 = ""; sub(/^ +/, "")
    n = split($0, w, " "); args = ""
    # Pair the words back into arguments (--param x=1, --sym s=v, otp=...), then sort them.
    m = 0
    for (i = 1; i <= n; i++) {
      if (w[i] ~ /^--/) { a[++m] = w[i] " " w[i + 1]; i++ } else a[++m] = w[i]
    }
    for (i = 2; i <= m; i++) { x = a[i]; for (j = i - 1; j >= 1 && a[j] > x; j--) a[j + 1] = a[j]; a[j + 1] = x }
    for (i = 1; i <= m; i++) args = args " " a[i]
    key = img args
    short = t; sub(/^chip_sw_/, "", short); sub(/^chip_/, "", short)
    if (!(key in best) || length(short) < length(best[key]) ||
        (length(short) == length(best[key]) && short < best[key])) best[key] = short
    image[key] = img; rest[key] = args
  }
  END { for (k in best) print image[k] "@" best[k] rest[k] }' "${OUT}.raw" | sort > "${OUT}.body"

{
  echo "# Generated by gen_variants.sh from the EG2 fork's test lists: the chip tests that run a test"
  echo "# image with settings of their own (parameters, life-cycle state, peripheral instance), each a"
  echo "# run of its own on the model. <image>@<chip test> <eg2sim arguments>; otp=<file> is the OTP"
  echo "# image run.sh loads."
  awk '{
    if ($1 ~ /^eg2_nist_sha[23](_cheriot)?@eg2_nist_sha[23](_cheriot)?_monte$/)
      $0 = $0 " --param boundary_vectors=0 --param random_vectors=256"
    print
  }' "${OUT}.body"
  # Every vector embedded in each non-OTBN image. These are separate from the regression's
  # boundary/random budget and from the weekly Monte Carlo settings above.
  for spec in \
    aes:512 aes_gcm:256 sha2:256 hmac:256 sha3:256 drbg:64 aes_kw:512; do
    image="eg2_nist_${spec%%:*}"; random="${spec#*:}"
    echo "${image}@full-vector-budget --param boundary_vectors=0 --param random_vectors=${random}"
    echo "${image}_cheriot@full-vector-budget --param boundary_vectors=0 --param random_vectors=${random}"
  done
} > "${OUT}"
rm -f "${OUT}.raw" "${OUT}.body"
echo "== $(grep -vc '^#' "${OUT}") runs of chip-test settings, in ${OUT}"
