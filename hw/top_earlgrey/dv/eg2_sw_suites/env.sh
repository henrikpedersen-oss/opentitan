# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# What every software suite here needs from its surroundings, sourced by each runner. The
# defaults are eg2_top_verification's layout, with this tree as its eg2/; its make sw-suite-*
# targets set them:
#   EG2_ROOT        this OpenTitan tree (the chip test images in its bazel-out/, the sources)
#   EG2_SCRATCH     eg2_top_verification's scratch/: what its make targets build
#   EG2SIM          the model, built from hw/top_earlgrey/dv/eg2_model (lowRISC/ot-model):
#                   eg2_top_verification's make eg2sim
#   SW_SUITES_OUT   where each suite writes its runs and report, one directory per suite
SUITES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EG2_ROOT="${EG2_ROOT:-$(cd "${SUITES_DIR}/../../../.." && pwd)}"
EG2_SCRATCH="${EG2_SCRATCH:-$(cd "${EG2_ROOT}/.." && pwd)/scratch}"
EG2SIM="${EG2SIM:-${EG2_SCRATCH}/eg2sim/eg2sim}"
SW_SUITES_OUT="${SW_SUITES_OUT:-${EG2_SCRATCH}/sw-suites}"
#   IBEX_CV         the ibex_cheriot_verification checkout beside eg2_top_verification: its ibex/
#                   (riscv-tests, the architecture tests, the RTOS firmware sources) and its builds
IBEX_CV="${IBEX_CV:-$(cd "${EG2_ROOT}/../.." && pwd)/ibex_cheriot_verification}"
# The Ibex test sources the assembler suites build (riscv-tests, the architecture tests, the Ibex
# directed tests): eg2_top_verification's plain copies (cheriot-assembler-test-suite/README.md).
IBEX_TESTS="${IBEX_TESTS:-$(cd "${EG2_ROOT}/.." && pwd)/cheriot-assembler-test-suite}"
