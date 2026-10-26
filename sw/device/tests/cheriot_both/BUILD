# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

# The legacy chip tests built in both modes: the list, tests.bzl, which `opentitan_test`
# (rules/opentitan/defs.bzl, cheriot_both.bzl) reads. The CHERIoT twins live in the tests' own
# packages (`<package>:<test>_cheriot_sim_dv`); their packages' visibility keeps them out of a
# filegroup here.

package(default_visibility = ["//visibility:public"])

exports_files(["tests.bzl"])
