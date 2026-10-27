# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR", "EG2_VERILATOR_LONG")

# The random-number tests of Earl Grey 2 (eg2_top_verification spec "Random Number Generation Test
# Suite", REQ_PBF_24, REQ_PBF_12). One source per test, built for RISC-V mode (OTTF) and CHERIoT
# mode (`_cheriot`); `_inject_fail` (-DEG2_RNG_INJECT_FAIL) breaks the first expected value (and,
# in rng_capture, writes every captured word twice) and must fail with `RNG FAIL` (REQ_STE_03).
# The chip bench drives entropy_src's noise input from `+eg2_rng=<source>`
# (hw/top_earlgrey/dv/eg2_rng, build mode en_eg2_rng); each test's vseq is in
# hw/top_earlgrey/dv/env/seq_lib/chip_sw_eg2_rng_vseq.sv.

load("@rules_python//python:defs.bzl", "py_binary")
load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

py_binary(
    name = "gen_rng_conditioner_kat",
    srcs = ["gen_rng_conditioner_kat.py"],
)

genrule(
    name = "eg2_rng_conditioner_kat_table",
    outs = ["eg2_rng_conditioner_kat_table.h"],
    cmd = "$(location :gen_rng_conditioner_kat) --out $@",
    tools = [":gen_rng_conditioner_kat"],
)

# Checks, the entropy complex's registers, bench steps, the capture's DV log lines.
cc_library(
    name = "eg2_rng",
    srcs = ["eg2_rng.c"],
    hdrs = ["eg2_rng.h"],
    copts = select({
        "//toolchain:isa_cheriot": ["-fno-jump-tables"],
        "//conditions:default": [],
    }),
    deps = [
        "//hw/top:entropy_src_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top/dt",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:csrng",
        "//sw/device/lib/dif:csrng_shared",
        "//sw/device/lib/dif:edn",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/eg2:test",
        "//sw/device/tests/eg2_regmem",
    ],
)

# rng_capture needs 3× the default timeout on Verilator in CHERIoT mode (cosim overhead,
# regr-vlt 2026-10-27: CHERIoT timeout at 1h, RISC-V passes at the same 64 capture_words).
_VLT_LONG = ["eg2_rng_capture"]

_TESTS = {
    "eg2_rng_capture": [],
    "eg2_rng_health": [],
    "eg2_rng_stuck": [],
    "eg2_rng_conditioner_kat": [":eg2_rng_conditioner_kat_table"],
}

[
    opentitan_test(
        verilator = EG2_VERILATOR_LONG if name in _VLT_LONG else EG2_VERILATOR,
        name = name + mode + variant,
        srcs = [name + ".c"] + extra,
        copts = select({
            "//toolchain:isa_cheriot": ["-fno-jump-tables"],
            "//conditions:default": [],
        }),
        exec_env = {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_verilator": None,
        },
        local_defines = defines,
        platform = platform,
        deps = [
            ":eg2_rng",
            "//hw/top:entropy_src_c_regs",
            "//hw/top/dt",
            "//sw/device/lib/base:multibits",
            "//sw/device/lib/dif:csrng",
            "//sw/device/lib/testing/eg2:test",
            "//sw/device/tests/eg2_regmem",
        ],
    )
    for name, extra in _TESTS.items()
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_RNG_INJECT_FAIL"]),
    ]
]
