# reason: toggle bits that are constant in Earl Grey 2 (tied off in earlgrey_pd_main.sv, or fixed by the TL-UL rules) owner: Henrik Pedersen <henrik.pedersen@lowrisc.org>
# OpenTitan's common_cov_excl.tcl excludes the TL-UL constants with *tl_i / *tl_o patterns, which do
# not match rv_dm's port names (IMC, 2026-11-07: "No matching entities"). 416 bits, 2026-11-09 (toggle 53.0 % -> 71.6 % on the blockall49 merge).

# earlgrey_pd_main.sv: .dbg_tl_d_i(tlul_pkg::TL_H2D_DEFAULT), .dbg_tl_d_o() (UseDmiInterface 0).
edagen_waive exclude -inst rv_dm -toggle 'dbg_tl_d_i*' -comment "\[UNR\] EG2 ties the DMI-over-TL port off (earlgrey_pd_main.sv)"
edagen_waive exclude -inst rv_dm -toggle 'dbg_tl_d_o*' -comment "\[UNR\] EG2 leaves the DMI-over-TL response open; its input is tied off"
# earlgrey_pd_main.sv: .racl_policies_i(RACL_POLICY_VEC_DEFAULT), .racl_error_o(); EnableRacl 0.
edagen_waive exclude -inst rv_dm -toggle 'racl_policies_i*' -comment "\[UNR\] EG2 ties the RACL policies to their default (EnableRacl 0)"
edagen_waive exclude -inst rv_dm -toggle 'racl_error_o*' -comment "\[UNR\] EG2 leaves racl_error_o unconnected (RACL off); its address bits follow the bus address but nothing reads them"
# earlgrey_pd_main.sv: .next_dm_addr_i('0): the only debug module.
edagen_waive exclude -inst rv_dm -toggle 'next_dm_addr_i*' -comment "\[UNR\] EG2 ties next_dm_addr_i to 0 (one debug module)"

# TL-UL constants (Comportability: a_user.rsvd and a_param unused, d_param and d_sink 0).
foreach port {regs_tl_d_i mem_tl_d_i} {
  edagen_waive exclude -inst rv_dm -toggle "'${port}.a_user.rsvd*'" -comment "\[UNSUPPORTED\] Comportability spec: reserved user bits"
  edagen_waive exclude -inst rv_dm -toggle "'${port}.a_param*'" -comment "\[UNSUPPORTED\] Comportability spec: a_param unused"
}
foreach port {regs_tl_d_o mem_tl_d_o} {
  edagen_waive exclude -inst rv_dm -toggle "'${port}.d_param*'" -comment "\[UNR\] Follows a_param, which is unused"
  edagen_waive exclude -inst rv_dm -toggle "'${port}.d_sink*'" -comment "\[UNR\] Comportability spec: d_sink 0"
}
# The system-bus host port: rv_dm drives a fixed source ID and no a_param or reserved user bits; the
# answers it gets carry d_param and d_sink 0 and echo the fixed source ID.
edagen_waive exclude -inst rv_dm -toggle 'sba_tl_h_o.a_user.rsvd*' -comment "\[UNSUPPORTED\] Comportability spec: reserved user bits"
edagen_waive exclude -inst rv_dm -toggle 'sba_tl_h_o.a_param*' -comment "\[UNSUPPORTED\] Comportability spec: a_param unused"
edagen_waive exclude -inst rv_dm -toggle 'sba_tl_h_o.a_source*' -comment "\[UNR\] rv_dm's system-bus host uses one fixed source ID"
edagen_waive exclude -inst rv_dm -toggle 'sba_tl_h_i.d_param*' -comment "\[UNR\] Follows a_param, which is unused"
edagen_waive exclude -inst rv_dm -toggle 'sba_tl_h_i.d_sink*' -comment "\[UNR\] Comportability spec: d_sink 0"
edagen_waive exclude -inst rv_dm -toggle 'sba_tl_h_i.d_source*' -comment "\[UNR\] Echoes the fixed source ID"
