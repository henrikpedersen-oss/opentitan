# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The debug-module tests of Earl Grey 2 (eg2_top_verification spec REQ_PBF_20 and REQ_TL_03,
# "Debug Controller"): chip_rv_dm_jtag, chip_rv_dm_dtm, chip_rv_dm_control_status and
# chip_rv_dm_sba_tl_errors. The bench drives the debug module over JTAG
# (env/seq_lib/chip_eg2_rv_dm_*_vseq.sv); the software keeps the core in a known loop that the
# bench ends over SBA (eg2_debug.h). One source per test, built for RISC-V mode (OTTF) and CHERIoT
# mode (`_cheriot`); `_inject_fail` (-D<TEST>_INJECT_FAIL and -DEG2_TEST_INJECT_FAIL) breaks the
# first expected value and must fail with `DEBUG FAIL` (REQ_STE_03). Each runs in eg2sim before
# the chip (REQ_STE_11), where the debug module's part is a listed model gap.

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "eg2_debug",
    srcs = ["eg2_debug.c"],
    hdrs = ["eg2_debug.h"],
    deps = [
        "//hw/top:rv_dm_c_regs",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:log",
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
    ":eg2_debug",
    "//hw/top:top_lib",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/tests/eg2_mem:eg2_chk",
]

# Sources (eg2_rv_dm_<x>.c is chip_rv_dm_<x>); every test runs in both modes.
_TESTS = [
    "eg2_rv_dm_jtag",
    "eg2_rv_dm_dtm",
    # CHERIoT mode: expected to fail on halt until ticket F13 (rv_dm has no CHERIoT input).
    "eg2_rv_dm_control_status",
    "eg2_rv_dm_sba_tl_errors",
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

# chip_sw_rv_dm_sba_cheriot_mode (REQ_CDM_06, REQ_CMS_16): CHERIoT mode only. Expected to fail on
# the chip's tag check until ticket F23 (SBA bypasses cheriot_mem_sys) is decided.
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_rv_dm_sba_cheriot_mode_cheriot" + variant,
        srcs = ["eg2_rv_dm_sba_cheriot_mode.c"],
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = CHERIOT_PLATFORM,
        deps = _DEPS,
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", [
            "EG2_RV_DM_SBA_CHERIOT_MODE_INJECT_FAIL",
            "EG2_TEST_INJECT_FAIL",
        ]),
    ]
]

# chip_rv_dm_lc_disabled_cheriot (REQ_CDM_05): CHERIoT mode only, in the life-cycle states where
# the core runs (chip_rv_dm_lc_disabled covers the others on the stub CPU).
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_rv_dm_lc_disabled_cheriot" + variant,
        srcs = ["eg2_rv_dm_lc_disabled.c"],
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = CHERIOT_PLATFORM,
        deps = _DEPS + [
            "//hw/top:lc_ctrl_c_regs",
            "//hw/top:rv_dm_c_regs",
            "//sw/device/lib/base:multibits",
        ],
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", [
            "EG2_RV_DM_LC_DISABLED_INJECT_FAIL",
            "EG2_TEST_INJECT_FAIL",
        ]),
    ]
]

# The debug module's corner tests (EG2 spec "Debug Controller" / "Corner cases",
# chip_eg2_rv_dm_*): the core runs the kernels of eg2_dm_corners.h the bench asks for while the
# bench times its debug requests against them (env/seq_lib/chip_eg2_rv_dm_corners_base_vseq.sv).
# Both modes; the CHERIoT-mode halts fail until ticket F13 (rv_dm has no CHERIoT input).
cc_library(
    name = "eg2_dm_corners",
    srcs = [
        "eg2_dm_corners.c",
        "eg2_dm_kernels.S",
    ],
    hdrs = ["eg2_dm_corners.h"],
    deps = [
        ":eg2_debug",
        "//hw/top:aon_timer_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:rv_dm_c_regs",
        "//hw/top:rv_plic_c_regs",
        "//hw/top:rv_timer_c_regs",
        "//hw/top:top_lib",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/eg2:test",
        "//sw/device/tests/eg2_mem:eg2_chk",
    ],
)

_CORNER_TESTS = [
    ("eg2_rv_dm_halt_corners", []),
    ("eg2_rv_dm_abstract_errors", []),
    ("eg2_rv_dm_reset_corners", []),
    ("eg2_rv_dm_lc_drop", [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:rv_dm_c_regs",
        "//sw/device/lib/base:multibits",
    ]),
    ("eg2_rv_dm_sba_corners", []),
    ("eg2_rv_dm_step_corners", []),
    ("eg2_rv_dm_counters", []),
    ("eg2_rv_dm_triggers", []),
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
        deps = _DEPS + [":eg2_dm_corners"] + extra_deps,
    )
    for src, extra_deps in _CORNER_TESTS
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

# chip_eg2_rv_dm_cap_bypass, chip_eg2_rv_dm_halt_revocation and chip_eg2_rv_dm_priv (EG2 spec
# "Debug Controller" / "Corner cases"): stages in which the core waits with known registers for
# the bench's program buffers (eg2_dm_cap.h), under eg2_dm_serve's command protocol and the
# eg2_cheriot probe (expected traps). The CHERIoT-mode halts fail until ticket F13.
cc_library(
    name = "eg2_dm_cap",
    srcs = [
        "eg2_dm_cap.c",
        "eg2_dm_cap_stage.S",
    ],
    hdrs = ["eg2_dm_cap.h"],
    deps = [
        ":eg2_dm_corners",
        "//sw/device/tests/eg2_cheriot:eg2_cheriot_rand",
    ],
)

_DMX_DEPS = _DEPS + [
    ":eg2_dm_cap",
    ":eg2_dm_corners",
    "//hw/top:cheriot_mem_sys_c_regs",
    "//hw/top:rv_plic_c_regs",
    "//sw/device/tests/eg2_cheriot:eg2_cheriot_rand",
    "//sw/device/tests/eg2_cheriot:eg2_memsys",
]

# (source, modes): cap_bypass and priv in both modes, halt_revocation in CHERIoT mode only.
_DMX_TESTS = [
    ("eg2_rv_dm_cap_bypass", ["", "_cheriot"], []),
    ("eg2_rv_dm_priv", ["", "_cheriot"], []),
    ("eg2_rv_dm_halt_revocation", ["_cheriot"], ["//sw/device/tests/eg2_cheriot:eg2_revoke"]),
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = src + mode + variant,
        srcs = [src + ".c"],
        copts = _COPTS,
        exec_env = _ENVS,
        local_defines = defines,
        platform = CHERIOT_PLATFORM if mode == "_cheriot" else None,
        deps = _DMX_DEPS + extra_deps,
    )
    for src, modes, extra_deps in _DMX_TESTS
    for mode in modes
    for variant, defines in [
        ("", []),
        ("_inject_fail", [
            src.upper() + "_INJECT_FAIL",
            "EG2_TEST_INJECT_FAIL",
        ]),
    ]
]
