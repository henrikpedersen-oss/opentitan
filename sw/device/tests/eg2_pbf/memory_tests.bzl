# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""The memory, debug, ROM_EXT and CHERIoT group of the post-boot feature tests (REQ_PBF_14, 19,
20, 22); see BUILD.

Each entry: name (the source is <name>.c), deps (beyond the common ones in BUILD), secure_boot
(also built behind ROM_EXT, exec env sim_dv_rom_ext), modes ("" RISC-V, "cheriot"; default both).
Every test of the group shares eg2_pbf_mem.{h,c}: bench steps, the free memories, the RRAM and
SRAM controllers, the life cycle state, escalation to reset and the crash dumps.
"""

_MEM_SRCS = [
    "eg2_pbf_mem.c",
    "eg2_pbf_heap.h",
    "eg2_pbf_mem.h",
]

_MEM_DEPS = [
    "//hw/top:alert_handler_c_regs",
    "//hw/top:aon_timer_c_regs",
    "//hw/top:lc_ctrl_c_regs",
    "//hw/top:pwrmgr_c_regs",
    "//hw/top:rram_ctrl_c_regs",
    "//hw/top:rstmgr_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:sram_ctrl_c_regs",
    "//sw/device/lib/base:multibits",
    "//sw/device/lib/dif:rram_ctrl",
    "//sw/device/lib/testing:rram_ctrl_testutils",
]

_BOOT_SVC_DEPS = [
    "//sw/device/lib/testing:nv_counter_testutils",
    "//sw/device/silicon_creator/lib:boot_data",
    "//sw/device/silicon_creator/lib/boot_svc:boot_svc_msg",
    "//sw/device/silicon_creator/lib/drivers:retention_sram",
]

MEMORY_TESTS = [
    {"name": "eg2_pbf_memory_sb", "srcs": _MEM_SRCS, "deps": _MEM_DEPS, "secure_boot": True},
    {"name": "eg2_pbf_memory_fault", "srcs": _MEM_SRCS, "deps": _MEM_DEPS},
    {"name": "eg2_pbf_debug_sb", "srcs": _MEM_SRCS, "deps": _MEM_DEPS, "secure_boot": True},
    {"name": "eg2_pbf_debug_fault", "srcs": _MEM_SRCS, "deps": _MEM_DEPS},
    {"name": "eg2_pbf_meta_zeroed_sb", "srcs": _MEM_SRCS, "deps": _MEM_DEPS, "secure_boot": True},
    {"name": "eg2_pbf_rom_ext_fault", "srcs": _MEM_SRCS, "deps": _MEM_DEPS + _BOOT_SVC_DEPS, "secure_boot": True},
    {"name": "eg2_pbf_cheriot_sb", "srcs": _MEM_SRCS, "deps": _MEM_DEPS, "secure_boot": True, "modes": ["cheriot"]},
    {"name": "eg2_pbf_cheriot_fault", "srcs": _MEM_SRCS, "deps": _MEM_DEPS, "modes": ["cheriot"]},
    # REQ_PBF_22: an area's device through a compartment's grant, one access beyond it.
    {"name": "eg2_pbf_compartment", "modes": ["cheriot"]},
]
