// CHERIoT memory subsystem bench: the RTL it elaborates, from this tree (paths relative to the
// repository root). The packages in compile order, then the library directories (-y) for Verilator;
// cms_xlm.f (make filelist) lists every file explicitly for Xcelium.
+incdir+hw/ip/prim/rtl
hw/ip/prim/rtl/prim_util_pkg.sv
hw/ip/prim/rtl/prim_mubi_pkg.sv
hw/ip/prim/rtl/prim_secded_pkg.sv
hw/ip/prim/rtl/prim_alert_pkg.sv
hw/ip/prim/rtl/prim_subreg_pkg.sv
hw/ip/prim/rtl/prim_count_pkg.sv
hw/ip/prim_generic/rtl/prim_pkg.sv
hw/top_earlgrey/rtl/top_pkg.sv
hw/ip/tlul/rtl/tlul_pkg.sv
hw/vendor/lowrisc_ibex/rtl/ibex_pkg.sv
hw/vendor/lowrisc_ibex/rtl/ibex_cheriot_pkg.sv
hw/ip/cheriot_mem_sys/rtl/cheriot_mem_sys_reg_pkg.sv
-y hw/ip/prim/rtl
-y hw/ip/prim_generic/rtl
-y hw/vendor/pulp_common_cells/rtl
-y hw/ip/tlul/rtl
-y hw/ip/cheriot_mem_sys/rtl
hw/ip/cheriot_mem_sys/rtl/cheriot_mem_sys.sv
