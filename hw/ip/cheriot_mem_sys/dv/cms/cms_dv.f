// CHERIoT memory subsystem block-level bench (cms): file list for Verilator (lint). Paths are
// relative to the repository root, which the Makefile runs from.
//
// The RTL is this tree's (cms_rtl.f); library modules are found with -y. Xcelium uses the explicit
// list cms_xlm.f instead, generated from this one (make filelist).
-f hw/ip/cheriot_mem_sys/dv/cms/cms_rtl.f
+incdir+hw/dv/sv/dv_utils
+incdir+hw/ip/cheriot_mem_sys/dv/cms/tb

// Testbench
// The C reference model (DPI; xrun compiles cms_dpi.cc, cms_xlm_build.sh)
hw/ip/cheriot_mem_sys/dv/cms/cmodel/cms_cmodel_pkg.sv
hw/ip/cheriot_mem_sys/dv/cms/tb/cms_pkg.sv
hw/ip/cheriot_mem_sys/dv/cms/tb/cms_tl_host.sv
hw/ip/cheriot_mem_sys/dv/cms/tb/cms_tl_mem.sv
// Functional coverage of the RTL internals: the RTOS SoC's covergroups, bound here as well
hw/ip/cheriot_mem_sys/dv/cms/cov/core_ibex_trvk_fcov_if.sv
hw/ip/cheriot_mem_sys/dv/cms/cov/cheriot_mem_subsys_fcov.sv
hw/ip/cheriot_mem_sys/dv/cms/tb/cms_fcov_bind.sv
hw/ip/cheriot_mem_sys/dv/cms/tb/cms_tb.sv
