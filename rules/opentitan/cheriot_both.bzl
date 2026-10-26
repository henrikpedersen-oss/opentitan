# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""Both modes: a legacy `opentitan_test` also built for CHERIoT mode, from the same source.

Earl Grey 2 (eg2_top_verification, EG2 spec REQ_STE_02 and REQ_PER) runs OpenTitan's legacy chip
tests in RISC-V mode and, unchanged, in CHERIoT mode. For every test listed in
`//sw/device/tests/cheriot_both:tests.bzl`, `opentitan_test` (defs.bzl) also defines
`<name>_cheriot`: the same sources, dependencies, defines and DV parameters, built for
`CHERIOT_PLATFORM` (purecap; OTTF's CHERIoT start-up code switches and locks CHERIoT mode before
`test_main`) for the DV simulation environment only. The test's own targets are not changed.
"""

load("//rules:rv.bzl", "CHERIOT_PLATFORM")
load("//sw/device/tests/cheriot_both:tests.bzl", "CHERIOT_BOTH_MODES")

# The execution environment of the CHERIoT twins: the chip simulation with the test ROM.
CHERIOT_TWIN_EXEC_ENV = "//hw/top_earlgrey:sim_dv"

# `opentitan_test` arguments a twin keeps; the parameters of other environments are dropped.
_TWIN_ARGS = [
    "srcs",
    "kind",
    "deps",
    "copts",
    "defines",
    "local_defines",
    "includes",
    "linkopts",
    "data",
]

def cheriot_twin_kwargs(name, kwargs):
    """The `opentitan_test` arguments of `name`'s CHERIoT twin, or None if it has none.

    Args:
      name: the legacy test's name.
      kwargs: the legacy test's other `opentitan_test` arguments.
    Returns:
      A dict of arguments, `name` included, or None.
    """
    label = "//{}:{}".format(native.package_name(), name)
    if label not in CHERIOT_BOTH_MODES:
        return None
    exec_env = kwargs.get("exec_env", {})
    if CHERIOT_TWIN_EXEC_ENV not in exec_env:
        fail("{} is in CHERIOT_BOTH_MODES but has no {} environment".format(label, CHERIOT_TWIN_EXEC_ENV))
    if kwargs.get("platform") != None or kwargs.get("linker_script") != None:
        fail("{}: a CHERIoT twin needs the default platform and linker script".format(label))
    pname = exec_env[CHERIOT_TWIN_EXEC_ENV] or "dv"
    twin = {k: kwargs[k] for k in _TWIN_ARGS if k in kwargs}
    if pname in kwargs:
        twin[pname] = kwargs[pname]
    twin["name"] = name + "_cheriot"
    twin["exec_env"] = {CHERIOT_TWIN_EXEC_ENV: exec_env[CHERIOT_TWIN_EXEC_ENV]}
    twin["platform"] = CHERIOT_PLATFORM
    return twin
