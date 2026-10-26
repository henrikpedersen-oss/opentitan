# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//sw/device/tests:eg2_exec.bzl", "EG2_VERILATOR")

# The NIST CAVP chip tests of Earl Grey 2 (eg2_top_verification spec: NIST CAVP Test Suite,
# REQ_PBF_23). Each test's vector table is generated from the CAVP archives OpenTitan pins
# (third_party/nist_cavp_testvectors) by gen_nist_vectors.py; nothing is copied by hand.

load("@rules_python//python:defs.bzl", "py_binary")
load(
    "//rules/opentitan:defs.bzl",
    "CHERIOT_PLATFORM",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

py_binary(
    name = "gen_nist_vectors",
    srcs = ["gen_nist_vectors.py"],
)

cc_library(
    name = "eg2_nist",
    srcs = ["eg2_nist.c"],
    hdrs = ["eg2_nist.h"],
    deps = ["//sw/device/lib/testing/eg2:test"],
)

# SHA-2 and HMAC through the hmac registers (the DIF covers SHA-2 256 with a 256-bit key only).
cc_library(
    name = "eg2_nist_sha2_engine",
    srcs = ["eg2_nist_sha2_engine.c"],
    hdrs = ["eg2_nist_sha2_engine.h"],
    deps = [
        "//hw/top:hmac_c_regs",
        "//hw/top/dt",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/testing/eg2:test",
    ],
)

# The DRBG archive is a zip of zips: its three CTR_DRBG.rsp files, unpacked.
[
    genrule(
        name = "ctr_drbg_" + variant,
        srcs = ["@nist_cavp_drbg_sp_800_90a_root//:drbgvectors_{}.zip".format(variant)],
        outs = ["drbgvectors_{}/CTR_DRBG.rsp".format(variant)],
        cmd = "unzip -p $< CTR_DRBG.rsp > $@",
    )
    for variant in [
        "no_reseed",
        "pr_false",
        "pr_true",
    ]
]

_AES_KAT = [
    "@nist_cavp_aes_kat//:{}{}{}.rsp".format(mode, kind, bits)
    for mode in [
        "ECB",
        "CBC",
        "CFB128",
        "OFB",
    ]
    for kind in [
        "GFSbox",
        "KeySbox",
        "VarKey",
        "VarTxt",
    ]
    for bits in [
        "128",
        "192",
        "256",
    ]
]

_GCM = [
    "@nist_cavp_aes_gcm//:gcm{}{}.rsp".format(op, bits)
    for op in [
        "EncryptExtIV",
        "Decrypt",
    ]
    for bits in [
        "128",
        "192",
        "256",
    ]
]

_SHA2 = [
    "@nist_cavp_sha2_fips_180_4//:SHA{}{}.rsp".format(bits, kind)
    for bits in [
        "256",
        "384",
        "512",
    ]
    for kind in [
        "ShortMsg",
        "LongMsg",
        "Monte",
    ]
]

_SHA3 = [
    "@nist_cavp_sha3_fips_202//:SHA3_{}{}.rsp".format(bits, kind)
    for bits in [
        "224",
        "256",
        "384",
        "512",
    ]
    for kind in [
        "ShortMsg",
        "LongMsg",
        "Monte",
    ]
] + [
    "@nist_cavp_shake_fips_202//:SHAKE{}{}.rsp".format(bits, kind)
    for bits in [
        "128",
        "256",
    ]
    for kind in [
        "ShortMsg",
        "LongMsg",
        "VariableOut",
        "Monte",
    ]
]

_DRBG = [":drbgvectors_{}/CTR_DRBG.rsp".format(v) for v in [
    "no_reseed",
    "pr_false",
    "pr_true",
]]

# name, generator kind, vector files, random vectors per file in the pool, engine deps.
_TESTS = [
    ("aes", "aes", _AES_KAT, 4, ["//sw/device/lib/dif:aes"]),
    # GCM through the crypto library (software GHASH over the AES engine): EG2 has no GCM in
    # hardware (EG2 HLS; top_earlgrey.hjson AESGCMEnable 0), 2026-10-26.
    ("aes_gcm", "gcm", _GCM, 4, [
        "//sw/device/lib/base:hardened",
        "//sw/device/lib/crypto/drivers:aes",
        "//sw/device/lib/crypto/impl/aes_gcm",
        "//sw/device/lib/crypto/include:crypto_hdrs",
        "//sw/device/lib/crypto/include:datatypes",
        "//sw/device/lib/testing:entropy_testutils",
    ]),
    ("sha2", "sha2", _SHA2, 2, [":eg2_nist_sha2_engine"]),
    ("hmac", "hmac", ["@nist_cavp_hmac_fips_198_1//:HMAC.rsp"], 6, [":eg2_nist_sha2_engine"]),
    ("sha3", "sha3", _SHA3, 2, ["//sw/device/lib/dif:kmac", "//hw/top:kmac_c_regs"]),
    ("drbg", "drbg", _DRBG, 1, ["//sw/device/lib/dif:csrng", "//sw/device/lib/dif:csrng_shared"]),
]

[
    genrule(
        name = "eg2_nist_{}_vectors".format(name),
        srcs = files,
        outs = ["eg2_nist_{}_vectors.c".format(name)],
        cmd = "$(location :gen_nist_vectors) --kind {} --random {} --out $@ $(SRCS)".format(kind, nrandom),
        tools = [":gen_nist_vectors"],
    )
    for name, kind, files, nrandom, _ in _TESTS
]

# Key wrap with padding through the crypto library, and the OTBN tests through its OTBN drivers
# (the formats of eg2_model/nist/nist_otbn_check.py). Each lists the algorithm's other files of
# the archive it does not run (`--not-run`), so the run reports them as "not run".
_KW = "@nist_cavp_aes_kw_sp_800_38f//:"

_KWP = [_KW + "KWP_{}_{}.txt".format(op, bits) for op in [
    "AE",
    "AD",
] for bits in [
    "128",
    "192",
    "256",
]]

_KW_NOT_RUN = [_KW + "KW_{}_{}{}.txt".format(op, bits, inv) for op in [
    "AE",
    "AD",
] for bits in [
    "128",
    "192",
    "256",
] for inv in [
    "",
    "_inv",
]] + [_KW + "KWP_{}_{}_inv.txt".format(op, bits) for op in [
    "AE",
    "AD",
] for bits in [
    "128",
    "192",
    "256",
]] + [_KW + "TKW_{}{}.txt".format(op, inv) for op in [
    "AE",
    "AD",
] for inv in [
    "",
    "_inv",
]]

_ECDSA = "@nist_cavp_ecdsa_fips_186_4//:"

_RSA = "@nist_cavp_rsa_fips_186_3//:"

_OTBN_DEPS = [
    ":eg2_nist_otbn",
    "//sw/device/lib/base:hardened",
]

# name, generator kind, vector files, files not run, random vectors per file, engine deps.
_TESTS_LIB = [
    ("aes_kw", "kwp", _KWP, _KW_NOT_RUN, 4, [
        "//sw/device/lib/base:hardened",
        "//sw/device/lib/crypto/drivers:aes",
        "//sw/device/lib/crypto/impl/aes_kwp",
        "//sw/device/lib/testing:entropy_testutils",  # 2026-10-26: EDN0 in auto mode first
    ]),
    ("otbn_ecdsa", "ecdsa", [
        _ECDSA + "SigVer.rsp",
        _ECDSA + "SigGen.txt",
    ], [], 2, _OTBN_DEPS + [
        "//sw/device/lib/crypto/impl/ecc:p256",
        "//sw/device/lib/crypto/impl/ecc:p384",
    ]),
    ("otbn_ecdh", "ecdh", ["@nist_cavp_ecdh_sp_800_56a//:KAS_ECC_CDH_PrimitiveTest.txt"], [], 2, _OTBN_DEPS + [
        "//sw/device/lib/crypto/impl/ecc:p256",
        "//sw/device/lib/crypto/impl/ecc:p384",
    ]),
    ("otbn_rsa", "rsa", [
        _RSA + "SigVer15_186-3.rsp",
        _RSA + "SigGen15_186-3.txt",
    ], [], 2, _OTBN_DEPS + [
        "//sw/device/lib/base:crc32",
        "//sw/device/lib/crypto/impl/rsa:run_rsa",
    ]),
]

# The budget and model gap of the OTBN tests.
cc_library(
    name = "eg2_nist_otbn",
    srcs = ["eg2_nist_otbn.c"],
    hdrs = ["eg2_nist_otbn.h"],
    deps = [
        ":eg2_nist",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/testing/eg2:test",
    ],
)

[
    genrule(
        name = "eg2_nist_{}_vectors".format(name),
        srcs = files + not_run,
        outs = ["eg2_nist_{}_vectors.c".format(name)],
        cmd = "$(location :gen_nist_vectors) --kind {} --random {} --out $@ {} {}".format(
            kind,
            nrandom,
            " ".join(["$(location {})".format(f) for f in files]),
            " ".join(["--not-run $(location {})".format(f) for f in not_run]),
        ),
        tools = [":gen_nist_vectors"],
    )
    for name, kind, files, not_run, nrandom, _ in _TESTS_LIB
]

[
    opentitan_test(
        verilator = EG2_VERILATOR,
        name = "eg2_nist_{}{}{}".format(name, mode, variant),
        srcs = [
            "eg2_nist_{}.c".format(name),
            ":eg2_nist_{}_vectors".format(name),
        ],
        exec_env = {
            "//hw/top_earlgrey:sim_dv": None,
            "//hw/top_earlgrey:sim_verilator": None,
        },
        local_defines = defines,
        platform = platform,
        deps = [
            ":eg2_nist",
            "//hw/top/dt",
            "//sw/device/lib/base:memory",
            "//sw/device/lib/testing/eg2:test",
        ] + engine_deps,
    )
    for name, engine_deps in [(t[0], t[4]) for t in _TESTS] + [(t[0], t[5]) for t in _TESTS_LIB]
    for mode, platform in [
        ("", None),
        ("_cheriot", CHERIOT_PLATFORM),
    ]
    # The OTBN tests are RISC-V mode only until rules/otbn.bzl embeds the OTBN programs for the
    # CHERIoT ABI: its `<program>.rv32embed.o` carries ELF flags 0, and the CHERIoT linker refuses
    # the archive holding it ("is incompatible with" the purecap objects) even when another
    # object already defines its symbols.
    if not (mode == "_cheriot" and name.startswith("otbn_"))
    for variant, defines in [
        ("", []),
        ("_inject_fail", ["EG2_NIST_INJECT_FAIL"]),
    ]
]

