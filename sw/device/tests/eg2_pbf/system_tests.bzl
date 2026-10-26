# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""The system group of the post-boot feature tests (REQ_PBF_16, 17, 18, 21); see BUILD.

Each entry: name (the source is <name>.c), deps (beyond the common ones in BUILD), secure_boot
(also built behind ROM_EXT, exec env sim_dv_rom_ext), modes ("" RISC-V, "cheriot"; default both),
srcs (the group's shared helper: the always-on timer, pwrmgr, the crash dump, lc_ctrl, OTP DAI).
"""

_SYSTEM_SRCS = [
    "eg2_pbf_system.c",
    "eg2_pbf_system.h",
]

_SYSTEM_DEPS = [
    "//hw/top:alert_handler_c_regs",
    "//hw/top:aon_timer_c_regs",
    "//hw/top:lc_ctrl_c_regs",
    "//hw/top:otp_ctrl_c_regs",
    "//hw/top:pinmux_c_regs",
    "//hw/top:pwrmgr_c_regs",
    "//hw/top:rstmgr_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:rv_plic_c_regs",
    "//hw/top:sensor_ctrl_c_regs",
    "//sw/device/lib/base:multibits",
]

SYSTEM_TESTS = [
    {"name": "eg2_pbf_lc_otp_sb", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS, "secure_boot": True},
    {"name": "eg2_pbf_lc_otp_fault", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS},
    {"name": "eg2_pbf_alert_sb", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS, "secure_boot": True},
    {"name": "eg2_pbf_alert_fault", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS},
    {"name": "eg2_pbf_power_sb", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS, "secure_boot": True},
    {"name": "eg2_pbf_power_fault", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS},
    {"name": "eg2_pbf_sensor_sb", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS, "secure_boot": True},
    {"name": "eg2_pbf_sensor_fault", "deps": _SYSTEM_DEPS, "srcs": _SYSTEM_SRCS},
]
