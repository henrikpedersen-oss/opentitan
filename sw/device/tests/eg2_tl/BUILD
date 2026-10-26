# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The TileLink tests of Earl Grey 2 (eg2_top_verification spec REQ_TL_03, REQ_TL_04, "TileLink in
# RISC-V mode" and the CHERIoT twins): chip_sw_eg2_tl_errors and chip_sw_eg2_tl_intg_links. One
# source per test, built for RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail`
# (-D<TEST>_INJECT_FAIL and -DEG2_TEST_INJECT_FAIL) breaks the first expected value and must fail
# with `TL FAIL` (REQ_STE_03). Each runs in eg2sim before the chip (REQ_STE_11).

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

_COPTS = select({
    "//toolchain:isa_cheriot": ["-fno-jump-tables"],
    "//conditions:default": [],
})

_ENVS = {
    "//hw/top_earlgrey:sim_dv": None,
    "//hw/top_earlgrey:sim_verilator": None,
}

_DEPS = [
    "//hw/top:cheriot_mem_sys_c_regs",
    "//hw/top:rv_timer_c_regs",
    "//hw/top:top_lib",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/tests/eg2_mem:eg2_chk",
]

# (source, extra sources); every test runs in both modes.
_TESTS = [
    # chip_sw_eg2_tl_errors: the explicit error cases from the core; the capability probe of the
    # CHERIoT twin.
    ("eg2_tl_errors", ["eg2_tl_cap.S"]),
    # chip_sw_eg2_tl_intg_links: one integrity bit flipped by the bench on an Earl Grey 2 link.
    ("eg2_tl_intg_links", []),
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = src + mode + variant,
        srcs = [src + ".c"] + extra,
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = platform,
        deps = _DEPS,
    )
    for src, extra in _TESTS
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
