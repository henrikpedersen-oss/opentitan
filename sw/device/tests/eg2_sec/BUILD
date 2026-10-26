# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The security, life-cycle, key-manager and ownership tests of Earl Grey 2 (eg2_top_verification
# spec: REQ_SEC, "Security", "Security in RISC-V mode"). One source per test, built for RISC-V mode
# (OTTF) and CHERIoT mode (`_cheriot`); `_inject_fail` breaks one expected value and must fail with
# `EG2CHK FAIL <what>` (REQ_STE_03). The shared helpers are //sw/device/tests/eg2_boot:eg2_boot.
#
# Tests that start from the test ROM run in sim_dv, sim_verilator and the SystemC model. Tests
# whose checks are about what ROM and ROM_EXT leave behind run behind the full secure-boot chain
# (`sim_dv_rom_ext`) only: see eg2_boot/BUILD for why they have no Verilator or model run.

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "dv_params",
    "opentitan_test",
)
load(
    "//rules:otp.bzl",
    "STD_OTP_OVERLAYS",
    "otp_image",
    "otp_json",
    "otp_partition",
)

package(default_visibility = ["//visibility:public"])

# chip_sw_eg2_sec_device_id (REQ_SEC_12) checks the identifier's format, which provisioning gives
# it: the default images draw DEVICE_ID at random, so the test loads this RMA image with a
# provisioned one (identities and root keys "Device Identifier"): Silicon Creator id 0x4001,
# product id 0x0002, device number 0x0123456789abcdef, the CRC-32 (IEEE 802.3) of those 12 bytes,
# then a 128-bit SKU half. The dvsim entry loads it (`:4`, +use_otp_image=OtpTypeCustom).
_DEVICE_ID = [
    "0x00024001",
    "0x89abcdef",
    "0x01234567",
    "0x13d47efc",  # CRC-32 of the 12 bytes before it (little-endian words)
    "0xa0a1a2a3",
    "0xb0b1b2b3",
    "0xc0c1c2c3",
    "0xd0d1d2d3",
]

otp_json(
    name = "otp_json_eg2_sec_device_id",
    partitions = [
        otp_partition(
            name = "HW_CFG0",
            items = {"DEVICE_ID": "0x" + "".join([w[2:] for w in reversed(_DEVICE_ID)])},
        ),
    ],
)

otp_image(
    name = "otp_img_eg2_sec_device_id_rma",
    src = "//hw/top_earlgrey/data/otp:otp_json_rma",
    overlays = STD_OTP_OVERLAYS + [":otp_json_eg2_sec_device_id"],
)

_DIRECT_DEPS = [
    "//hw/top:alert_handler_c_regs",
    "//hw/top:aon_timer_c_regs",
    "//hw/top:cheriot_mem_sys_c_regs",
    "//hw/top:hmac_c_regs",
    "//hw/top:keymgr_dpe_c_regs",
    "//hw/top:lc_ctrl_c_regs",
    "//hw/top:otp_ctrl_c_regs",
    "//hw/top:pwrmgr_c_regs",
    "//hw/top:rram_ctrl_c_regs",
    "//hw/top:rstmgr_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:sram_ctrl_c_regs",
    "//hw/top:top_lib",
    "//sw/device/lib/base:macros",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/tests/eg2_boot",
    "//sw/device/tests/eg2_regmem",
]

# Tests from the test ROM (direct boot), in both modes.
_DIRECT_TESTS = [
    # REQ_SEC_05. Must fail with `EG2CHK FAIL DAI write LC_STATE refused`.
    "eg2_lc_rram_rollback",
    # REQ_SEC_06 (the bench checks; this image runs after the encoding is restored). Must fail
    # with `EG2CHK FAIL CPU runs only in a defined state`.
    "eg2_lc_invalid_encodings",
    # REQ_SEC_07, dvsim chip_sw_eg2_sec_secret_partitions. Must fail with
    # `EG2CHK FAIL DAI read SECRET<n> error code`.
    "eg2_sec_lc_collateral",
    # REQ_SEC_08. Must fail with `EG2CHK FAIL K1 differs across RMA`.
    "eg2_sec_rma_key_change",
    # REQ_SEC_09. Must fail with `EG2CHK FAIL K1 changed with the input`.
    "eg2_keymgr_dpe_diversification",
    # REQ_SEC_10. Must fail with `EG2CHK FAIL generate above max`.
    "eg2_keymgr_dpe_locks",
    # REQ_SEC_12, with the provisioned OTP image above. Must fail with
    # `EG2CHK FAIL hardware half CRC-32`.
    "eg2_sec_device_id",
    # REQ_SEC_22. Must fail with `EG2CHK FAIL lockstep: alert and FATAL_CORE_ERR together`.
    "eg2_sec_partial_reset",
    # REQ_SEC_25. Must fail with `EG2CHK FAIL hmac digest cleared by the reset`.
    "eg2_sec_reset_residue",
]

_DIRECT_DV = {"eg2_sec_device_id": dv_params(otp = ":otp_img_eg2_sec_device_id_rma")}

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = name + mode + variant,
        srcs = [name + ".c"],
        dv = _DIRECT_DV.get(name, dv_params()),
        exec_env = {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_verilator": None,
        },
        local_defines = defines,
        platform = platform,
        deps = _DIRECT_DEPS,
    )
    for name in _DIRECT_TESTS
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    for variant, defines in [
        ("", []),
        ("_inject_fail", [name.upper() + "_INJECT_FAIL"]),
    ]
]

# Tests behind the full secure-boot chain (ROM, ROM_EXT, the test as a signed owner image), in both
# modes. No sim_verilator target and no model run (see eg2_boot/BUILD).
_SB_TESTS = [
    # REQ_SEC_02. Must fail with `EG2CHK FAIL RRAM MP_REGION_CFG_0 unchanged`.
    "eg2_sec_locks_after_switch",
    # REQ_SEC_03. Must fail with `EG2CHK FAIL read CreatorSecret (info 5)`.
    "eg2_sec_creator_isolation",
    # REQ_SEC_11 (the bench searches the SRAMs). Must fail with `EG2CHK FAIL boot_log bl0_slot`.
    "eg2_sec_wipe_before_switch",
    # REQ_SEC_14 (blocked: needs a signer for the ownership requests). Must fail with
    # `EG2CHK FAIL unlock accepted`.
    "eg2_ownership_power_loss",
    # REQ_SEC_26. Must fail with `EG2CHK FAIL rule 5: fetch from a device`.
    "eg2_sec_epmp_equivalents",
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = name + mode + variant,
        srcs = [name + ".c"],
        exec_env = {"//hw/top_earlgrey:sim_dv_rom_ext": None},
        local_defines = defines,
        platform = platform,
        deps = _DIRECT_DEPS + [
            "//sw/device/lib/base:hardened",
            "//sw/device/lib/base:multibits",
            "//sw/device/silicon_creator/lib:error",
            "//sw/device/silicon_creator/lib/boot_svc:boot_svc_ownership_activate",
            "//sw/device/silicon_creator/lib/boot_svc:boot_svc_ownership_unlock",
            "//sw/device/tests/eg2_boot:eg2_boot_svc",
        ],
    )
    for name in _SB_TESTS
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    for variant, defines in [
        ("", []),
        ("_inject_fail", [name.upper() + "_INJECT_FAIL"]),
    ]
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_sec_e2e_<case>_<variation>[_cheriot] (REQ_SEC_15, REQ_SEC_11, REQ_PBF_14): ROM_EXT's
# `attestation` and `dice_chain` end-to-end suites (sw/device/silicon_creator/rom_ext/e2e) in
# simulation, their sources unchanged, behind the full secure-boot chain, with each ROM_EXT DICE
# variation (X.509 and CWT certificates) and the owner image in each mode, as the boot area runs
# the `boot_svc` suite (eg2_boot/BUILD). chip_sw_eg2_sec_rom_ext_e2e_vseq makes the checks the FPGA
# host makes where the bench can (console lines, the certificates' presence and encoding) and
# records the mode each ran in. The host steps it cannot make are not here: verifying the
# certificates' signatures and contents against the key manager (sw/host/tests/attestation), the
# second bootstrap of `variation_interop`, and the re-bootstrap of `corrupted_digest`
# (status/dv_todo.md). No Verilator or model run: see eg2_boot/BUILD.
_E2E_ATTESTATION = "//sw/device/silicon_creator/rom_ext/e2e/attestation"

_E2E_DICE_CHAIN = "//sw/device/silicon_creator/rom_ext/e2e/dice_chain"

_E2E_CERT_DEPS = [
    "//sw/device/lib/base:status",
    "//sw/device/lib/runtime:log",
    "//sw/device/lib/testing/test_framework:check",
    "//sw/device/lib/testing/test_framework:ottf_main",
    "//sw/device/silicon_creator/lib:nvm_ctrl",
    "//sw/device/silicon_creator/manuf/base:perso_tlv_data",
]

_E2E_CASES = {
    # case: (source, defines, owner key, deps)
    "attestation_print_certs": (
        _E2E_ATTESTATION + ":print_certs.c",
        [],
        None,
        _E2E_CERT_DEPS + ["//sw/device/lib/runtime:print"],
    ),
    "dice_no_refresh": (
        _E2E_DICE_CHAIN + ":no_refresh_test.c",
        [],
        None,
        [
            "//sw/device/lib/base:status",
            "//sw/device/lib/runtime:log",
            "//sw/device/lib/testing/test_framework:check",
            "//sw/device/lib/testing/test_framework:ottf_main",
            "//sw/device/silicon_creator/lib/drivers:retention_sram",
            "//sw/device/silicon_creator/lib/drivers:rstmgr",
        ],
    ),
    "dice_debug_mode_off": (
        _E2E_DICE_CHAIN + ":debug_mode_test.c",
        ["TEST_EXPECT_DEBUG_OFF=1"],
        {"//sw/device/silicon_creator/lib/ownership/keys/fake:ecdsa_keyset": "app_prod_0"},
        _E2E_CERT_DEPS,
    ),
    "dice_debug_mode_on": (
        _E2E_DICE_CHAIN + ":debug_mode_test.c",
        ["TEST_EXPECT_DEBUG_ON=1"],
        {"//sw/device/silicon_creator/lib/ownership/keys/fake:ecdsa_keyset": "app_test_0"},
        _E2E_CERT_DEPS,
    ),
}

_E2E_ROM_EXT = {
    "x509": "//sw/device/silicon_creator/rom_ext:rom_ext_dice_x509_slot_a",
    "cwt": "//sw/device/silicon_creator/rom_ext:rom_ext_dice_cwt_slot_a",
}

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_sec_e2e_{}_{}{}".format(case, variation, mode),
        srcs = [src],
        dv = dv_params(rom_ext = rom_ext),
        ecdsa_key = key,
        exec_env = {"//hw/top_earlgrey:sim_dv_rom_ext": None},
        local_defines = defines,
        platform = platform,
        deps = deps,
    )
    for case, (src, defines, key, deps) in _E2E_CASES.items()
    for variation, rom_ext in _E2E_ROM_EXT.items()
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
]
