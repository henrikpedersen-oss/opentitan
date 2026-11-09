# reason: dmi_cdc's two prim_fifo_async_simple (EnRzHs = 1) cross TCK and clk_i with a 4-phase req/ack handshake; declared as Jasper's handshake scheme (CDC user guide 2025.09, "HANDSHAKE": data, sreq, dreq, dack, sack) so the data path is checked as qualified by the handshake owner: Henrik Pedersen <henrik.pedersen@lowrisc.org>
check_cdc -scheme -add handshake -module prim_fifo_async_simple -map {{data data_wr_q} {sreq u_prim_sync_reqack.gen_rz_hs_protocol.src_req} {dreq u_prim_sync_reqack.gen_rz_hs_protocol.dst_req} {dack u_prim_sync_reqack.gen_rz_hs_protocol.dst_ack} {sack u_prim_sync_reqack.gen_rz_hs_protocol.src_ack}}
# The four CTL_NO_STBL reports on dmi_cdc's handshake synchronisers (req/ack of both FIFOs and the
# clear synchroniser) fail only when a DMI clear (dmihardreset, Test-Logic-Reset) or TRST arrives in
# the middle of a transfer: it resets the TCK side at once and the clk_i side a few cycles later
# through u_combined_rstn_sync, so dst_ack is cut short (Henrik's trace, 2026-11-09). With jtag_i.trst_n
# and dap.dmi_clear held static all four are proven. The RZ protocol ignores ack in LoSt and the clear
# drops the transfer by design.
# Called by tools/jasper/cdc.tcl after extraction: the filter needs the extracted violations.
proc edagen_cdc_after_extract {} {
  set f [check_cdc -filter -add -tag CTL_NO_STBL -instance {gen_jtag_gating\.dap\.i_dmi_cdc\..*} -regexp]
  check_cdc -waiver -add -filter $f -reason SEE_COMMENTS -comment "dmi_cdc: unstable only on a DMI clear or TRST mid-transfer; proven stable without it (2026-11-09)"
}
