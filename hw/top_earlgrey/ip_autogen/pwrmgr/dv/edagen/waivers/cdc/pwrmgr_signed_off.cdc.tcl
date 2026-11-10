# reason: reconvergence and crossings OpenTitan's own top-level CDC sign-off already waives for pwrmgr (hw/top_earlgrey/cdc: cdc_waivers.tcl "Intended reconvergence in pwrmgr fsm"; cdc_waivers.w_recon.tcl "multiple sources with mux" for intr_state, reg_if.rdata and wake_info; cdc_waivers.w_interface.tcl rom_ctrl_i.good), and reset crossings from a reset to one of its children (rstmgr: the life-cycle reset of a domain is asserted whenever its POR is; rst_esc_ni and rst_lc_ni are the same net, earlgrey_pd_aon.sv) owner: Henrik Pedersen <henrik.pedersen@lowrisc.org>
# Each entry: one Jasper waiver, its filter and its reason. Written 2026-11-07 from Jasper CDC on pwrmgr
# (scratch_pwrcdc57 and the detailed rerun in scratch_pwrcdc_detail on nyx).

proc pwrmgr_waive {comment args} {
  set f [check_cdc -filter -add {*}$args]
  check_cdc -waiver -add -reason SEE_COMMENTS -comment $comment -filter $f
}

# Convergence after synchronization, signed off by OpenTitan (cdc_waivers.tcl, W_RECON_GROUPS
# "Intended reconvergence in pwrmgr fsm"): the fast FSM takes each synchronized request and
# acknowledge as its own handshake step (theory_of_operation.md "Overall Sequencing").
pwrmgr_waive {OpenTitan sign-off: intended reconvergence in the pwrmgr fast FSM} \
  -tag CNV_ST_CONV -violation_key {convergence_check:u_fsm.u_state_regs.u_state_flop.q_o}
# Convergence of independent status bits into the register read and the interrupt
# (cdc_waivers.w_recon.tcl "multiple sources with mux"): each bit is its own request
# ("current wake requests"), a read may see them from different cycles.
pwrmgr_waive {OpenTitan sign-off: multiple sources with mux (intr_state)} \
  -tag CNV_ST_CONV -violation_key {convergence_check:u_reg.u_intr_state.q}
pwrmgr_waive {OpenTitan sign-off: multiple sources with mux (register read data)} \
  -tag CNV_ST_CONV -violation_key {convergence_check:u_reg.u_reg_if.rdata_q.*} -regexp
pwrmgr_waive {OpenTitan sign-off: multiple sources with mux (read data integrity, from the read data)} \
  -tag CNV_ST_CONV -violation_key {convergence_check:tl_o.d_user.data_intg}
pwrmgr_waive {OpenTitan sign-off: multiple sources with mux (wake_info)} \
  -tag CNV_ST_CONV -violation_key {convergence_check:i_wake_info.*} -regexp
# rom_ctrl_i.good into the fast FSM without a two-flop synchronizer: OpenTitan waives it
# (cdc_waivers.w_interface.tcl); rom_ctrl's done/good are multi-bit (MuBi4) values held after its
# check, and pwrmgr samples them only once done is true.
pwrmgr_waive {OpenTitan sign-off: rom_ctrl_i.good, a held MuBi4 value sampled after done} \
  -tag CDC_NO_SYNC -source_unit {rom_ctrl_i.*} -regexp
# Reset crossings from a reset to one of its children: when the source's reset asserts, the
# destination's asserts too (rstmgr reset tree), so the destination never samples a flop that is
# being reset while it runs.
pwrmgr_waive {Parent to child reset: POR (rst_ni) to the life-cycle reset (rst_lc_ni)} \
  -tag RDC_RS_DFRS -local_source_reset rst_ni -local_destination_reset rst_lc_ni
pwrmgr_waive {Same reset net on the chip: rst_esc_ni and rst_lc_ni (rst_lc_io_div4_n)} \
  -tag RDC_RS_DFRS -local_source_reset rst_esc_ni -local_destination_reset rst_lc_ni
pwrmgr_waive {Parent to child reset: AON POR (rst_slow_ni) to the main domain POR (rst_main_ni)} \
  -tag RDC_RS_DFRS -local_source_reset rst_slow_ni -local_destination_reset rst_main_ni
