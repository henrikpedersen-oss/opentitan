-- Copyright lowRISC Contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Performance test: Earl Grey 2's ROM_EXT hash feed (128 KiB from RRAM into the hash engine's
-- message FIFO), by the core and by a DMA. Runs on the test SoC's perf device
-- (rtl/cheriot_rtos_perf_dev.sv), which the board patch adds to sonata-1.1.

-- The board patch, by path: board files are found relative to this script.
local perf_board = path.join(os.scriptdir(), "perf-board.patch")

compartment("perf_test")
    add_files("perf_test.cc")
    -- The 128 KiB image is computed at compile time.
    add_cxflags("-fconstexpr-steps=4000000")

firmware("perf_test_fw")
    add_deps("freestanding", "debug", "perf_test")
    on_load(function(target)
        target:values_set("board", perf_board)
        target:values_set("threads", {
            {
                compartment          = "perf_test",
                priority             = 1,
                entry_point          = "run_perf_tests",
                stack_size           = 0x1000,
                trusted_stack_frames = 4
            }
        }, {expand = false})
    end)
