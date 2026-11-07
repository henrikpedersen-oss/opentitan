-- Copyright lowRISC Contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Directed test for the debug host: the debug module's system bus access (SBA) on the RTOS test
-- SoC, driven by the testbench (tb/cheriot_rtos_dbg_host.sv) on this firmware's request. Testplan
-- non_core_bus_masters: bus_master_dbg_host_tag_clear and bus_master_dbg_host_locked. See
-- dbg_host_test.cc.

compartment("dbg_host_test")
    add_files("dbg_host_test.cc")

firmware("dbg_host_test_fw")
    -- debug: ConditionalDebug<true, ...> (see revocation_test/xmake.lua).
    add_deps("freestanding", "debug", "dbg_host_test")
    on_load(function(target)
        target:values_set("board", "$(board)")
        target:values_set("threads", {
            {
                compartment          = "dbg_host_test",
                priority             = 1,
                entry_point          = "run_dbg_host_tests",
                stack_size           = 0x1000,
                trusted_stack_frames = 4
            }
        }, {expand = false})
    end)
