# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The register, memory and connectivity tests of Earl Grey 2 (eg2_top_verification spec:
# "Registers and memories" in the RISC-V and CHERIoT test suites, and chip_sw_eg2_connectivity).
# One source per test, built for RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail`
# breaks one expected value and must fail (REQ_STE_03).
#
# eg2_regmem_tables.h is generated from the IPs' hjson by
# eg2_top_verification/scripts/gen_eg2_regmem_tables.py (`make gen`).

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

# The probe (one access that may fault), the pointer for a bus address, the knobs.
cc_library(
    name = "eg2_regmem",
    srcs = [
        "eg2_regmem.c",
        "eg2_regmem_probe.S",
    ],
    hdrs = [
        "eg2_regmem.h",
        "eg2_regmem_tables.h",
    ],
    deps = [
        "//sw/device/lib/base:macros",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/testing/eg2:test",
    ],
)

_TESTS = [
    # Must fail with `REG FAIL`: the first compared value is expected with one bit flipped.
    "eg2_rand_regs",
    # Must fail with `MEM FAIL`, likewise.
    "eg2_mem_test",
    # Must fail with `CONN <block> FAIL interrupt`: the first interrupt is expected at the wrong
    # rv_plic source ID.
    "eg2_connectivity",
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = name + mode + variant,
        srcs = [name + ".c"],
        exec_env = {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_verilator": None,
        },
        local_defines = defines,
        platform = platform,
        deps = [
            ":eg2_regmem",
            "//hw/top:alert_handler_c_regs",
            "//hw/top:aon_timer_c_regs",
            "//hw/top:clkmgr_c_regs",
            "//hw/top:lc_ctrl_c_regs",
            "//hw/top:pwrmgr_c_regs",
            "//hw/top:rom_ctrl_c_regs",
            "//hw/top:rstmgr_c_regs",
            "//hw/top:rv_core_ibex_c_regs",
            "//hw/top:sensor_ctrl_c_regs",
            "//hw/top:sram_ctrl_c_regs",
            "//hw/top:top_lib",
            "//sw/device/lib/base:memory",
            "//sw/device/lib/testing/eg2:test",
        ],
    )
    for name in _TESTS
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_REGMEM_INJECT_FAIL"]),
    ]
]
