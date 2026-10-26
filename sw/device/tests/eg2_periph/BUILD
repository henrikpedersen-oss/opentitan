# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The peripheral tests of Earl Grey 2 (eg2_top_verification spec: REQ_PBF_13,
# chip_sw_eg2_pbf_periph_sb and chip_sw_eg2_pbf_periph_fault). One source per test, built for
# RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail` breaks one expected value and
# must fail with `PERIPH FAIL` (REQ_STE_03). Each runs in eg2sim before the chip (REQ_STE_11).

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

# Pin routing to the chip bench's pads, and the UART, SPI host, I2C and GPIO helpers.
cc_library(
    name = "eg2_periph",
    srcs = ["eg2_periph.c"],
    hdrs = ["eg2_periph.h"],
    deps = [
        "//hw/top:gpio_c_regs",
        "//hw/top:i2c_c_regs",
        "//hw/top:pinmux_c_regs",
        "//hw/top:spi_host_c_regs",
        "//hw/top:top_lib",
        "//hw/top:uart_c_regs",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:i2c",
        "//sw/device/lib/testing/eg2:test",
    ],
)

_TESTS = [
    # Must fail with `PERIPH FAIL`: the first data check expects bit 0 flipped.
    "eg2_pbf_periph_sb",
    # Must fail with `PERIPH FAIL`: the first check after the fault expects bit 0 flipped.
    "eg2_pbf_periph_fault",
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = name + mode + variant,
        srcs = [name + ".c"],
        # The sb test also runs behind the full secure-boot chain (ROM, ROM_EXT, owner image).
        # Verilator too, for `make regr-vlt` (status/test_writing_rules.md).
        exec_env = {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_dv_rom_ext": None,
            "//hw/top_earlgrey:sim_verilator": None,
        } if name == "eg2_pbf_periph_sb" else {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_verilator": None,
        },
        local_defines = defines,
        platform = platform,
        deps = [
            ":eg2_periph",
            "//hw/top:gpio_c_regs",
            "//hw/top:i2c_c_regs",
            "//hw/top:rstmgr_c_regs",
            "//hw/top:spi_host_c_regs",
            "//hw/top:top_lib",
            "//hw/top:uart_c_regs",
            "//sw/device/lib/testing/eg2:test",
            "//sw/device/tests/eg2_regmem",
        ],
    )
    for name in _TESTS
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_PERIPH_INJECT_FAIL"]),
    ]
]
