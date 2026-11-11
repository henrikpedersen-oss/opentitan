# reason: prim_fifo_sync's data storage has no reset by design (OpenTitan prim, not cheriot_mem_sys); the FIFO empties on the same reset, so the storage is never read before it is written owner: Henrik Pedersen <henrik.pedersen@lowrisc.org>
# Written 2026-11-11 from Jasper CDC/RDC on cheriot_mem_sys (edagen RTL flow on nyx,
# ~/work/eg2-claude-1111/edagen-out/out/jasper/cdc_violations.txt), the one error of the run:
#   RDC_RS_DFRS: u_cheriot_tag_filter.u_prim_fifo_sync_meta_rsp.gen_normal_fifo.u_fifo_cnt.rptr_wrap_cnt_q
#   (reset by rst_ni) -> ...gen_normal_fifo.storage[0] (no reset), both on clk_i.
# Not a reset synchronizer finding: rst_ni comes synchronized from the reset manager and is declared
# so (config_rtlds -reset -synchronized rst_ni -clock clk_i). The crossing is the FIFO's read pointer
# selecting its storage (prim_fifo_sync.sv: storage is written in an always_ff with no reset,
# read as storage[fifo_rptr]). Reset empties the FIFO (pointers reset, rvalid_o low), and an entry is
# only read after it has been written, so the unreset contents are never presented. OpenTitan's top
# sign-off waives the same storage flops of other FIFOs (hw/top_earlgrey/cdc/cdc_waivers.w_data.tcl,
# w_masync.tcl: "*.gen_normal_fifo.storage*", "*.rspfifo.storage*").
# Every prim_fifo_sync of the block has this crossing; Jasper shows them one at a time (each waived
# one uncovers the next): the tag filter's metadata response FIFO (from its read pointer), the TBRE
# tag filter's align FIFO, the WTRC's expectation FIFO (from w0_ok_q, the data written into it).
# One reason covers them all: the source flops and the FIFO's own pointers are reset by the same
# rst_ni, so when it asserts the FIFO empties; whatever the storage captured meanwhile is never read.
# The filter: this rule with a FIFO's storage as the destination. Any other reset-domain crossing
# still reports and is read before it is waived. Not the violation key: Jasper matches it as a
# glob, so "storage[0]" never matched (debug runs 2026-11-11).
# Called by tools/jasper/cdc.tcl after extraction: the filter needs the extracted violations.
proc edagen_cdc_after_extract {} {
  set f [check_cdc -filter -add -tag RDC_RS_DFRS -destination_unit {*.gen_normal_fifo.storage*}]
  check_cdc -waiver -add -filter $f -reason SEE_COMMENTS \
    -comment "prim_fifo_sync storage: no reset by design; its FIFO empties on the same reset, so it is never read (OpenTitan prim)"
}
