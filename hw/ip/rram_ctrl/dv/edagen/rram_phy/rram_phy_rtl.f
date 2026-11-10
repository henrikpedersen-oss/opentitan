// rram_ctrl's PHY unit (rram_phy with rram_phy_rd, rram_phy_rd_buffer, rram_phy_rd_buf_dep,
// rram_phy_wr and the shared rram_scramble) and what it depends on, in dependency order; the
// packages and primitives as rram_ctrl_mp_rtl.f has them (FuseSoC's order), with the Earl Grey top
// package. ${REPO} is the root of this tree (the EG2 fork). The wrapper rram_phy_unit flattens the
// macro interface structs for the bench.
+incdir+${REPO}/hw/ip/prim/rtl
// prim_assert.sv first: rram_phy_rd.sv, rram_phy_rd_buffer.sv and rram_phy_rd_buf_dep.sv use `ASSERT
// without including it (the other tools get the macros from an earlier file; Yosys/slang did not).
${REPO}/hw/ip/prim/rtl/prim_assert.sv
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
${REPO}/hw/ip/prim/rtl/prim_sparse_fsm_flop.sv
${REPO}/hw/ip/prim_generic/rtl/prim_xor2.sv
${REPO}/hw/ip/prim/rtl/prim_mubi4_sync.sv
${REPO}/hw/ip/prim/rtl/prim_count.sv
${REPO}/hw/ip/prim/rtl/prim_fifo_sync_cnt.sv
${REPO}/hw/ip/prim/rtl/prim_fifo_sync.sv
${REPO}/hw/ip/prim/rtl/prim_secded_inv_39_32_enc.sv
${REPO}/hw/ip/prim/rtl/prim_secded_inv_39_32_dec.sv
${REPO}/hw/ip/tlul/rtl/tlul_data_integ_enc.sv
${REPO}/hw/ip/tlul/rtl/tlul_data_integ_dec.sv
${REPO}/hw/ip/prim/rtl/prim_gf_mult.sv
${REPO}/hw/ip/prim/rtl/prim_prince.sv
${REPO}/hw/ip/prim/rtl/prim_arbiter_fixed.sv
${REPO}/hw/ip/prim/rtl/prim_arbiter_tree.sv
${REPO}/hw/ip/prim/rtl/prim_arbiter_tree_dup.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_scramble.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_phy_rd_buffer.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_phy_rd_buf_dep.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_phy_rd.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_phy_wr.sv
${REPO}/hw/ip/rram_ctrl/rtl/rram_phy.sv
${REPO}/hw/ip/rram_ctrl/dv/edagen/rram_phy/rtl/rram_phy_unit.sv
