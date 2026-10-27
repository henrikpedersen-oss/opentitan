# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""Verilator console verdicts for the EG2 tests (`make regr-vlt`).

OpenTitan's defaults end a Verilator run at the first line containing "FAIL" or "FAULT" (failure)
or "PASS" (success). An EG2 test prints one line per check, and a check's text can contain those
words: "PBF ok entropy_fault: ALERT_FAIL_COUNTS counts ..." ended a passing run as failed
(regr-vlt 2026-10-25), and a line with "PASS" in it would end a run early as passed. The EG2
framework's verdict is its last line, `PASS!` or `FAIL!` (lib/testing/eg2/eg2_result.c); an
unexpected trap prints `CTRAP`/`RTRAP` (CHERIoT/RISC-V) or OTTF's `FAULT:`; a ROM failure prints
`BFV:`. Those are the only verdicts. A failing run then also reports every check, not only the
first.
"""

load("//rules/opentitan:defs.bzl", "verilator_params")

# The trap and fault patterns wait for the end of their line: the trap handlers print the cause, mtval
# and the PC after the word, and matching `CTRAP ` alone ended the run before them (2026-10-27,
# eg2_tag_roundtrip_cheriot: the log ended in "CTRAP ").
EG2_VERILATOR = verilator_params(
    exit_success = r"PASS!\r?\n",
    exit_failure = r"(FAIL!|FAULT: [^\n]*\n|[CR]TRAP [^\n]*\n|BFV:[0-9a-f]{8})",
)

# The same verdicts with Bazel's "eternal" timeout, which regr_vlt.sh maps to VLT_TIMEOUT_LONG:
# for tests whose work the software draws from the seed on Xcelium too, so it cannot be cut for
# Verilator alone, and which need more than VLT_TIMEOUT there (2026-10-26).
EG2_VERILATOR_LONG = verilator_params(
    timeout = "eternal",
    exit_success = r"PASS!\r?\n",
    exit_failure = r"(FAIL!|FAULT: [^\n]*\n|[CR]TRAP [^\n]*\n|BFV:[0-9a-f]{8})",
)
