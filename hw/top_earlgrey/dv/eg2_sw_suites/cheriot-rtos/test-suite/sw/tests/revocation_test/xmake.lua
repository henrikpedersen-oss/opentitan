-- Copyright lowRISC Contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Directed test for the CHERIoT hardware load barrier (ibex_trvk).
--
-- Unlike juliet_cwe, this is a single compartment that does not fault: it
-- drives the shadow bitmap over MMIO and checks the tag of a reloaded
-- capability in both directions. See revocation_test.cc for why the CWE
-- use-after-free tests are not sufficient on their own.

compartment("revocation_test")
    add_files("revocation_test.cc")

firmware("revocation_test_fw")
    -- debug resolves __library_export_libcalls_..debug_log_message_write..,
    -- pulled in by ConditionalDebug<true, ...>; without it the compartment
    -- compiles and only the final firmware link fails.
    add_deps("freestanding", "debug", "revocation_test")
    on_load(function(target)
        target:values_set("board", "$(board)")
        -- Two threads: the tests, and the thread test_revocation_preempt preempts while it holds
        -- capabilities in registers. That one waits on a futex except during that test, and
        -- has the lower priority, so it runs only while the tests sleep and the tests' wake-up
        -- preempts it.
        target:values_set("threads", {
            {
                compartment          = "revocation_test",
                priority             = 2,
                entry_point          = "run_revocation_tests",
                stack_size           = 0x1000,
                trusted_stack_frames = 4
            },
            {
                compartment          = "revocation_test",
                priority             = 1,
                entry_point          = "revocation_preempt_thread",
                stack_size           = 0x400,
                trusted_stack_frames = 2
            }
        }, {expand = false})
    end)
