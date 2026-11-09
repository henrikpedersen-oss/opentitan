# reason: rv_dm's internal reset nets are deliberately NOT declared to Jasper CDC owner: Henrik Pedersen <henrik.pedersen@lowrisc.org>
# 2026-11-09: declaring gen_jtag_gating.trst_n_muxed (config_rtlds -reset -async) and
# gen_jtag_gating.dap.i_dmi_cdc.combined_rstn (-synchronized to clk_i) made Jasper put a stopat on
# them (WSA013), cutting the real reset off: the whole TCK side and both dmi_cdc handshakes came out
# of reset analysis as X (get_reset_info -x_value: 42 registers) and every handshake check failed in
# 3-8 cycles from that random state. TRST is declared at the port instead (formal.resets,
# jtag_i.trst_n), and reaches those flops through the pinmux gate and the scan mux. The resulting
# RST_NO_DECL / FLP_RS_UDCL reports for these internal nets are read as such.
