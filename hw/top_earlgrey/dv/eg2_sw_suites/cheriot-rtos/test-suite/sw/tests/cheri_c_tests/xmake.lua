-- Copyright lowRISC Contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Cambridge CHERI C Test Suite for CHERIoT RTOS
--
-- Each Cambridge purecap test is wrapped in its own compartment.  Normal tests
-- (DECLARE_TEST) return int: 0 = PASS.  Fault tests (DECLARE_TEST_FAULT) are
-- expected to ForceUnwind; the orchestrator verifies the return capability is
-- invalid (address == -1) and that the fault was the expected one, with no
-- failed check before it (../common/expected_fault.hh).
--
-- Include paths required by every compartment that includes an original .c:
--   "."                    → cheri_c_test_framework.h (angle-bracket include)
--   cheri_c_tests_dir      → cheri_c_test.h and clang-purecap/*.c (the superproject's cheri-c-tests
--                            submodule; set in ../../xmake.lua, override with CHERI_C_TESTS_DIR)

-- ── Helper: add include paths needed by compartments that wrap .c files ──────
local function add_cheri_c_incdirs()
    add_includedirs(".", cheri_c_tests_dir)
end

-- ── Normal test compartments (DECLARE_TEST) ──────────────────────────────────

compartment("cc_init")
    add_files("test_clang_purecap_init.cc")
    add_cheri_c_incdirs()

compartment("cc_capcmp")
    add_files("test_clang_purecap_capcmp.cc")
    add_cheri_c_incdirs()

compartment("cc_atomic")
    add_files("test_clang_purecap_atomic.cc")
    add_cheri_c_incdirs()

compartment("cc_capretaddr")
    add_files("test_clang_purecap_capretaddr.cc")
    add_cheri_c_incdirs()
    -- The test checks that foo()'s return address lies inside testfn(), which assumes testfn
    -- really calls foo. Optimised, testfn is a tail call (`j foo`), foo returns to testfn's
    -- caller -- placed before testfn -- and both offset checks fail against a correct return
    -- capability (2026-09-29). Keep the call a call.
    add_cxflags("-fno-optimize-sibling-calls", {force = true})

compartment("cc_funptr")
    add_files("test_clang_purecap_funptr.cc")
    add_cheri_c_incdirs()

compartment("cc_intcap")
    add_files("test_clang_purecap_intcap.cc")
    add_cheri_c_incdirs()

compartment("cc_null")
    add_files("test_clang_purecap_null.cc")
    add_cheri_c_incdirs()

compartment("cc_smallint")
    add_files("test_clang_purecap_smallint.cc")
    add_cheri_c_incdirs()

compartment("cc_union")
    add_files("test_clang_purecap_union.cc")
    add_cheri_c_incdirs()

compartment("cc_union_struct")
    add_files("test_clang_purecap_union_struct.cc")
    add_cheri_c_incdirs()

compartment("cc_byval_args")
    add_files("test_clang_purecap_byval_args.cc")
    add_cheri_c_incdirs()

compartment("cc_intcapmath")
    add_files("test_clang_purecap_intcapmath.cc")
    add_cheri_c_incdirs()

compartment("cc_uintcapmath")
    add_files("test_clang_purecap_uintcapmath.cc")
    add_cheri_c_incdirs()

compartment("cc_va_args")
    add_files("test_clang_purecap_va_args.cc")
    add_cheri_c_incdirs()

compartment("cc_va_copy")
    add_files("test_clang_purecap_va_copy.cc")
    add_cheri_c_incdirs()

compartment("cc_capret")
    add_files("test_clang_purecap_capret.cc")
    add_cheri_c_incdirs()

compartment("cc_stack_cap")
    add_files("test_clang_purecap_stack_cap.cc")
    add_cheri_c_incdirs()

compartment("cc_int64math")
    add_files("test_clang_purecap_int64math.cc")
    add_cheri_c_incdirs()

compartment("cc_uint64math")
    add_files("test_clang_purecap_uint64math.cc")
    add_cheri_c_incdirs()

-- The capability-width atomics cc_atomic needs and the RTOS does not have (atomic_cap.cc).
library("cc_atomic_cap")
    add_files("atomic_cap.cc")

-- ── libc test compartments (cheri-c-tests libc/, DECLARE_TEST) ───────────────
-- Not built: libc_qsort (the RTOS has no qsort) and libc_string (no strcpy in the RTOS's string
-- library). setjmp and malloc are from CHERIoT ports (cheriot_libc_*.c; the changes in their
-- headers).

compartment("cc_libc_printf")
    add_files("test_libc_printf.cc")
    add_cheri_c_incdirs()

compartment("cc_libc_malloc")
    add_files("test_libc_malloc.cc")
    add_cheri_c_incdirs()

compartment("cc_libc_memcpy")
    add_files("test_libc_memcpy.cc")
    add_cheri_c_incdirs()

compartment("cc_libc_memmove")
    add_files("test_libc_memmove.cc")
    add_cheri_c_incdirs()

compartment("cc_libc_setjmp")
    add_files("test_libc_setjmp.cc")
    add_cheri_c_incdirs()

-- ── Fault test compartments (DECLARE_TEST_FAULT) ─────────────────────────────

compartment("cc_array")
    add_files("test_clang_purecap_array.cc")
    add_cheri_c_incdirs()

compartment("cc_badcall")
    add_files("test_clang_purecap_badcall.cc")
    add_cheri_c_incdirs()

-- noexec_call has no upstream file: the execute-permission case of upstream badcall, which on
-- CHERIoT never runs (test_clang_purecap_noexec_call.cc).
compartment("cc_noexec_call")
    add_files("test_clang_purecap_noexec_call.cc")

compartment("cc_va_list_global")
    add_files("test_clang_purecap_va_list_global.cc")
    add_cheri_c_incdirs()

-- input/output use standalone .cc (no original .c include — FreeBSD qualifiers)
compartment("cc_input")
    add_files("test_clang_purecap_input.cc")

compartment("cc_output")
    add_files("test_clang_purecap_output.cc")

-- ── Orchestrator compartment ──────────────────────────────────────────────────

compartment("cc_orchestrator")
    add_files("cheri_c_orchestrator.cc")

-- ── Firmware image ───────────────────────────────────────────────────────────

firmware("cheri_c_tests")
    -- freestanding alone is not enough; these resolve
    -- __library_export_libcalls_* undefined symbols at the final firmware link:
    --   debug  -> debug_log_message_write, pulled in by every wrapper's
    --             ConditionalDebug<true, ...> (referenced from 15 compartments)
    --   string -> strcmp from cc_funptr
    -- Same shape as the known-good firmware in cheriot-rtos/tests/xmake.lua, which
    -- lists freestanding/string/atomic_fixed/debug. atomic is kept although the one
    -- compartment that named __atomic_* directly (cc_atomic) is now excluded below:
    -- it costs nothing and guards against an indirect user via locks. The aggregate
    -- target pulls atomic_fixed -> atomic1/2/4/8, so the individual widths need not
    -- be named.
    add_deps("freestanding",
             "debug",
             "atomic",
             "string",
             "stdio",
             "crt",
             "cc_init",
             "cc_capcmp",
             -- cc_atomic needs __atomic_{load,store,exchange,compare_exchange}_cap, which
             -- CHERIoT RTOS does not implement: cc_atomic_cap (atomic_cap.cc) has them.
             "cc_atomic_cap",
             "cc_atomic",
             "cc_capretaddr",
             "cc_funptr",
             "cc_intcap",
             "cc_null",
             "cc_smallint",
             "cc_union",
             "cc_union_struct",
             "cc_byval_args",
             "cc_intcapmath",
             "cc_uintcapmath",
             "cc_va_args",
             "cc_va_copy",
             "cc_capret",
             "cc_stack_cap",
             "cc_int64math",
             "cc_uint64math",
             "cc_libc_printf",
             "cc_libc_malloc",
             "cc_libc_memcpy",
             "cc_libc_memmove",
             "cc_libc_setjmp",
             -- clang_purecap_va_die is not ported: it needs va_arg past the last argument
             -- to fault, and a CHERIoT va_list is not bounded to the arguments. See
             -- the matching comment in cheri_c_orchestrator.cc.
             "cc_array",
             "cc_badcall",
             "cc_noexec_call",
             "cc_va_list_global",
             "cc_input",
             "cc_output",
             "cc_orchestrator")
    on_load(function(target)
        target:values_set("board", "$(board)")
        target:values_set("threads", {
            {
                compartment          = "cc_orchestrator",
                priority             = 1,
                entry_point          = "run_cheri_c_tests",
                -- cc_stack_cap puts about 5.7 KiB of arrays and frame on this stack, which
                -- every compartment call from the thread runs on; the other tests fit in
                -- 0x1000. The RTOS build rejects stacks over 8176 bytes
                -- (cheriot-rtos/sdk/xmake.lua stack_size_limit), and lld rejects 8176
                -- itself ("Thread 0has invalid stack"); 0x1c00 links.
                stack_size           = 0x1c00,
                trusted_stack_frames = 20
            }
        }, {expand = false})
    end)
