# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The memory tests of Earl Grey 2 (eg2_top_verification spec: REQ_OTP, REQ_SRAM, REQ_CMS_14,
# REQ_PBF_19 and the REQ_PBF_11/15/18 testpoints given to this area: "OTP and SRAM in Both
# Modes", "Memory subsystem in RISC-V mode", "Post-boot faults"). One source per test, built for
# RISC-V mode (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail` (-D<TEST>_INJECT_FAIL and
# -DEG2_TEST_INJECT_FAIL) breaks the first expected value and must fail with `MEM FAIL`
# (REQ_STE_03). Each runs in eg2sim before the chip (REQ_STE_11).
#
# `eg2_chk` is shared by the memory, RRAM, debug and bus areas (eg2_mem, eg2_rram, eg2_debug,
# eg2_tl): checks, model-gap skips, bench synchronisation and calls that may trap.

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)
load("//rules:otbn.bzl", "otbn_binary")
load("@rules_python//python:defs.bzl", "py_binary")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "eg2_chk",
    srcs = [
        "eg2_chk.c",
        "eg2_chk_call.S",
    ],
    hdrs = ["eg2_chk.h"],
    deps = [
        "//sw/device/lib/base:macros",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/eg2:test",
    ],
)

# OTBN from the core's side (chip_sw_otbn_isa, chip_sw_otbn_keymgr).
cc_library(
    name = "eg2_otbn",
    srcs = ["eg2_otbn.c"],
    hdrs = ["eg2_otbn.h"],
    deps = [
        ":eg2_chk",
        "//hw/top:otbn_c_regs",
        "//sw/device/lib/testing/eg2:test",
    ],
)

# chip_sw_otbn_isa's second program: it only dumps the register files.
otbn_binary(
    name = "eg2_otbn_dump",
    srcs = ["eg2_otbn_dump.s"],
)

# OTBN reads its sideloaded key (KEY_S0/S1 WSRs) into DMEM (chip_sw_otbn_keymgr).
otbn_binary(
    name = "eg2_otbn_key_read",
    srcs = ["eg2_otbn_key_read.s"],
)

# The OTBN programs as word tables (the RV32 object otbn_binary embeds does not link with CHERIoT
# code): (program, header, name, symbols).
py_binary(
    name = "gen_otbn_image",
    srcs = ["gen_otbn_image.py"],
)

_OTBN_IMAGES = [
    ("//hw/ip/otbn/dv/smoke:smoke_test", "eg2_otbn_smoke_image", "Smoke", ["gpr_state", "wdr_state"]),
    (":eg2_otbn_dump", "eg2_otbn_dump_image", "Dump", ["gpr_state", "wdr_state"]),
    (":eg2_otbn_key_read", "eg2_otbn_key_read_image", "KeyRead", ["key_out"]),
]

[
    filegroup(
        name = hdr + "_elf",
        srcs = [prog],
        output_group = "elf",
    )
    for prog, hdr, _, _ in _OTBN_IMAGES
]

[
    genrule(
        name = hdr + "_gen",
        srcs = [":" + hdr + "_elf"],
        outs = [hdr + ".h"],
        cmd = "$(location :gen_otbn_image) --name {} {} --out $@ $(SRCS)".format(
            name,
            " ".join(["--sym " + s for s in syms]),
        ),
        tools = [":gen_otbn_image"],
    )
    for _, hdr, name, syms in _OTBN_IMAGES
]

[
    cc_library(
        name = hdr,
        hdrs = [hdr + ".h"],
    )
    for _, hdr, _, _ in _OTBN_IMAGES
]

# No switch lookup tables in the CHERIoT build (an unnamed capability array in .data).
_COPTS = select({
    "//toolchain:isa_cheriot": ["-fno-jump-tables"],
    "//conditions:default": [],
})

_ENVS = {
    "//hw/top_earlgrey:sim_dv": None,
    "//hw/top_earlgrey:sim_verilator": None,
}

_DEPS = [
    ":eg2_chk",
    "//hw/top:alert_handler_c_regs",
    "//hw/top:keymgr_dpe_c_regs",
    "//hw/top:kmac_c_regs",
    "//hw/top:lc_ctrl_c_regs",
    "//hw/top:otbn_c_regs",
    "//hw/top:otp_ctrl_c_regs",
    "//hw/top:pinmux_c_regs",
    "//hw/top:pwrmgr_c_regs",
    "//hw/top:rstmgr_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:rv_plic_c_regs",
    "//hw/top:sram_ctrl_c_regs",
    "//hw/top:top_lib",
    "//hw/top:usbdev_c_regs",
    "//sw/device/lib/base:memory",
    "//sw/device/lib/base:multibits",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/eg2:test",
]

# (source, extra deps); eg2_<x> is chip_sw_<x>. Every test runs in both modes.
_TESTS = [
    # chip_sw_sram_ctrl_meta: sram_ctrl_meta's key from OTP, INIT, key renewal, lc_escalate_en
    # (bench) and readback (REQ_CMS_14, REQ_OTP_02, REQ_SEC_18).
    ("eg2_sram_ctrl_meta", []),
    # chip_sw_sram_ctrl_meta_lc_escalation: chip_sw_sram_lc_escalation on sram_ctrl_meta, the
    # escalation through the alert handler and lc_ctrl (REQ_CMS_14, REQ_SEC_18).
    ("eg2_sram_ctrl_meta_lc_escalation", []),
    # chip_sw_sram_readback: the chip_sw_sram_readback testpoint on sec and retention SRAM
    # (REQ_PBF_19).
    ("eg2_sram_readback", []),
    # chip_sw_otp_ctrl_dai_ecc_error: an integrity error planted by the bench in an OTP word, then
    # read through the DAI (REQ_OTP_01).
    ("eg2_otp_ctrl_dai_ecc_error", []),
    # chip_sw_otbn_isa, chip_sw_otbn_keymgr (REQ_PBF_11).
    ("eg2_otbn_isa", [
        ":eg2_otbn",
        ":eg2_otbn_dump_image",
        ":eg2_otbn_smoke_image",
    ]),
    ("eg2_otbn_keymgr", [
        ":eg2_otbn",
        ":eg2_otbn_key_read_image",
        "//sw/device/lib/dif:keymgr_dpe",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/testing:keymgr_dpe_testutils",
    ]),
    # chip_sw_keymgr_dpe_sideload_kmac_error (REQ_PBF_15).
    ("eg2_keymgr_dpe_sideload_kmac_error", [
        "//sw/device/lib/dif:keymgr_dpe",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/testing:keymgr_dpe_testutils",
    ]),
    # chip_sw_usb_suspend (REQ_PBF_18).
    ("eg2_usb_suspend", [
        "//hw/top:aon_timer_c_regs",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/testing:usb_testutils",
        "//sw/device/lib/testing:usb_testutils_simpleserial",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ]),
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
        deps = _DEPS + extra,
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

# chip_sw_sram_ctrl_execution_main_cheriot (REQ_SRAM_01, REQ_TL_05): CHERIoT mode only (the
# RISC-V mode test is the legacy chip_sw_sram_ctrl_execution_main).
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_sram_execution_main_cheriot" + variant,
        srcs = [
            "eg2_sram_execution_main.c",
            "eg2_sram_exec_call.S",
        ],
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = CHERIOT_PLATFORM,
        deps = _DEPS,
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", [
            "EG2_SRAM_EXECUTION_MAIN_INJECT_FAIL",
            "EG2_TEST_INJECT_FAIL",
        ]),
    ]
]

# chip_sw_sram_ctrl_scrambled_access_cheriot (REQ_OTP_02): OpenTitan's sram_ctrl_scrambled_access
# test's checks in purecap CHERIoT code, on main, sec and retention SRAM; CHERIoT mode only (the
# RISC-V mode test is the legacy chip_sw_sram_ctrl_scrambled_access, its source unchanged).
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_sram_scrambled_access_cheriot" + variant,
        srcs = [
            "eg2_sram_scrambled_access_cheriot.c",
            "eg2_sram_scrambled_access_main.S",
        ],
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = CHERIOT_PLATFORM,
        deps = _DEPS,
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", [
            "EG2_SRAM_SCRAMBLED_ACCESS_CHERIOT_INJECT_FAIL",
            "EG2_TEST_INJECT_FAIL",
        ]),
    ]
]
