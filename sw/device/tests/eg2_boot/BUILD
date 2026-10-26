# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The secure-boot tests of Earl Grey 2 (eg2_top_verification spec: REQ_BOOT, "Secure Boot", "Secure
# boot in RISC-V mode", "Debug and boot"). One source per test, built for RISC-V mode (OTTF) and,
# where the spec's Modes column says so, CHERIoT mode (`_cheriot`); `_inject_fail` breaks one
# expected value and must fail with `EG2CHK FAIL <what>` (REQ_STE_03).
#
# Every test except chip_sw_eg2_boot_epmp_reset runs behind the full secure-boot chain (exec env
# `sim_dv_rom_ext`: the mask ROM verifies ROM_EXT, ROM_EXT verifies the test as an owner image
# signed with the fake owner key). They have no `sim_verilator` target: the Verilator exec env with
# a ROM (`sim_verilator_rom_with_fake_keys`) boots the test as a ROM_EXT, without ROM_EXT, so it
# cannot run what these tests check. The SystemC model cannot run them either (open decision 6: no
# OTBN, so no signature verification); their `_inject_fail` runs are shown on the chip
# (`make must-fail`). Image variants (other ROM_EXT keys, manifests, corrupted signatures, an older
# image in slot B) are built here, so no image is edited by hand.

load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "dv_params",
    "opentitan_binary",
    "opentitan_test",
)
load("//rules:const.bzl", "CONST", "hex")
load("//rules:manifest.bzl", "manifest")
load(
    "//rules:otp.bzl",
    "STD_OTP_OVERLAYS",
    "otp_image",
    "otp_json",
    "otp_partition",
)

package(default_visibility = ["//visibility:public"])

# Checks, probes, retention SRAM and boot log, the RRAM controller, keymgr_dpe, OTP DAI and lc_ctrl
# helpers; shared with //sw/device/tests/eg2_sec.
cc_library(
    name = "eg2_boot",
    srcs = [
        "eg2_boot.c",
        "eg2_boot_probe.S",
    ],
    hdrs = ["eg2_boot.h"],
    deps = [
        "//hw/top:keymgr_dpe_c_regs",
        "//hw/top:lc_ctrl_c_regs",
        "//hw/top:otp_ctrl_c_regs",
        "//hw/top:rram_ctrl_c_regs",
        "//hw/top:rstmgr_c_regs",
        "//hw/top:top_lib",
        "//sw/device/lib/base:macros",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/testing/eg2:test",
        "//sw/device/silicon_creator/lib:boot_log",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)

# Boot services requests through the retention SRAM, in both modes.
cc_library(
    name = "eg2_boot_svc",
    srcs = ["eg2_boot_svc.c"],
    hdrs = ["eg2_boot_svc.h"],
    deps = [
        ":eg2_boot",
        "//sw/device/silicon_creator/lib:error",
        "//sw/device/silicon_creator/lib/boot_svc:boot_svc_header",
        "//sw/device/silicon_creator/lib/boot_svc:boot_svc_msg",
    ],
)

_SB_DEPS = [
    ":eg2_boot",
    ":eg2_boot_svc",
    "//hw/top:keymgr_dpe_c_regs",
    "//hw/top:rram_ctrl_c_regs",
    "//hw/top:rstmgr_c_regs",
    "//hw/top:rv_core_ibex_c_regs",
    "//hw/top:top_lib",
    "//sw/device/lib/base:multibits",
    "//sw/device/lib/testing/eg2:test",
    "//sw/device/silicon_creator/lib:error",
    "//sw/device/silicon_creator/lib/boot_svc:boot_svc_empty",
    "//sw/device/silicon_creator/lib/boot_svc:boot_svc_min_bl0_sec_ver",
    "//sw/device/silicon_creator/lib/boot_svc:boot_svc_next_boot_bl0_slot",
    "//sw/device/silicon_creator/lib/boot_svc:boot_svc_ownership_history",
    "//sw/device/silicon_creator/lib/boot_svc:boot_svc_ownership_unlock",
    "//sw/device/tests/eg2_regmem",
]

_SB_ENV = {"//hw/top_earlgrey:sim_dv_rom_ext": None}

_MODES = [
    ("", None),
    ("_cheriot", CHERIOT_PLATFORM),
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_boot_epmp_reset (REQ_BOOT_05): the test is the boot ROM. Must fail with
# `EG2CHK FAIL RRAM fetch`.
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_boot_epmp_reset" + variant,
        srcs = [
            "eg2_boot_epmp_reset.c",
            "eg2_boot_epmp_reset_start.S",
        ],
        exec_env = {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_verilator": None,
        },
        kind = "rom",
        linker_script = "//sw/device/lib/testing/test_rom:linker_script",
        local_defines = defines,
        deps = [
            "//hw/top:rv_core_ibex_c_regs",
            "//hw/top:top_lib",
            "//sw/device/lib/arch:device",
            "//sw/device/lib/crt",
            "//sw/device/lib/testing/eg2:dev",
            "//sw/device/lib/testing/eg2:status",
            "//sw/device/silicon_creator/lib/base:static_critical",
        ],
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_BOOT_EPMP_RESET_INJECT_FAIL"]),
    ]
]

# --------------------------------------------------------------------------------------------------
# Owner images behind secure boot, in both modes:
#   eg2_boot_handover_protection (REQ_BOOT_10): `EG2CHK FAIL host store into ROM_EXT's slot`;
#   eg2_boot_keymgr_stage (REQ_BOOT_13): `EG2CHK FAIL Load Root Key refused`;
#   eg2_boot_svc_test (REQ_BOOT_12, REQ_SEC_16; dvsim chip_sw_eg2_boot_svc[_cheriot]):
#     `EG2CHK FAIL unlock with a bad signature: status`.
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = name + mode + variant,
        srcs = [name + ".c"],
        exec_env = _SB_ENV,
        local_defines = defines,
        platform = platform,
        deps = _SB_DEPS,
    )
    for name in [
        "eg2_boot_handover_protection",
        "eg2_boot_keymgr_stage",
        "eg2_boot_svc_test",
    ]
    for mode, platform in _MODES
    for variant, defines in [
        ("", []),
        ("_inject_fail", [name.upper() + "_INJECT_FAIL"]),
    ]
]

# --------------------------------------------------------------------------------------------------
# chip_sw_cheriot_secure_boot (REQ_BOOT_02): CHERIoT build only. Must fail with
# `EG2CHK FAIL boot_log bl0_slot`.
[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_cheriot_secure_boot" + variant,
        srcs = ["eg2_cheriot_secure_boot.c"],
        exec_env = _SB_ENV,
        local_defines = defines,
        platform = CHERIOT_PLATFORM,
        deps = _SB_DEPS,
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_CHERIOT_SECURE_BOOT_INJECT_FAIL"]),
    ]
]

# chip_sw_cheriot_secure_boot_bad_sig_<stage>_<corruption> (REQ_BOOT_02, REQ_BOOT_11): ROM_EXT or
# the owner image with one byte of its signature, its signed code (digest) or its public key (key
# id) flipped. The good images come from the binaries below; corrupt.sh flips the byte.
opentitan_binary(
    name = "eg2_cheriot_secure_boot_owner",
    testonly = True,
    srcs = ["eg2_cheriot_secure_boot.c"],
    exec_env = ["//hw/top_earlgrey:sim_dv_rom_ext"],
    platform = CHERIOT_PLATFORM,
    deps = _SB_DEPS,
)

filegroup(
    name = "eg2_cheriot_secure_boot_owner_signed_bin",
    testonly = True,
    srcs = [":eg2_cheriot_secure_boot_owner"],
    output_group = "sim_dv_rom_ext_signed_bin",
)

filegroup(
    name = "rom_ext_signed_bin",
    testonly = True,
    srcs = ["//sw/device/silicon_creator/rom_ext:rom_ext_dice_x509_slot_a"],
    output_group = "sim_dv_signed_bin",
)

# The manifest's offsets (manifest.h): ecdsa_signature at 0, ecdsa_public_key at 432; the signed
# code starts after the 1 KiB manifest.
_CORRUPTIONS = {
    "signature": 8,
    "digest": 0x1000,
    "key_id": 440,
}

[
    genrule(
        name = "{}_bad_{}".format(img, corruption),
        testonly = True,
        srcs = [":{}_signed_bin".format(img)],
        outs = ["{}_bad_{}.signed.bin".format(img, corruption)],
        cmd = "$(location corrupt.sh) $< $@ {}".format(offset),
        tools = ["corrupt.sh"],
    )
    for img in [
        "eg2_cheriot_secure_boot_owner",
        "rom_ext",
    ]
    for corruption, offset in _CORRUPTIONS.items()
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_cheriot_secure_boot_bad_sig_{}_{}".format(stage, corruption),
        srcs = ["eg2_boot_outcome.c"],
        dv = dv_params(
            assemble = (
                "--size=0x100000 {bad}@{rom_ext_slot_a} {firmware:signed_bin}@{owner_slot_a}" if stage == "rom_ext" else "--size=0x100000 {rom_ext:signed_bin}@{rom_ext_slot_a} {bad}@{owner_slot_a}"
            ),
            binaries = {
                ":{}_bad_{}".format(
                    "rom_ext" if stage == "rom_ext" else "eg2_cheriot_secure_boot_owner",
                    corruption,
                ): "bad",
            },
        ),
        exec_env = _SB_ENV,
        local_defines = ["EG2_BOOT_MUST_NOT_RUN"],
        platform = CHERIOT_PLATFORM,
        deps = _SB_DEPS,
    )
    for stage in [
        "rom_ext",
        "owner",
    ]
    for corruption in _CORRUPTIONS.keys()
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_boot_key_role (REQ_BOOT_06): ROM_EXT signed with each fake Silicon Creator key role
# (test, dev, prod) and with a key the ROM does not store (the fake owner key), assembled with the
# outcome image; the dvsim entries load each in each life-cycle state.
_ROM_EXT_KEYS = {
    "test": {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "test_key_0"},
    "dev": {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "dev_key_0"},
    "prod": {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "prod_key_0"},
    "unknown": {"//sw/device/silicon_creator/lib/ownership/keys/fake:ecdsa_keyset": "app_prod_0"},
}

[
    opentitan_binary(
        name = "rom_ext_key_{}".format(key),
        testonly = True,
        ecdsa_key = keyset,
        exec_env = ["//hw/top_earlgrey:sim_dv_base"],
        linker_script = "//sw/device/silicon_creator/rom_ext:ld_slot_a",
        manifest = "//sw/device/silicon_creator/rom_ext:manifest",
        slot = "a",
        spx_key = {"//sw/device/silicon_creator/rom/keys/fake/spx:spx_keyset": "prod_key_0"},
        transitive_features = [
            "minsize",
            "use_lld",
        ],
        deps = [
            "//sw/device/lib/crt",
            "//sw/device/silicon_creator/lib:manifest_def",
            "//sw/device/silicon_creator/lib/ownership:test_owner",
            "//sw/device/silicon_creator/lib/ownership/keys/fake",
            "//sw/device/silicon_creator/lib/rescue:rescue_xmodem",
            "//sw/device/silicon_creator/rom_ext:rom_ext_dice_x509",
            "//sw/device/silicon_creator/rom_ext/imm_section:main_section_dice_x509_slot_a",
        ],
    )
    for key, keyset in _ROM_EXT_KEYS.items()
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_boot_key_role_{}{}".format(key, variant),
        srcs = ["eg2_boot_outcome.c"],
        dv = dv_params(rom_ext = ":rom_ext_key_{}".format(key)),
        exec_env = _SB_ENV,
        local_defines = defines,
        deps = _SB_DEPS,
    )
    for key in _ROM_EXT_KEYS.keys()
    for variant, defines in [
        ("", []),
        # Must fail with `EG2CHK FAIL boot_log bl0_slot` where the image boots.
        ("_inject_fail", ["EG2_BOOT_OUTCOME_INJECT_FAIL"]),
    ]
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_boot_usage_constraints (REQ_BOOT_08): the owner image's manifest constrained to this
# device, another device, this life-cycle state, another one, or nothing; the OTP image gives the
# device its identifier (RMA, the bench's default state).
_DEVICE_ID = [
    "0xa0a1a2a3",
    "0x12345678",
    "0x00000003",
    "0xabababab",
    "0xcdcdcdcd",
    "0x01010101",
    "0x10101010",
    "0xf0f1f2f3",
]

otp_json(
    name = "otp_json_eg2_device_id",
    partitions = [
        otp_partition(
            name = "HW_CFG0",
            items = {"DEVICE_ID": "0x" + "".join([w[2:] for w in reversed(_DEVICE_ID)])},
        ),
    ],
)

otp_image(
    name = "otp_img_eg2_device_id_rma",
    src = "//hw/top_earlgrey/data/otp:otp_json_rma",
    overlays = STD_OTP_OVERLAYS + [":otp_json_eg2_device_id"],
)

_USAGE_CASES = {
    # case: (manifest fields, must the image boot)
    "this_device": ({"device_id": _DEVICE_ID}, True),
    "other_device": ({"device_id": ["0xbad"] + _DEVICE_ID[1:]}, False),
    "this_state": ({"life_cycle_state": hex(CONST.LCV_SW.RMA)}, True),
    "other_state": ({"life_cycle_state": hex(CONST.LCV_SW.DEV)}, False),
    "none": ({}, True),
}

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_boot_usage_constraints_{}".format(case),
        srcs = ["eg2_boot_outcome.c"],
        dv = dv_params(otp = ":otp_img_eg2_device_id_rma"),
        exec_env = _SB_ENV,
        local_defines = [] if boots else ["EG2_BOOT_MUST_NOT_RUN"],
        manifest = manifest(dict(
            fields,
            name = "manifest_eg2_usage_{}".format(case),
            identifier = hex(CONST.OWNER),
        )),
        deps = _SB_DEPS,
    )
    for case, (fields, boots) in _USAGE_CASES.items()
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_boot_manifest_bad (REQ_BOOT_14): one malformed field of the owner image's manifest
# per image, each corruption class at least once: zero (identifier), all ones (identifier), off
# by one (entry point at the code end, an empty code region), random (code outside the slot, a
# length past the slot).
_M = CONST.MANIFEST_SIZE

_MANIFEST_CASES = {
    "identifier_zero": {"identifier": "0"},
    "entry_at_code_end": {
        "identifier": hex(CONST.OWNER),
        "code_start": hex(_M + 8),
        "code_end": hex(_M + 12),
        "entry_point": hex(_M + 12),
    },
    "code_empty": {
        "identifier": hex(CONST.OWNER),
        "code_start": hex(_M + 12),
        "code_end": hex(_M + 12),
    },
    "code_outside_slot": {
        "identifier": hex(CONST.OWNER),
        "code_start": hex(0x5a5a0),
        "code_end": hex(0x5a5b0),
        "entry_point": hex(0x5a5a0),
    },
    # All ones goes in the identifier: opentitantool refuses to sign an image whose entry point,
    # code start or code end lies past the image (opentitanlib image.rs), so those fields cannot be
    # all ones.
    "identifier_all_ones": {"identifier": hex(0xffffffff)},
    "length_past_slot": {
        "identifier": hex(CONST.OWNER),
        "length": hex(0x100000),
    },
}

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_boot_manifest_bad_{}".format(case),
        srcs = ["eg2_boot_outcome.c"],
        exec_env = _SB_ENV,
        local_defines = ["EG2_BOOT_MUST_NOT_RUN"],
        manifest = manifest(dict(
            fields,
            name = "manifest_eg2_bad_{}".format(case),
        )),
        deps = _SB_DEPS,
    )
    for case, fields in _MANIFEST_CASES.items()
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_boot_rollback (REQ_BOOT_09): this image (security version 2) in slot A, the same
# source built as an older image (security version 1) in slot B. Must fail with
# `EG2CHK FAIL lower minimum refused`.
opentitan_binary(
    name = "eg2_boot_rollback_old",
    testonly = True,
    srcs = ["eg2_boot_rollback.c"],
    exec_env = ["//hw/top_earlgrey:sim_dv_rom_ext"],
    linker_script = "//sw/device/lib/testing/test_framework:ottf_ld_silicon_owner_slot_b",
    local_defines = ["EG2_BOOT_ROLLBACK_OLD"],
    manifest = manifest({
        "name": "manifest_eg2_rollback_old",
        "identifier": hex(CONST.OWNER),
        "security_version": "1",
    }),
    deps = _SB_DEPS,
)

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_boot_rollback" + variant,
        srcs = ["eg2_boot_rollback.c"],
        dv = dv_params(
            assemble = "--size=0x1FF400 {rom_ext:signed_bin}@{rom_ext_slot_a} {firmware:signed_bin}@{owner_slot_a} {old:signed_bin}@{owner_slot_b}",
            binaries = {":eg2_boot_rollback_old": "old"},
        ),
        exec_env = _SB_ENV,
        local_defines = defines,
        manifest = manifest({
            "name": "manifest_eg2_rollback" + variant,
            "identifier": hex(CONST.OWNER),
            "security_version": "2",
        }),
        deps = _SB_DEPS,
    )
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_BOOT_ROLLBACK_INJECT_FAIL"]),
    ]
]

# --------------------------------------------------------------------------------------------------
# chip_sw_eg2_boot_svc_e2e_<case>[_cheriot] (REQ_BOOT_12, REQ_PBF_14): ROM_EXT's `boot_svc`
# end-to-end suite (sw/device/silicon_creator/rom_ext/e2e/boot_svc) in simulation, its sources
# unchanged, the owner image in each mode, behind the full secure-boot chain with the image in both
# owner slots as the FPGA suite assembles it. On the FPGA the host matches the console line
# `FinalBootLog: <boots>:<slots>`; in simulation the slots are a RAM string the DV log cannot
# print, so chip_sw_eg2_boot_outcome_vseq reads the test's state in the retention SRAM's owner
# area (boot_svc_test_lib.h) and compares it with +eg2_boot_final_log, the FPGA suite's expected
# value. The cases with a host step (rescue, wake-up from sleep, the minimum version with a second
# signed image) are not here (status/dv_todo.md).
#
# The `_cheriot` builds: only the cases that boot slot A throughout (empty, bad_next). The others
# need the image linked for the virtual window both owner slots map to
# (ottf_ld_silicon_owner_slot_virtual), and the CHERIoT test framework has a slot A linker script
# only (status/dv_todo.md).
_E2E_BOOT_SVC = "//sw/device/silicon_creator/rom_ext/e2e/boot_svc"

_E2E_BOOT_SVC_CASES = {
    # case: (source, defines)
    "empty": ("boot_svc_empty_test.c", []),
    "next_aab": ("boot_svc_next_test.c", ["TEST_SEQUENCE_AAB=1"]),
    "next_aba": ("boot_svc_next_test.c", ["TEST_SEQUENCE_ABA=1"]),
    "next_abb": ("boot_svc_next_test.c", ["TEST_SEQUENCE_ABB=1"]),
    "primary": ("boot_svc_primary_test.c", []),
    "bad_next": ("boot_svc_bad_next_test.c", []),
    "bad_primary": ("boot_svc_bad_primary_test.c", []),
}

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_boot_svc_e2e_{}{}".format(case, mode),
        srcs = ["{}:{}".format(_E2E_BOOT_SVC, src)],
        dv = dv_params(
            assemble = "--size=0x1FF400 {rom_ext:signed_bin}@{rom_ext_slot_a} {firmware:signed_bin}@{owner_slot_a} {firmware:signed_bin}@{owner_slot_b}",
        ),
        exec_env = _SB_ENV,
        linker_script = (
            "//sw/device/lib/testing/test_framework/cheriot:ottf_ld_silicon_owner_slot_a" if platform else "//sw/device/lib/testing/test_framework:ottf_ld_silicon_owner_slot_virtual"
        ),
        local_defines = defines,
        platform = platform,
        deps = [
            _E2E_BOOT_SVC + ":boot_svc_test_lib",
            "//sw/device/lib/base:status",
            "//sw/device/lib/runtime:log",
            "//sw/device/lib/testing/test_framework:check",
            "//sw/device/lib/testing/test_framework:ottf_main",
            "//sw/device/silicon_creator/lib:boot_log",
            "//sw/device/silicon_creator/lib/boot_svc:boot_svc_empty",
            "//sw/device/silicon_creator/lib/boot_svc:boot_svc_msg",
            "//sw/device/silicon_creator/lib/boot_svc:boot_svc_next_boot_bl0_slot",
            "//sw/device/silicon_creator/lib/drivers:retention_sram",
            "//sw/device/silicon_creator/lib/drivers:rstmgr",
        ],
    )
    for case, (src, defines) in _E2E_BOOT_SVC_CASES.items()
    for mode, platform in _MODES
    if not platform or case in ["empty", "bad_next"]
]
