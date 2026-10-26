# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The post-boot feature tests of Earl Grey 2 (eg2_top_verification spec REQ_PBF: "Post-boot
# features in CHERIoT mode", "Post-boot faults" and the per-area tables under "Post-Boot
# Features"). One source per test, built for RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`);
# `_inject_fail` (-DEG2_PBF_INJECT_FAIL) breaks the first expected value and must fail with
# `PBF FAIL` (REQ_STE_03). Each runs in eg2sim before the chip (REQ_STE_11).
#
# The `_sb` tests are also built behind the full secure-boot chain (exec env sim_dv_rom_ext: the
# ROM verifies ROM_EXT, ROM_EXT verifies the test as a signed owner image; dvsim image flag
# `rom_ext`, run mode sw_test_mode_mask_rom). Verilator has no secure-boot environment that fits
# (sim_verilator_rom_with_fake_keys signs the test as the ROM_EXT stage, not as an owner image
# behind ROM_EXT), so their Verilator targets boot from the test ROM.
#
# The tests are listed by group in the .bzl files next to this one (one per group, so the groups
# are written independently): each entry names the test, its own dependencies, whether it is built
# behind secure boot, its modes, and any extra sources it shares with its group ("srcs").
# The peripheral pair (chip_sw_eg2_pbf_periph_sb, _fault) is in sw/device/tests/eg2_periph.

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)
load(":crypto_tests.bzl", "CRYPTO_TESTS")
load(":memory_tests.bzl", "MEMORY_TESTS")
load(":system_tests.bzl", "SYSTEM_TESTS")

package(default_visibility = ["//visibility:public"])

# Checks, gaps, activity lines, the boot stage and index, the retention words, escalation.
cc_library(
    name = "eg2_pbf",
    srcs = ["eg2_pbf.c"],
    hdrs = ["eg2_pbf.h"],
    deps = [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:rstmgr_c_regs",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/eg2:test",
        "//sw/device/silicon_creator/lib:boot_log",
        "//sw/device/silicon_creator/lib/base:chip",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
        "//sw/device/tests/eg2_regmem",
    ],
)

_COMMON_DEPS = [
    ":eg2_pbf",
    "//hw/top:top_lib",
    "//hw/top/dt",
    "//sw/device/lib/base:memory",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/tests/eg2_regmem",
]

_ENVS = {
    "//hw/top_earlgrey:sim_dv": None,
    "//hw/top_earlgrey:sim_verilator": None,
}

_SB_ENVS = {
    "//hw/top_earlgrey:sim_dv": None,
    "//hw/top_earlgrey:sim_dv_rom_ext": None,
    "//hw/top_earlgrey:sim_verilator": None,
}

_TESTS = CRYPTO_TESTS + SYSTEM_TESTS + MEMORY_TESTS

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = t["name"] + mode + variant,
        srcs = [t["name"] + ".c"] + t.get("srcs", []),
        copts = select({
            "//toolchain:isa_cheriot": ["-fno-jump-tables"],
            "//conditions:default": [],
        }),
        exec_env = _SB_ENVS if t.get("secure_boot", False) else _ENVS,
        local_defines = defines,
        platform = platform,
        deps = _COMMON_DEPS + t.get("deps", []),
    )
    for t in _TESTS
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    if mode[1:] in t.get("modes", ["", "cheriot"])
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_PBF_INJECT_FAIL"]),
    ]
]
