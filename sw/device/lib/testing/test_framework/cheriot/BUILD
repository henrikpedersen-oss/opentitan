# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//hw/top:defs.bzl", "opentitan_require_top")
load("//rules:linker.bzl", "ld_library")

package(default_visibility = ["//visibility:public"])

ld_library(
    name = "ottf_ld_common",
    includes = ["ottf_common.ld"],
    target_compatible_with = opentitan_require_top("earlgrey"),
    deps = [
        "//sw/device:info_sections",
        "//sw/device/lib/coverage:coverage_sections",
    ],
)

[
    ld_library(
        name = "ottf_ld_{}".format(slot),
        script = "ottf_{}.ld".format(slot),
        target_compatible_with = opentitan_require_top("earlgrey"),
        deps = [
            ":ottf_ld_common",
            "//hw/top:top_ld",
            "//sw/device/lib/testing/test_framework:ottf_ld_top_config",
        ],
    )
    for slot in [
        "silicon_creator_slot_a",
        "silicon_owner_slot_a",
    ]
]

# OTTF in CHERIoT mode (eg2_top_verification, framework step 4; EG2 spec REQ_STE_02). Selected by
# `//sw/device/lib/testing/test_framework:ottf_start` and `:ottf_isrs` on `//toolchain:isa_cheriot`
# only; the RISC-V build of OTTF is unchanged.

# OTTF's trap entry in CHERIoT mode: OTTF's frame and handlers, purecap underneath, and the
# function that points MTCC at it.
cc_library(
    name = "ottf_isrs_cheriot",
    srcs = ["ottf_isrs_cheriot.S"],
    target_compatible_with = ["//toolchain:isa_cheriot"],
    alwayslink = True,
)

# The start-up code: the EG2 framework's CHERIoT start-up code (mode switch and lock, CSP, CGP,
# globals, capability relocations; the source is shared, not copied), then `eg2_cheriot_main`,
# which here installs OTTF's trap entry and runs `_ottf_main`. The device capabilities the DIFs
# need come from the EG2 window table (`eg2:dev`, `ot_cheriot_mmio_cap`).
cc_library(
    name = "ottf_start_cheriot",
    srcs = [
        "ottf_cheriot_main.c",
        "//sw/device/lib/testing/eg2:cheriot/eg2_cheriot_start.S",
    ],
    target_compatible_with = ["//toolchain:isa_cheriot"],
    deps = [
        ":ottf_isrs_cheriot",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top:top_lib",
        "//hw/top:uart_c_regs",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/testing/eg2:dev",
    ],
    alwayslink = True,
)
