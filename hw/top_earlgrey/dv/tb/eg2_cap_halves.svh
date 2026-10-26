// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_cheriot_cap_halves's assertion (eg2_cap_halves_chk.sv) on cheriot_mem_sys's core port
// (cored_tl_d, the hint on cored_tag_h2d) and on its TBRE port (tbre_tl_h). Included in tb.sv;
// checks only with +eg2_cap_halves=1, so no other run changes. The test's vseq reads the counts
// (eg2_ch_core.caps, .fails; eg2_ch_tbre.caps, .fails).

bit eg2_ch_on, eg2_ch_selftest;
initial begin
  eg2_ch_on = $test$plusargs("eg2_cap_halves");
  eg2_ch_selftest = $test$plusargs("eg2_cap_halves_selftest");
end

eg2_cap_halves_chk #(.Name("cheriot_mem_sys core")) eg2_ch_core (
  .clk_i   (`PD_MAIN_HIER.u_cheriot_mem_sys.clk_i),
  .rst_ni  (`PD_MAIN_HIER.u_cheriot_mem_sys.rst_ni),
  .h2d     (`PD_MAIN_HIER.u_cheriot_mem_sys.cored_tl_d_i),
  .d2h     (`PD_MAIN_HIER.u_cheriot_mem_sys.cored_tl_d_o),
  .hint    (`PD_MAIN_HIER.u_cheriot_mem_sys.cored_tag_h2d_i),
  .use_hint(1'b1),
  .on      (eg2_ch_on),
  .selftest(eg2_ch_selftest)
);

eg2_cap_halves_chk #(.Name("cheriot_mem_sys TBRE")) eg2_ch_tbre (
  .clk_i   (`PD_MAIN_HIER.u_cheriot_mem_sys.clk_i),
  .rst_ni  (`PD_MAIN_HIER.u_cheriot_mem_sys.rst_ni),
  .h2d     (`PD_MAIN_HIER.u_cheriot_mem_sys.tbre_tl_h_o),
  .d2h     (`PD_MAIN_HIER.u_cheriot_mem_sys.tbre_tl_h_i),
  .hint    (1'b1),
  .use_hint(1'b0),
  .on      (eg2_ch_on),
  .selftest(1'b0)
);
