# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The RRAM controller tests of Earl Grey 2 (eg2_top_verification spec REQ_RRAM, "RRAM controller in
# RISC-V mode", the RRAM Controller test table; hw/ip/rram_ctrl's documentation). One source per
# test, built for RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail`
# (-D<TEST>_INJECT_FAIL and -DEG2_TEST_INJECT_FAIL) breaks the first expected value and must fail
# with `RRAM FAIL` (REQ_STE_03). Each runs in eg2sim before the chip (REQ_STE_11); what the model
# does not model is skipped there behind the `model_gaps` knob, listed in each source.
#
# chip_rram_jtag_lc_gate has no software of its own: its sequence runs on the stub CPU
# (chip_eg2_rram_tests.hjson).

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "eg2_rram",
    srcs = ["eg2_rram.c"],
    hdrs = ["eg2_rram.h"],
    deps = [
        "//hw/top:rram_ctrl_c_regs",
        "//hw/top:top_lib",
        "//sw/device/lib/testing/eg2:test",
        "//sw/device/tests/eg2_mem:eg2_chk",
    ],
)

_COPTS = select({
    "//toolchain:isa_cheriot": ["-fno-jump-tables"],
    "//conditions:default": [],
})

_ENVS = {
    "//hw/top_earlgrey:sim_dv": None,
    "//hw/top_earlgrey:sim_verilator": None,
}

_DEPS = [
    ":eg2_rram",
    "//hw/top:alert_handler_c_regs",
    "//hw/top:aon_timer_c_regs",
    "//hw/top:clkmgr_c_regs",
    "//hw/top:lc_ctrl_c_regs",
    "//hw/top:otp_ctrl_c_regs",
    "//hw/top:pwrmgr_c_regs",
    "//hw/top:rram_ctrl_c_regs",
    "//hw/top:rram_macro_c_regs",
    "//hw/top:rstmgr_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:rv_plic_c_regs",
    "//hw/top:sram_ctrl_c_regs",
    "//hw/top:top_lib",
    "//sw/device/lib/base:memory",
    "//sw/device/lib/base:multibits",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/tests/eg2_mem:eg2_chk",
]

# Sources (eg2_rram_<x>.c is chip_sw_rram_<x>); every test runs in both modes.
_TESTS = [
    # Host read path (REQ_RRAM_01-06).
    "eg2_rram_host_exec",
    "eg2_rram_exec_en",
    "eg2_rram_host_wr_reject",
    "eg2_rram_fetch_during_write",
    "eg2_rram_write_then_fetch",
    "eg2_rram_fetch_perf",
    # Scrambling, ECC and integrity (REQ_RRAM_07, 09, 10, 12).
    "eg2_rram_scramble",
    "eg2_rram_ecc_corr_rewrite",
    "eg2_rram_uncorr_fetch",
    "eg2_rram_host_intg",
    # Controller operations and memory protection (REQ_RRAM_13, 14).
    "eg2_rram_ctrl_ops",
    "eg2_rram_mem_protection",
    # OTP partition (REQ_RRAM_16-18).
    "eg2_rram_otp_isolation",
    "eg2_rram_otp_rsw",
    "eg2_rram_otp_layout",
    # Seeds and keys (REQ_RRAM_21).
    "eg2_rram_init",
    # Alerts, escalation, power and clocks (REQ_RRAM_23, 25-27).
    "eg2_rram_crash_alert",
    "eg2_rram_prim_tl_access",
    "eg2_rram_idle_low_power",
    "eg2_rram_ctrl_clock_freqs",
    # Countermeasures (REQ_RRAM_08, 11, 24): chip_sw_rram_sec_cm_<countermeasure>.
    "eg2_rram_sec_cm",
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = src + mode + variant,
        srcs = [src + ".c"],
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = platform,
        deps = _DEPS,
    )
    for src in _TESTS
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    for variant, defines in [
        ("", []),
        ("_inject_fail", [
            src.upper() + "_INJECT_FAIL",
            "EG2_TEST_INJECT_FAIL",
        ]),
    ]
]
