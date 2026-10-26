# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The CHERIoT test suite of the EG2 verification specification (eg2_top_verification,
# eg2_toplevel_verification_spec.md, "CHERIoT Test Suite"), on the EG2 framework
# (doc/eg2_test_framework.md). A test that runs in both modes is one source built twice: RISC-V
# (`<name>`, OTTF) and CHERIoT (`<name>_cheriot`, platform CHERIOT_PLATFORM). Every test has an
# `_inject_fail` build, with `-D<TEST>_INJECT_FAIL`, that breaks one expected value and must fail
# (REQ_STE_03).
#
# Three kinds of image:
#  - framework tests (`//sw/device/lib/testing/eg2:test`): CHERIoT mode switched and locked by the
#    framework's start-up code before the test;
#  - late-switch tests (`:eg2_late`, eg2_late.h): the test's own assembly runs in RISC-V mode
#    first and switches itself (handover, the switch, resets across the switch);
#  - the OTTF smoke test: OTTF's CHERIoT build (test_framework/cheriot), unchanged test source.

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

# No switch lookup tables: in the CHERIoT build a table of pointers is an unnamed capability array
# in .data (inside the 4 KiB the CGP-relative accesses can reach, and invisible to
# `make check-cheriot`); the tests return pointers from switches.
_COPTS = ["-fno-jump-tables"]

# Every test also builds for the Verilator chip model (`make regr-vlt`).
_EXEC_ENV = {
    "//hw/top_earlgrey:sim_dv": None,
    "//hw/top_earlgrey:sim_verilator": None,
}

# `eg2_cheriot_rand` (EG2 spec, "Shared classes"): the capability model (from CHERIoT-Sail), the
# class weights, the tally, the test-owned memory, the capability pairs, and the expected-trap
# probes with their stubs and code snippets.
cc_library(
    name = "eg2_cheriot_rand",
    copts = _COPTS,
    srcs = [
        "eg2_cap_model.c",
        "eg2_cheriot_rand.c",
        "eg2_probe.S",
        "eg2_probe.c",
    ],
    hdrs = [
        "eg2_cap_model.h",
        "eg2_cheriot_rand.h",
        "eg2_probe.h",
    ],
    deps = [
        "//sw/device/lib/testing/eg2:dev",
        "//sw/device/lib/testing/eg2:params",
        "//sw/device/lib/testing/eg2:status",
    ],
)

# `eg2_memsys`: the memory-subsystem tests' helpers (meta SRAM map, CLC/CSC at an address, the
# revocation bitmap and engine, RRAM programming, SRAM INIT and re-keying, the boot record and the
# resets, escalation, bench steps).
cc_library(
    name = "eg2_memsys",
    copts = _COPTS,
    srcs = ["eg2_memsys.c"],
    hdrs = ["eg2_memsys.h"],
    deps = [
        ":eg2_cheriot_rand",
        "//hw/top:alert_handler_c_regs",
        "//hw/top:aon_timer_c_regs",
        "//hw/top:cheriot_mem_sys_c_regs",
        "//hw/top:pwrmgr_c_regs",
        "//hw/top:rram_ctrl_c_regs",
        "//hw/top:rstmgr_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:rv_plic_c_regs",
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top:top_lib",
        "//sw/device/lib/base:macros",
        "//sw/device/lib/testing/eg2:dev",
        "//sw/device/lib/testing/eg2:params",
        "//sw/device/lib/testing/eg2:status",
    ],
)

# `eg2_revoke`: the revocation corner cases' shared part (the load filter's expression, the
# instruction sequences, the bench steps of chip_sw_eg2_revocation_bench_vseq).
cc_library(
    name = "eg2_revoke",
    copts = _COPTS,
    srcs = [
        "eg2_revoke.S",
        "eg2_revoke.c",
    ],
    hdrs = ["eg2_revoke.h"],
    deps = [
        ":eg2_cheriot_rand",
        ":eg2_memsys",
        "//hw/top:cheriot_mem_sys_c_regs",
        "//hw/top:top_lib",
    ],
)

# The late-switch start-up code and C entry (eg2_late.h). CHERIoT images only.
cc_library(
    name = "eg2_late",
    copts = _COPTS,
    srcs = [
        "eg2_late_main.c",
        "eg2_late_start.S",
    ],
    hdrs = ["eg2_late.h"],
    target_compatible_with = ["//toolchain:isa_cheriot"],
    deps = [
        ":eg2_cheriot_rand",
        ":eg2_memsys",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top:top_lib",
        "//hw/top:uart_c_regs",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/testing/eg2:params",
        "//sw/device/lib/testing/eg2:status",
        "//sw/device/silicon_creator/lib:manifest_def",
    ],
    alwayslink = True,
)

_DEPS = [
    ":eg2_cheriot_rand",
    ":eg2_memsys",
    "//hw/top:gpio_c_regs",
    "//hw/top:rv_plic_c_regs",
    "//hw/top:rv_timer_c_regs",
    "//hw/top:top_lib",
]

_FRAMEWORK_DEPS = _DEPS + ["//sw/device/lib/testing/eg2:test"]

_LATE_DEPS = _DEPS + [":eg2_late"]

_MODES = {
    "": None,
    "_cheriot": CHERIOT_PLATFORM,
}

# The register tables of eg2_regmem (generated from the IPs' hjson), for the tests that walk the
# register windows.
_REGMEM_DEPS = _FRAMEWORK_DEPS + ["//sw/device/tests/eg2_regmem:eg2_regmem"]

# An OTTF test (OTTF's own test_main and console), with the EG2 parameter block.
_OTTF_DEPS = [
    ":eg2_cheriot_rand",
    "//hw/top:rv_core_ibex_c_regs",
    "//sw/device/lib/base:macros",
    "//sw/device/lib/base:mmio",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/eg2:params",
    "//sw/device/lib/testing/eg2:status",
    "//sw/device/lib/testing/eg2:tables",
    "//sw/device/lib/testing/test_framework:ottf_main",
]

_KIND_DEPS = {
    "framework": _FRAMEWORK_DEPS,
    "late": _LATE_DEPS,
    "regmem": _REGMEM_DEPS,
    "ottf": _OTTF_DEPS,
    "revoke": _FRAMEWORK_DEPS + [":eg2_revoke"],
}

# (image name, sources, the modes it runs in, its inject-fail define, the image kind, defines).
_TESTS = [
    # chip_sw_eg2_rand_cap_ops: CHERIoT only.
    ("eg2_rand_cap_ops", ["eg2_rand_cap_ops.c"], ["_cheriot"], "EG2_RAND_CAP_OPS_INJECT_FAIL", "framework", []),
    # chip_sw_eg2_rand_mem[_cheriot]: capability bounds edges (CHERIoT), ePMP region edges
    # (RISC-V).
    ("eg2_rand_mem", ["eg2_rand_mem.c"], ["", "_cheriot"], "EG2_RAND_MEM_INJECT_FAIL", "framework", []),
    # chip_sw_eg2_rand_traps[_cheriot]: PCC bounds edges, sentries, CJALR's checks, SR, illegal
    # encodings, interrupts (CHERIoT); ePMP execute edges, U mode, illegal encodings, interrupts
    # (RISC-V).
    ("eg2_rand_traps", ["eg2_rand_traps.c"], ["", "_cheriot"], "EG2_RAND_TRAPS_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_tag_roundtrip: CHERIoT only (REQ_CMS_10).
    ("eg2_tag_roundtrip", ["eg2_tag_roundtrip.c"], ["_cheriot"], "EG2_TAG_ROUNDTRIP_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_handover: the switch from RISC-V mode (REQ_SOC_07).
    ("eg2_handover", ["eg2_handover.c", "eg2_handover.S", "eg2_handover_params.h"], ["_cheriot"], "EG2_HANDOVER_INJECT_FAIL", "late", []),
    # chip_sw_cheriot_revocation: the load filter on marked granules (REQ_CMS_11).
    ("eg2_revocation", ["eg2_revocation.c"], ["_cheriot"], "EG2_REVOCATION_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_tbre_sweep_irq: a sweep and its interrupt through the PLIC (REQ_CMS_12).
    ("eg2_tbre_sweep_irq", ["eg2_tbre_sweep_irq.c"], ["_cheriot"], "EG2_TBRE_SWEEP_IRQ_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_nvm_cap_validate: verified capability stores to RRAM (REQ_CMS_17).
    ("eg2_nvm_cap_validate", ["eg2_nvm_cap_validate.c"], ["_cheriot"], "EG2_NVM_CAP_VALIDATE_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_sram_rekey_tag: INIT and key renewal clear tags (REQ_CMS_16).
    ("eg2_sram_rekey_tag", ["eg2_sram_rekey_tag.c"], ["_cheriot"], "EG2_SRAM_REKEY_TAG_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_rram_program_tag: RRAM controller writes clear tags (REQ_CMS_16).
    ("eg2_rram_program_tag", ["eg2_rram_program_tag.c"], ["_cheriot"], "EG2_RRAM_PROGRAM_TAG_INJECT_FAIL", "framework", []),
    # chip_cheriot_sba_tag_clear: SBA writes clear tags (REQ_CMS_16); expected to fail (F23).
    ("eg2_sba_tag_clear", ["eg2_sba_tag_clear.c"], ["_cheriot"], "EG2_SBA_TAG_CLEAR_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_meta_unmapped[_cheriot]: the meta SRAM unreachable (REQ_CMS_09).
    ("eg2_meta_unmapped", ["eg2_meta_unmapped.c"], ["", "_cheriot"], "EG2_META_UNMAPPED_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_mem_sys_e2e[_cheriot]: the block scenarios as firmware (REQ_CMS_19).
    ("eg2_mem_sys_e2e", ["eg2_mem_sys_e2e.c"], ["", "_cheriot"], "EG2_MEM_SYS_E2E_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_periph_cap_random: device windows through exact capabilities (REQ_PER_01/02).
    ("eg2_periph_cap_random", ["eg2_periph_cap_random.c"], ["_cheriot"], "EG2_PERIPH_CAP_RANDOM_INJECT_FAIL", "regmem", []),
    # chip_sw_cheriot_lock_policy and chip_sw_cheriot_periph_lock: one source (REQ_SOC_08).
    ("eg2_lock_policy", ["eg2_lock_policy.c"], ["_cheriot"], "EG2_LOCK_POLICY_INJECT_FAIL", "regmem", []),
    ("eg2_periph_lock", ["eg2_lock_policy.c"], ["_cheriot"], "EG2_PERIPH_LOCK_INJECT_FAIL", "regmem", ["EG2_PERIPH_LOCK"]),
    # chip_sw_rv_core_ibex_cheriot_addr_translation (REQ_CIB_04).
    ("eg2_addr_translation", ["eg2_addr_translation.c"], ["_cheriot"], "EG2_ADDR_TRANSLATION_INJECT_FAIL", "framework", []),
    # chip_sw_eg2_rand_revocation (REQ_CMS_11, REQ_CMS_12, REQ_PBF_22).
    ("eg2_rand_revocation", ["eg2_rand_revocation.c"], ["_cheriot"], "EG2_RAND_REVOCATION_INJECT_FAIL", "framework", []),
    # Faults the bench injects, escalated to a reset that the next boot judges (REQ_CMS_13,
    # REQ_CMS_18): chip_sw_cheriot_meta_intg_escalation, chip_sw_rv_core_ibex_cheriot_revbm_intg,
    # chip_sw_cheriot_bus_intg.
    ("eg2_meta_intg_escalation", ["eg2_fault_escalation.c"], ["_cheriot"], "EG2_META_INTG_ESCALATION_INJECT_FAIL", "framework", ["EG2_FAULT_META"]),
    ("eg2_revbm_intg", ["eg2_fault_escalation.c"], ["_cheriot"], "EG2_REVBM_INTG_INJECT_FAIL", "framework", ["EG2_FAULT_REVBM"]),
    ("eg2_bus_intg", ["eg2_fault_escalation.c"], ["_cheriot"], "EG2_BUS_INTG_INJECT_FAIL", "framework", ["EG2_FAULT_BUS"]),
    # chip_sw_rv_core_ibex_alerts_cheriot (REQ_CIB_02): a register-file bit flipped in CHERIoT mode.
    ("eg2_rf_intg", ["eg2_fault_escalation.c"], ["_cheriot"], "EG2_RF_INTG_INJECT_FAIL", "framework", ["EG2_FAULT_RF"]),
    # chip_cheriot_fi_meta: the fault campaign (REQ_CMS_18).
    ("eg2_fi_meta", ["eg2_fi_meta.c"], ["_cheriot"], "EG2_FI_META_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_reset_state (REQ_SOC_02) and chip_sw_cheriot_meta_init_after_reset
    # (REQ_CMS_15): one source, the state before the switch recorded by its assembly.
    ("eg2_reset_state", ["eg2_reset_state.c", "eg2_reset_late.S", "eg2_reset_params.h"], ["_cheriot"], "EG2_RESET_STATE_INJECT_FAIL", "late", []),
    ("eg2_meta_init_after_reset", ["eg2_reset_state.c", "eg2_reset_late.S", "eg2_reset_params.h"], ["_cheriot"], "EG2_META_INIT_AFTER_RESET_INJECT_FAIL", "late", ["EG2_META_INIT_TEST"]),
    # chip_sw_rv_core_ibex_cheriot_switch (REQ_SOC_03).
    ("eg2_switch", ["eg2_switch.c", "eg2_switch.S", "eg2_switch_params.h"], ["_cheriot"], "EG2_SWITCH_INJECT_FAIL", "late", []),
    # chip_sw_rv_core_ibex_cheriot_switch_err (REQ_SOC_04, REQ_CIB_01): RISC-V mode.
    ("eg2_switch_err", ["eg2_switch_err.c"], [""], "EG2_SWITCH_ERR_INJECT_FAIL", "framework", []),
    # chip_sw_cheriot_epmp_handover (REQ_SOC_06).
    ("eg2_epmp_handover", ["eg2_epmp_handover.c", "eg2_epmp_handover.S", "eg2_epmp_handover_params.h"], ["_cheriot"], "EG2_EPMP_HANDOVER_INJECT_FAIL", "late", []),
    # chip_sw_cheriot_ottf_smoke (REQ_STE_02): an OTTF test on OTTF's CHERIoT port.
    ("eg2_ottf_smoke", ["eg2_ottf_smoke.c"], ["_cheriot"], "EG2_OTTF_SMOKE_INJECT_FAIL", "ottf", []),
    # chip_sw_eg2_rand_isa[_cheriot] (REQ_CIB_06).
    ("eg2_rand_isa", ["eg2_rand_isa.c", "eg2_isa_run.S"], ["", "_cheriot"], "EG2_RAND_ISA_INJECT_FAIL", "framework", []),
    # chip_sw_eg2_testrig_seq (REQ_CIB_06): TestRIG streams (eg2_testrig_pool.h, generated by
    # eg2_top_verification scripts/testrig_pool.py).
    ("eg2_testrig_seq", ["eg2_testrig_seq.c", "eg2_testrig_run.S", "eg2_testrig_pool.h"], ["_cheriot"], "EG2_TESTRIG_SEQ_INJECT_FAIL", "framework", []),
    # The revocation corner cases (spec "Revocation", "Corner cases"; eg2_revoke.h).
    # chip_sw_cheriot_trvk_backpressure (REQ_CIB_06, REQ_CMS_11).
    ("eg2_trvk_backpressure", ["eg2_trvk_backpressure.c"], ["_cheriot"], "EG2_TRVK_BACKPRESSURE_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_trvk_truth_table (REQ_CMS_05, REQ_CMS_11).
    ("eg2_trvk_truth_table", ["eg2_trvk_truth_table.c"], ["_cheriot"], "EG2_TRVK_TRUTH_TABLE_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_tbre_core_write_snoop (REQ_CMS_12).
    ("eg2_tbre_core_write_snoop", ["eg2_tbre_core_write_snoop.c"], ["_cheriot"], "EG2_TBRE_CORE_WRITE_SNOOP_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_tbre_regs[_cheriot] (REQ_CMS_12): the start in RISC-V mode, the rest in CHERIoT.
    ("eg2_tbre_regs", ["eg2_tbre_regs.c"], ["", "_cheriot"], "EG2_TBRE_REGS_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_revbm_write_order (REQ_CMS_11).
    ("eg2_revbm_write_order", ["eg2_revbm_write_order.c"], ["_cheriot"], "EG2_REVBM_WRITE_ORDER_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_revocation_bounds (REQ_CMS_05, REQ_CMS_11, REQ_CIB_04).
    ("eg2_revocation_bounds", ["eg2_revocation_bounds.c"], ["_cheriot"], "EG2_REVOCATION_BOUNDS_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_trvk_fault (REQ_CIB_03): the glitch part of chip_sw_cheriot_revbm_errors.
    ("eg2_trvk_fault", ["eg2_trvk_fault.c"], ["_cheriot"], "EG2_TRVK_FAULT_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_revoke_store_clear (REQ_CMS_10, REQ_CMS_19).
    ("eg2_revoke_store_clear", ["eg2_revoke_store_clear.c"], ["_cheriot"], "EG2_REVOKE_STORE_CLEAR_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_revocation_race (REQ_CMS_11, REQ_CMS_12).
    ("eg2_revocation_race", ["eg2_revocation_race.c"], ["_cheriot"], "EG2_REVOCATION_RACE_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_tbre_fault_* (REQ_CMS_12, REQ_CMS_18).
    ("eg2_tbre_fault", ["eg2_tbre_fault.c"], ["_cheriot"], "EG2_TBRE_FAULT_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_revocation_irq (REQ_CIB_05, REQ_CMS_11, REQ_CMS_12).
    ("eg2_revocation_irq", ["eg2_revocation_irq.c", "eg2_revocation_irq.S"], ["_cheriot"], "EG2_REVOCATION_IRQ_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_fetch_backpressure (REQ_CIB_06, REQ_CMS_11).
    ("eg2_fetch_backpressure", ["eg2_fetch_backpressure.c", "eg2_fetch_backpressure.S"], ["_cheriot"], "EG2_FETCH_BACKPRESSURE_INJECT_FAIL", "revoke", []),
    # chip_cheriot_cap_halves (REQ_CMS_10, REQ_CDM_09): the stimulus of tb/eg2_cap_halves_chk.sv.
    ("eg2_cap_halves", ["eg2_cap_halves.c", "eg2_cap_halves.S"], ["_cheriot"], "EG2_CAP_HALVES_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_tbre_progress (REQ_CIB_06, REQ_CMS_12).
    ("eg2_tbre_progress", ["eg2_tbre_progress.c", "eg2_tbre_progress.S"], ["_cheriot"], "EG2_TBRE_PROGRESS_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_tbre_corners (REQ_CMS_12, REQ_CMS_15).
    ("eg2_tbre_corners", ["eg2_tbre_corners.c"], ["_cheriot"], "EG2_TBRE_CORNERS_INJECT_FAIL", "revoke", []),
    # chip_sw_cheriot_revocation_sleep (REQ_CMS_15, REQ_PBF_18).
    ("eg2_revocation_sleep", ["eg2_revocation_sleep.c"], ["_cheriot"], "EG2_REVOCATION_SLEEP_INJECT_FAIL", "revoke", []),
    ("eg2_revbm_errors", ["eg2_revbm_errors.c"], ["_cheriot"], "EG2_REVBM_ERRORS_INJECT_FAIL", "revoke", []),  # chip_sw_cheriot_revbm_errors_* (REQ_CMS_08, REQ_CIB_03)
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = name + mode + variant,
        srcs = srcs,
        exec_env = _EXEC_ENV,
        copts = _COPTS,
        local_defines = defines + extra,
        platform = _MODES[mode],
        deps = _KIND_DEPS[kind],
    )
    for name, srcs, modes, inject, kind, defines in _TESTS
    for mode in modes
    for variant, extra in [
        ("", []),
        ("_inject_fail", [inject]),
    ]
]
