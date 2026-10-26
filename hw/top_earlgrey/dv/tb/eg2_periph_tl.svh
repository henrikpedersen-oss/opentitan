// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// EG2 per-peripheral scoreboards (included in module tb under EG2_PERIPH_SB; build mode
// en_eg2_periph): a passive TL-UL tap on each peripheral's own device port, and the pads' output
// enables and levels. eg2_periph_env (env/eg2_periph_env.sv) puts a TL monitor on each tap and
// gives its items to that peripheral's extended agent monitor, which turns the register traffic
// into the peripheral's expected items; and to the path check, which matches each core access in
// RVFI to the access at the peripheral's port. Nothing here drives the design.
//
// The TL-UL protocol of these ports is already checked in every chip build: the crossbars' binds
// (sim_tops xbar_peri_bind: uart0-3, i2c0-2, gpio, spi_device, pinmux, sysrst_ctrl, adc_ctrl,
// sensor_ctrl, pwrmgr; xbar_main_bind: spi_host0, spi_host1, usbdev) put tlul_assert on the
// crossbars' device ports, which are the same nets as the
// peripherals' tl_i/tl_o. Their assume properties check the device side's responses in
// simulation, so a second tlul_assert (EndpointType "Device") here would only duplicate them.

`define EG2_PD_MAIN tb.dut.top_earlgrey.earlgrey_pd_main
`define EG2_PD_AON  tb.dut.top_earlgrey.earlgrey_pd_aon

// One tap: a tl_if in Monitor mode on <path>.tl_i / tl_o, clocked by the block's own clock.
`define EG2_TL_TAP(inst_, path_)                                     \
  tl_if eg2_tl_``inst_ (.clk(path_.clk_i), .rst_n(path_.rst_ni));     \
  assign eg2_tl_``inst_.if_mode = dv_utils_pkg::Monitor;              \
  assign eg2_tl_``inst_.h2d = path_.tl_i;                             \
  assign eg2_tl_``inst_.d2h = path_.tl_o;

`EG2_TL_TAP(uart0,      `EG2_PD_MAIN.u_uart0)
`EG2_TL_TAP(uart1,      `EG2_PD_MAIN.u_uart1)
`EG2_TL_TAP(uart2,      `EG2_PD_MAIN.u_uart2)
`EG2_TL_TAP(uart3,      `EG2_PD_MAIN.u_uart3)
`EG2_TL_TAP(gpio,       `EG2_PD_MAIN.u_gpio)
`EG2_TL_TAP(spi_device, `EG2_PD_MAIN.u_spi_device)
`EG2_TL_TAP(i2c0,       `EG2_PD_MAIN.u_i2c0)
`EG2_TL_TAP(i2c1,       `EG2_PD_MAIN.u_i2c1)
`EG2_TL_TAP(i2c2,       `EG2_PD_MAIN.u_i2c2)
`EG2_TL_TAP(spi_host0,  `EG2_PD_MAIN.u_spi_host0)
`EG2_TL_TAP(spi_host1,  `EG2_PD_MAIN.u_spi_host1)
`EG2_TL_TAP(usbdev,     `EG2_PD_MAIN.u_usbdev)
`EG2_TL_TAP(pinmux,     `EG2_PD_AON.u_pinmux)
// The system monitor's blocks (status/decisions.md 19) and pwrmgr (its wake-up record).
`EG2_TL_TAP(sysrst_ctrl, `EG2_PD_AON.u_sysrst_ctrl)
`EG2_TL_TAP(adc_ctrl,    `EG2_PD_AON.u_adc_ctrl)
`EG2_TL_TAP(sensor_ctrl, `EG2_PD_AON.u_sensor_ctrl)
`EG2_TL_TAP(pwrmgr,      `EG2_PD_AON.u_pwrmgr)

// The MIO pads' output enables and levels, as pinmux hands them to the pad ring (the GPIO
// monitor maps them to GPIOs by the pinmux configuration it sees on the pinmux tap).
eg2_pads_if u_eg2_pads_if (.clk(`EG2_PD_MAIN.u_gpio.clk_i));
assign u_eg2_pads_if.mio_out = dut.mio_out;
assign u_eg2_pads_if.mio_oe  = dut.mio_oe;

// The system monitor's blocks at their own ports (eg2_sysmon_if in env/eg2_pads_if.sv).
eg2_sysmon_if u_eg2_sysmon_if ();
// sysrst_ctrl
assign u_eg2_sysmon_if.sysrst_clk_aon   = `EG2_PD_AON.u_sysrst_ctrl.clk_aon_i;
assign u_eg2_sysmon_if.sysrst_rst_aon_n = `EG2_PD_AON.u_sysrst_ctrl.rst_aon_ni;
assign u_eg2_sysmon_if.sysrst_rst_n     = `EG2_PD_AON.u_sysrst_ctrl.rst_ni;
assign u_eg2_sysmon_if.sysrst_in = {`EG2_PD_AON.u_sysrst_ctrl.cio_flash_wp_l_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_ec_rst_l_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_lid_open_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_ac_present_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_key2_in_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_key1_in_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_key0_in_i,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_pwrb_in_i};
assign u_eg2_sysmon_if.sysrst_out = {`EG2_PD_AON.u_sysrst_ctrl.cio_flash_wp_l_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_z3_wakeup_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_key2_out_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_key1_out_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_key0_out_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_pwrb_out_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_ec_rst_l_o,
                                     `EG2_PD_AON.u_sysrst_ctrl.cio_bat_disable_o};
assign u_eg2_sysmon_if.sysrst_oe = {`EG2_PD_AON.u_sysrst_ctrl.cio_flash_wp_l_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_z3_wakeup_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_key2_out_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_key1_out_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_key0_out_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_pwrb_out_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_ec_rst_l_en_o,
                                    `EG2_PD_AON.u_sysrst_ctrl.cio_bat_disable_en_o};
assign u_eg2_sysmon_if.sysrst_wkup_req = `EG2_PD_AON.u_sysrst_ctrl.wkup_req_o;
assign u_eg2_sysmon_if.sysrst_rst_req  = `EG2_PD_AON.u_sysrst_ctrl.rst_req_o;
assign u_eg2_sysmon_if.sysrst_intr     = `EG2_PD_AON.u_sysrst_ctrl.intr_event_detected_o;
// adc_ctrl
assign u_eg2_sysmon_if.adc_clk_aon     = `EG2_PD_AON.u_adc_ctrl.clk_aon_i;
assign u_eg2_sysmon_if.adc_rst_aon_n   = `EG2_PD_AON.u_adc_ctrl.rst_aon_ni;
assign u_eg2_sysmon_if.adc_rst_n       = `EG2_PD_AON.u_adc_ctrl.rst_ni;
assign u_eg2_sysmon_if.adc_pd          = `EG2_PD_AON.u_adc_ctrl.adc_o.pd;
assign u_eg2_sysmon_if.adc_channel_sel = `EG2_PD_AON.u_adc_ctrl.adc_o.channel_sel;
assign u_eg2_sysmon_if.adc_data        = `EG2_PD_AON.u_adc_ctrl.adc_i.data;
assign u_eg2_sysmon_if.adc_data_valid  = `EG2_PD_AON.u_adc_ctrl.adc_i.data_valid;
assign u_eg2_sysmon_if.adc_wkup_req    = `EG2_PD_AON.u_adc_ctrl.wkup_req_o;
assign u_eg2_sysmon_if.adc_intr        = `EG2_PD_AON.u_adc_ctrl.intr_match_pending_o;
// sensor_ctrl
assign u_eg2_sysmon_if.sensor_clk_aon   = `EG2_PD_AON.u_sensor_ctrl.clk_aon_i;
assign u_eg2_sysmon_if.sensor_rst_aon_n = `EG2_PD_AON.u_sensor_ctrl.rst_aon_ni;
assign u_eg2_sysmon_if.sensor_rst_n     = `EG2_PD_AON.u_sensor_ctrl.rst_ni;
for (genvar i = 0; i < 11; i++) begin : gen_eg2_sensor_alerts
  assign u_eg2_sysmon_if.sensor_alert_p[i] = `EG2_PD_AON.u_sensor_ctrl.ast_alert_i.alerts[i].p;
  assign u_eg2_sysmon_if.sensor_alert_n[i] = `EG2_PD_AON.u_sensor_ctrl.ast_alert_i.alerts[i].n;
  assign u_eg2_sysmon_if.sensor_ack_p[i]   = `EG2_PD_AON.u_sensor_ctrl.ast_alert_o.alerts_ack[i].p;
end
assign u_eg2_sysmon_if.sensor_io_pok    = `EG2_PD_AON.u_sensor_ctrl.ast_status_i.io_pok;
assign u_eg2_sysmon_if.sensor_init_done = `EG2_PD_AON.u_sensor_ctrl.ast_init_done_i;
assign u_eg2_sysmon_if.sensor_wkup_req  = `EG2_PD_AON.u_sensor_ctrl.wkup_req_o;
assign u_eg2_sysmon_if.sensor_intr = {`EG2_PD_AON.u_sensor_ctrl.intr_init_status_change_o,
                                      `EG2_PD_AON.u_sensor_ctrl.intr_io_status_change_o};
// The wake-up detectors (pinmux) and pwrmgr's wake-up inputs
assign u_eg2_sysmon_if.pinmux_clk_aon   = `EG2_PD_AON.u_pinmux.clk_aon_i;
assign u_eg2_sysmon_if.pinmux_rst_aon_n = `EG2_PD_AON.u_pinmux.rst_aon_ni;
assign u_eg2_sysmon_if.mio_in           = `EG2_PD_AON.u_pinmux.mio_in_i;
assign u_eg2_sysmon_if.dio_in           = `EG2_PD_AON.u_pinmux.dio_in_i;
assign u_eg2_sysmon_if.pin_wkup_req     = `EG2_PD_AON.u_pinmux.pin_wkup_req_o;
assign u_eg2_sysmon_if.pwrmgr_wakeups   = `EG2_PD_AON.u_pwrmgr.wakeups_i;
assign u_eg2_sysmon_if.pwrmgr_low_power = `EG2_PD_AON.u_pwrmgr.low_power_o;

`define EG2_TL_TAP_SET(inst_) \
  uvm_config_db#(virtual tl_if)::set(null, "*.env.m_eg2_periph*", `"tl_``inst_`", eg2_tl_``inst_);

initial begin
  `EG2_TL_TAP_SET(uart0)
  `EG2_TL_TAP_SET(uart1)
  `EG2_TL_TAP_SET(uart2)
  `EG2_TL_TAP_SET(uart3)
  `EG2_TL_TAP_SET(gpio)
  `EG2_TL_TAP_SET(spi_device)
  `EG2_TL_TAP_SET(i2c0)
  `EG2_TL_TAP_SET(i2c1)
  `EG2_TL_TAP_SET(i2c2)
  `EG2_TL_TAP_SET(spi_host0)
  `EG2_TL_TAP_SET(spi_host1)
  `EG2_TL_TAP_SET(usbdev)
  `EG2_TL_TAP_SET(pinmux)
  `EG2_TL_TAP_SET(sysrst_ctrl)
  `EG2_TL_TAP_SET(adc_ctrl)
  `EG2_TL_TAP_SET(sensor_ctrl)
  `EG2_TL_TAP_SET(pwrmgr)
  uvm_config_db#(virtual eg2_pads_if)::set(null, "*.env.m_eg2_periph*", "pads_vif",
                                           u_eg2_pads_if);
  uvm_config_db#(virtual eg2_sysmon_if)::set(null, "*.env.m_eg2_periph*", "sysmon_vif",
                                             u_eg2_sysmon_if);
end

`undef EG2_TL_TAP_SET
`undef EG2_TL_TAP
`undef EG2_PD_MAIN
`undef EG2_PD_AON
