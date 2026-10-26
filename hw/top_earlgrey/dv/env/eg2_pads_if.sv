// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The MIO pads' output enables and levels as pinmux drives them into the pad ring (EG2 GPIO
// monitor, eg2_periph_monitors.sv). Connected by tb/eg2_periph_tl.svh in EG2_PERIPH_SB builds.
//
// eg2_sysmon_if: the interfaces of the system monitor's blocks (status/decisions.md 19: a monitor
// and a scoreboard per block, eg2_sysmon_monitors.sv), each as its documentation lists it
// (hw/ip/<ip>/doc/interfaces.md): the block's own pins and inter-module signals, and its always-on
// clock (the timers count it). Inputs are what the bench and the AST drive into the block; outputs
// are what the block drives. Nothing here drives the design.

`ifdef EG2_PERIPH_SB
interface eg2_pads_if (input logic clk);
  logic [pinmux_reg_pkg::NMioPads-1:0] mio_out;
  logic [pinmux_reg_pkg::NMioPads-1:0] mio_oe;
endinterface

interface eg2_sysmon_if;
  // sysrst_ctrl (hw/ip/sysrst_ctrl/doc/interfaces.md). Inputs, bit order:
  // 0 pwrb_in, 1 key0_in, 2 key1_in, 3 key2_in, 4 ac_present, 5 lid_open, 6 ec_rst_l, 7 flash_wp_l.
  // Outputs, bit order (PIN_OUT_CTL's): 0 bat_disable, 1 ec_rst_l, 2 pwrb_out, 3 key0_out,
  // 4 key1_out, 5 key2_out, 6 z3_wakeup, 7 flash_wp_l.
  logic       sysrst_clk_aon;
  logic       sysrst_rst_aon_n;
  logic       sysrst_rst_n;
  logic [7:0] sysrst_in;
  logic [7:0] sysrst_out;
  logic [7:0] sysrst_oe;
  logic       sysrst_wkup_req;
  logic       sysrst_rst_req;
  logic       sysrst_intr;

  // adc_ctrl (hw/ip/adc_ctrl/doc/theory_of_operation.md, "Signals"): adc_o to the AST ADC,
  // adc_i from it.
  logic       adc_clk_aon;
  logic       adc_rst_aon_n;
  logic       adc_rst_n;
  logic       adc_pd;
  logic [1:0] adc_channel_sel;
  logic [9:0] adc_data;
  logic       adc_data_valid;
  logic       adc_wkup_req;
  logic       adc_intr;

  // sensor_ctrl (hw/top_earlgrey/ip/sensor_ctrl/doc/interfaces.md): the AST's alert events (a
  // differential pair each), its acknowledgements, the IO power status and init done.
  logic        sensor_clk_aon;
  logic        sensor_rst_aon_n;
  logic        sensor_rst_n;
  logic [10:0] sensor_alert_p;
  logic [10:0] sensor_alert_n;
  logic [10:0] sensor_ack_p;
  logic [1:0]  sensor_io_pok;
  logic [3:0]  sensor_init_done;
  logic        sensor_wkup_req;
  logic [1:0]  sensor_intr;

  // The wake-up detectors of pinmux (hw/top_earlgrey/ip_autogen/pinmux/doc/theory_of_operation.md
  // "Wakeup Detectors") on the pads pinmux receives, and pwrmgr's wake-up inputs
  // (ip_autogen/pwrmgr: 0 sysrst_ctrl, 1 adc_ctrl, 2 pinmux, 3 USB, 4 aon_timer, 5 sensor_ctrl).
  logic                                pinmux_clk_aon;
  logic                                pinmux_rst_aon_n;
  logic [pinmux_reg_pkg::NMioPads-1:0] mio_in;
  logic [pinmux_reg_pkg::NDioPads-1:0] dio_in;
  logic                                pin_wkup_req;
  logic [5:0]                          pwrmgr_wakeups;
  logic                                pwrmgr_low_power;
endinterface
`endif
