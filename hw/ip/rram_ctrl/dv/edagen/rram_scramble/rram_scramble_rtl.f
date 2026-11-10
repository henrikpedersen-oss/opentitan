// rram_ctrl's scrambling unit (rram_scramble: GF multiplier, two PRINCE instances, arbiters)
// and what it depends on, in dependency order; the packages and primitives as rv_dm_rtl.f has them
// (FuseSoC's order), with the Earl Grey top package. ${REPO} is the root of this tree (the EG2
// fork). The wrapper rram_scramble_unit flattens its struct ports for the bench.
+incdir+${REPO}/hw/ip/prim/rtl
${REPO}/hw/ip/prim/rtl/prim_util_pkg.sv
${REPO}/hw/ip/prim/rtl/prim_mubi_pkg.sv
${REPO}/hw/ip/prim/rtl/prim_secded_pkg.sv
${REPO}/hw/ip/prim/rtl/prim_cipher_pkg.sv
${REPO}/hw/ip/prim/rtl/prim_count_pkg.sv
${REPO}/hw/ip/prim/rtl/prim_subreg_pkg.sv
${REPO}/hw/top_earlgrey/rtl/top_pkg.sv
${REPO}/hw/ip/tlul/rtl/tlul_pkg.sv
${REPO}/hw/top_earlgrey/rtl/autogen/top_racl_pkg.sv
${REPO}/hw/ip/lc_ctrl/rtl/lc_ctrl_state_pkg.sv
${REPO}/hw/ip/lc_ctrl/rtl/lc_ctrl_reg_pkg.sv
${REPO}/hw/ip/lc_ctrl/rtl/lc_ctrl_pkg.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_ctrl_reg_pkg.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_ctrl_pkg.sv
${REPO}/hw/ip/prim_generic/rtl/prim_buf.sv
${REPO}/hw/ip/prim_generic/rtl/prim_flop.sv
${REPO}/hw/ip/prim_generic/rtl/prim_flop_2sync.sv
${REPO}/hw/ip/prim/rtl/prim_sec_anchor_buf.sv
${REPO}/hw/ip/prim/rtl/prim_sec_anchor_flop.sv
${REPO}/hw/ip/prim/rtl/prim_gf_mult.sv
${REPO}/hw/ip/prim/rtl/prim_prince.sv
${REPO}/hw/ip/prim/rtl/prim_arbiter_fixed.sv
${REPO}/hw/ip/prim/rtl/prim_arbiter_tree.sv
${REPO}/hw/ip/prim/rtl/prim_arbiter_tree_dup.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_scramble.sv
${REPO}/hw/ip/rram_ctrl/dv/edagen/rram_scramble/rtl/rram_scramble_unit.sv
