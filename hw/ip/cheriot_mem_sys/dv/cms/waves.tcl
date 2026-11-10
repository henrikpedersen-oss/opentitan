# SimVision: probe the DUT ports and the testbench's hosts and memories.
database -open waves -shm -default
probe -create cms_tb -depth 2 -all -database waves
probe -create cms_tb.u_dut -depth all -all -database waves
run
