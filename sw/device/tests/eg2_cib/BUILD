# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

# The CHERIoT Ibex in the chip tests of Earl Grey 2 (eg2_top_verification spec "CHERIoT Ibex in
# the Chip", REQ_CIB_07): chip_sw_eg2_illegal_sweep and chip_sw_eg2_fetch_intg. One source per
# test, built for RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail`
# (-D<TEST>_INJECT_FAIL and -DEG2_TEST_INJECT_FAIL) breaks the first expected value and must fail
# with `ISA FAIL` or `FETCH FAIL` (REQ_STE_03). Each runs in eg2sim before the chip (REQ_STE_11).

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

# The decode table of the sweep, built from the ISA and IP documents (eg2_isa_decode.h).
cc_library(
    name = "eg2_isa_decode",
    srcs = ["eg2_isa_decode.c"],
    hdrs = ["eg2_isa_decode.h"],
    copts = select({
        "//toolchain:isa_cheriot": ["-fno-jump-tables"],
        "//conditions:default": [],
    }),
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
    ":eg2_isa_decode",
    "//hw/top:rram_ctrl_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:sram_ctrl_c_regs",
    "//hw/top:top_lib",
    "//sw/device/lib/base:multibits",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/tests/eg2_mem:eg2_chk",
    "//sw/device/tests/eg2_rram:eg2_rram",
]

# (source, extra sources); every test runs in both modes.
_TESTS = [
    # chip_sw_eg2_illegal_sweep: every 16-bit encoding and every 32-bit opcode x funct3 x funct7
    # with fixed operands, from ROM, RRAM and SRAM.
    ("eg2_illegal_sweep", ["eg2_isa_harness.S"]),
    # chip_sw_eg2_fetch_intg: an integrity error on a word fetched from ROM or SRAM.
    ("eg2_fetch_intg", ["eg2_isa_harness.S"]),
]

[
    opentitan_test(
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
