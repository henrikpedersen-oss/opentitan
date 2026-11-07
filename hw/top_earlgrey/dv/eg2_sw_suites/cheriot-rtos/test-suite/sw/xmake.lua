-- Copyright lowRISC Contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- CHERIoT RTOS firmware for the RTOS test SoC: the revocation and debug host directed tests, the Juliet CWE
-- compartment tests and the Cambridge CHERI-C tests. Moved here from sonata-software/examples
-- (sonata-software 5734522 plus its uncommitted fixes; see ../README.md), built against the
-- superproject's pinned cheriot-rtos submodule instead of sonata-software's.
--
-- Build inside the superproject's CHERIoT devShell (clang, xmake):
--   nix develop .#cheriot_sw_shell --command bash -c \
--     'cd ibex/dv/cheriot-rtos-test-suite/sw && xmake config --board=sonata-1.1 && xmake build <target>'
-- Targets: revocation_test_fw, dbg_host_test_fw, juliet_cwe_tests, cheri_c_tests. ELFs land in
-- build/cheriot/cheriot/release/.
--
-- Locations outside this directory, overridable from the environment:
--   CHERIOT_RTOS_DIR   cheriot-rtos checkout (default: the ibex submodule ../cheriot-rtos, beside this bench)
--   CHERI_C_TESTS_DIR  cheri-c-tests checkout (default: the ibex submodule ../cheri-c-tests)

local benchdir = path.absolute(path.join(os.scriptdir(), ".."))
local rtosdir  = os.getenv("CHERIOT_RTOS_DIR") or path.join(benchdir, "cheriot-rtos")
cheri_c_tests_dir = os.getenv("CHERI_C_TESTS_DIR") or path.join(benchdir, "cheri-c-tests")

set_project("CHERIoT RTOS test SoC firmware")
sdkdir = path.join(rtosdir, "sdk")
includes(sdkdir)
set_toolchains("cheriot-clang")

includes(path.join(sdkdir, "lib"))

option("board")
    set_default("sonata-1.1")

includes("tests/juliet_cwe", "tests/cheri_c_tests", "tests/revocation_test", "tests/dbg_host_test",
         "tests/perf_test")
