// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of the EG2 per-peripheral scoreboards (compiled with EG2_PERIPH_SB only).
// Every covergroup is per instance (option.per_instance, named by the instance's path): coverage is
// read per instance, never per type. Each is sampled from what the extended monitors and the
// scoreboards see (the register traffic of the peripheral's TL-UL port, the agents' pin items), so
// every bin is one the bench can observe. The coverpoints follow the spec's coverage column of the
// peripheral tests (eg2_periph_cg: device, transfer length class, baud or clock class) and the IPs'
// own testplans (hw/ip/<ip>/data/<ip>_testplan.hjson covergroups).

// UART: the configuration a transfer runs with (uart.hjson CTRL), sampled at each CTRL write that
// enables TX or RX. The NCO bins are baud classes at the 24 MHz io_div4 clock (NCO = baud * 2^20 /
// f): up to 115200, up to 1 Mbaud, 1 Mbaud, between, 1.5 Mbaud (0xffff).
covergroup eg2_uart_cfg_cg(string name) with function sample(bit tx, bit rx, bit [15:0] nco,
                                                             bit parity_en, bit parity_odd,
                                                             bit slpbk, bit llpbk);
  option.per_instance = 1;
  option.name = name;
  cp_dir: coverpoint {tx, rx} {
    bins tx_only = {2'b10};
    bins rx_only = {2'b01};
    bins both    = {2'b11};
  }
  cp_baud: coverpoint nco {
    bins up_to_115200  = {[1 : 5033]};
    bins below_1m      = {[5034 : 43689]};
    bins baud_1m       = {[43690 : 43691]};
    bins below_1m5     = {[43692 : 65534]};
    bins baud_1m5      = {16'hffff};
  }
  cp_parity: coverpoint {parity_en, parity_odd} {
    bins none = {2'b00, 2'b01};
    bins even = {2'b10};
    bins odd  = {2'b11};
  }
  cp_loopback: coverpoint {slpbk, llpbk} {
    bins none   = {2'b00};
    bins system = {2'b10};
    bins line   = {2'b01};
  }
  cr_baud_parity: cross cp_baud, cp_parity;
endgroup

// UART FIFOs: the RX FIFO level when a byte arrives (the extended monitor's model of the 64-byte
// FIFO), a byte lost because it was full, and the FIFO resets.
covergroup eg2_uart_fifo_cg(string name) with function sample(int unsigned kind, int unsigned level);
  option.per_instance = 1;
  option.name = name;
  // 0 an RX byte taken, 1 an RX byte lost (overflow), 2 RXRST, 3 TXRST
  cp_kind: coverpoint kind {
    bins rx_byte  = {0};
    bins overflow = {1};
    bins rxrst    = {2};
    bins txrst    = {3};
  }
  cp_rx_level: coverpoint level iff (kind == 0) {
    bins empty   = {0};
    bins one     = {1};
    bins few     = {[2 : 7]};
    bins half    = {[8 : 31]};
    bins high    = {[32 : 62]};
    bins full    = {[63 : 64]};
  }
endgroup

// UART TX: how many bytes the core had written and the chip had not sent yet when it wrote one
// more (the scoreboard's pending expected items: TX FIFO and the byte on the wire).
covergroup eg2_uart_tx_cg(string name) with function sample(int unsigned pending);
  option.per_instance = 1;
  option.name = name;
  cp_pending: coverpoint pending {
    bins idle      = {0};
    bins one       = {1};
    bins few       = {[2 : 7]};
    bins half      = {[8 : 23]};
    bins near_full = {[24 : 32]};
    bins full      = {[33 : $]};
  }
endgroup

// SPI host: each COMMAND segment (spi_host.hjson COMMAND, CONFIGOPTS).
covergroup eg2_spi_seg_cg(string name) with function sample(bit cpol, bit cpha, bit [1:0] speed,
                                                            bit csaat, bit [1:0] dir,
                                                            int unsigned len);
  option.per_instance = 1;
  option.name = name;
  cp_mode: coverpoint {cpol, cpha};
  cp_speed: coverpoint speed {
    bins standard = {0};
    bins dual     = {1};
    bins quad     = {2};
  }
  cp_csaat: coverpoint csaat;
  cp_dir: coverpoint dir {
    bins dummy = {0};
    bins rx    = {1};
    bins tx    = {2};
    bins bidir = {3};
  }
  cp_len: coverpoint len {
    bins one    = {1};
    bins word   = {[2 : 4]};
    bins short_len = {[5 : 16]};
    bins medium_len = {[17 : 64]};
    bins long_len  = {[65 : $]};
  }
  cr_dir_len: cross cp_dir, cp_len;
  cr_dir_csaat: cross cp_dir, cp_csaat;
endgroup

// I2C: each transfer the agent saw (i2c_testplan: host and target, ACK and NACK).
covergroup eg2_i2c_xfer_cg(string name) with function sample(bit chip_target, bit read,
                                                             int unsigned ack, int unsigned len);
  option.per_instance = 1;
  option.name = name;
  cp_role: coverpoint chip_target {
    bins chip_controller = {0};
    bins chip_target     = {1};
  }
  cp_dir: coverpoint read {
    bins write = {0};
    bins read  = {1};
  }
  // 0 every byte ACKed, 1 the address NACKed, 2 a data byte NACKed
  cp_ack: coverpoint ack {
    bins ack       = {0};
    bins addr_nack = {1};
    bins data_nack = {2};
  }
  cp_len: coverpoint len {
    bins none   = {0};
    bins one    = {1};
    bins short_len = {[2 : 16]};
    bins long_len  = {[17 : $]};
  }
  cr_role_dir: cross cp_role, cp_dir;
  cr_role_ack: cross cp_role, cp_ack;
endgroup

// I2C FIFO resets (i2c.hjson FIFO_CTRL).
covergroup eg2_i2c_fifo_cg(string name) with function sample(bit [8:0] fifo_ctrl);
  option.per_instance = 1;
  option.name = name;
  cp_rxrst: coverpoint fifo_ctrl[0] { bins reset = {1}; }
  cp_fmtrst: coverpoint fifo_ctrl[1] { bins reset = {1}; }
  cp_acqrst: coverpoint fifo_ctrl[7] { bins reset = {1}; }
  cp_txrst: coverpoint fifo_ctrl[8] { bins reset = {1}; }
endgroup

// GPIO: which register a write went to (direct, masked, per pin; the interrupt modes), and the
// direction of the pins after it.
covergroup eg2_gpio_cg(string name) with function sample(int unsigned reg_kind, bit [31:0] oe);
  option.per_instance = 1;
  option.name = name;
  cp_reg: coverpoint reg_kind {
    bins direct_out       = {0};
    bins masked_out_lower = {1};
    bins masked_out_upper = {2};
    bins direct_oe        = {3};
    bins masked_oe_lower  = {4};
    bins masked_oe_upper  = {5};
    bins per_pin_io       = {6};
    bins per_pin_oe       = {7};
    bins intr_rising      = {8};
    bins intr_falling     = {9};
    bins intr_lvlhigh     = {10};
    bins intr_lvllow      = {11};
    bins input_filter     = {12};
  }
  cp_direction: coverpoint oe {
    bins all_inputs  = {32'h0};
    bins all_outputs = {32'hffff_ffff};
    bins mixed       = default;
  }
endgroup

// pinmux: what each pad of the bench's GPIO pad set is routed to (MIO_OUTSEL writes).
covergroup eg2_pinmux_cg(string name) with function sample(int unsigned pad, int unsigned sel);
  option.per_instance = 1;
  option.name = name;
  cp_pad: coverpoint pad {
    bins pads[] = {[0 : 31]};
  }
  // 0 tied 0 or 1, 1 high-Z, 2 a GPIO, 3 another peripheral's output
  cp_sel: coverpoint sel {
    bins tie   = {0};
    bins hiz   = {1};
    bins gpio  = {2};
    bins other = {3};
  }
  cr_pad_gpio: cross cp_pad, cp_sel {
    ignore_bins not_gpio = !binsof(cp_sel.gpio);
  }
endgroup

// USB: each packet on the bus, by PID and direction; the endpoint of the tokens.
covergroup eg2_usb_cg(string name) with function sample(bit [3:0] pid, bit [3:0] endpoint,
                                                       bit token, bit from_chip);
  option.per_instance = 1;
  option.name = name;
  cp_pid: coverpoint pid {
    bins out_   = {4'h1};
    bins in_    = {4'h9};
    bins sof    = {4'h5};
    bins setup  = {4'hd};
    bins data0  = {4'h3};
    bins data1  = {4'hb};
    bins ack    = {4'h2};
    bins nak    = {4'ha};
    bins stall  = {4'he};
  }
  cp_dir: coverpoint from_chip {
    bins host_to_chip = {0};
    bins chip_to_host = {1};
  }
  cp_endpoint: coverpoint endpoint iff (token) {
    bins eps[] = {[0 : 11]};
  }
  cr_pid_dir: cross cp_pid, cp_dir;
endgroup

// usbdev: each answer the model predicted (eg2_usbdev_monitor): the host's request, the chip's
// answer, the endpoint, the payload length (usbdev_testplan: IN and OUT on every endpoint, NAK,
// STALL, SETUP).
covergroup eg2_usbdev_xfer_cg(string name) with function sample(int unsigned kind, bit [3:0] pid,
                                                               bit [3:0] ep, int unsigned len);
  option.per_instance = 1;
  option.name = name;
  // 0 IN, 1 SETUP, 2 OUT
  cp_kind: coverpoint kind {
    bins in_    = {0};
    bins setup  = {1};
    bins out_   = {2};
  }
  cp_answer: coverpoint pid {
    bins data  = {4'h3, 4'hb};
    bins ack   = {4'h2};
    bins nak   = {4'ha};
    bins stall = {4'he};
  }
  cp_ep: coverpoint ep {
    bins eps[] = {[0 : 11]};
  }
  cp_len: coverpoint len iff (pid inside {4'h3, 4'hb}) {
    bins zero     = {0};
    bins short_len = {[1 : 8]};
    bins mid_len  = {[9 : 63]};
    bins max_len  = {64};
  }
  cr_kind_answer: cross cp_kind, cp_answer {
    ignore_bins in_ack   = binsof(cp_kind.in_) && binsof(cp_answer.ack);
    ignore_bins out_data = binsof(cp_kind.out_) && binsof(cp_answer.data);
    ignore_bins setup_no = binsof(cp_kind.setup) && !binsof(cp_answer.ack);
  }
endgroup

// spi_device: each host transaction by what serves it, and its length.
covergroup eg2_spi_device_cg(string name) with function sample(int unsigned kind, int unsigned len);
  option.per_instance = 1;
  option.name = name;
  // 0 read status, 1 JEDEC ID, 2 SFDP, 3 read (buffer or mailbox), 4 upload, 5 TPM read,
  // 6 TPM write
  cp_kind: coverpoint kind {
    bins status    = {0};
    bins jedec     = {1};
    bins sfdp      = {2};
    bins read      = {3};
    bins upload    = {4};
    bins tpm_read  = {5};
    bins tpm_write = {6};
  }
  cp_len: coverpoint len {
    bins none      = {0};
    bins one_word  = {[1 : 4]};
    bins short_len = {[5 : 64]};
    bins long_len  = {[65 : $]};
  }
  cr_kind_len: cross cp_kind, cp_len;
endgroup

// sysrst_ctrl: the features that changed what the block does (sysrst_ctrl_testplan).
covergroup eg2_sysrst_cg(string name) with function sample(int unsigned kind, int unsigned which);
  option.per_instance = 1;
  option.name = name;
  // 1 a pin override enabled, 2 key inversion written, 3 a key interrupt, 4 a combo fired,
  // 5 the ULP wake-up, 6 EC reset stretching, 7 auto-block
  cp_kind: coverpoint kind {
    bins override_pin = {1};
    bins invert       = {2};
    bins key_intr     = {3};
    bins combo        = {4};
    bins ulp          = {5};
    bins ec_rst       = {6};
    bins auto_block   = {7};
  }
  cp_override: coverpoint which iff (kind == 1) {
    bins pins[] = {[0 : 7]};
  }
  cp_key_intr: coverpoint which iff (kind == 3) {
    bins edges[] = {[0 : 13]};
  }
  cp_combo: coverpoint which iff (kind == 4) {
    bins combos[] = {[0 : 3]};
  }
endgroup

// adc_ctrl: each debounced event: the mode it happened in, how many filters matched, the
// low-power to normal transition (adc_ctrl_testplan: normal, low power, oneshot).
covergroup eg2_adc_cg(string name) with function sample(int unsigned mode, int unsigned nmatch,
                                                       bit trans);
  option.per_instance = 1;
  option.name = name;
  // 0 normal, 1 low power, 2 oneshot
  cp_mode: coverpoint mode {
    bins normal    = {0};
    bins low_power = {1};
    bins oneshot   = {2};
  }
  cp_nmatch: coverpoint nmatch iff (mode != 2) {
    bins one  = {1};
    bins some = {[2 : 7]};
    bins all  = {8};
  }
  cp_trans: coverpoint trans;
  cr_mode_trans: cross cp_mode, cp_trans {
    ignore_bins no_lp_trans = binsof(cp_mode.normal) && binsof(cp_trans) intersect {1};
    ignore_bins no_os_trans = binsof(cp_mode.oneshot) && binsof(cp_trans) intersect {1};
  }
endgroup

// sensor_ctrl: each AST event that became active: which, recoverable or fatal, enabled or not.
covergroup eg2_sensor_cg(string name) with function sample(int unsigned idx, bit fatal_en,
                                                          bit enabled);
  option.per_instance = 1;
  option.name = name;
  cp_idx: coverpoint idx {
    bins events[] = {[0 : 10]};
  }
  cp_fatal: coverpoint fatal_en;
  cp_enabled: coverpoint enabled;
  cr_idx_fatal: cross cp_idx, cp_fatal;
endgroup

// The wake-up detectors: each detection by mode, filter and pad group.
covergroup eg2_wkup_cg(string name) with function sample(int unsigned mode, bit filter,
                                                        bit miodio);
  option.per_instance = 1;
  option.name = name;
  cp_mode: coverpoint mode {
    bins posedge_ = {0};
    bins negedge_ = {1};
    bins edge_    = {2};
    bins timed_hi = {3};
    bins timed_lo = {4};
  }
  cp_filter: coverpoint filter;
  cp_pads: coverpoint miodio {
    bins mio = {0};
    bins dio = {1};
  }
  cr_mode_filter: cross cp_mode, cp_filter;
endgroup

// The injection registry: each announced injection's kind against its outcome on each check it
// targets.
covergroup eg2_inject_cg(string name) with function sample(int unsigned kind, int unsigned outcome);
  option.per_instance = 1;
  option.name = name;
  // 0 a mutation, 1 a checker corruption, 2 a model fault, 3 stimulus with errors
  cp_kind: coverpoint kind {
    bins mutation    = {0};
    bins sb_corrupt  = {1};
    bins model_fault = {2};
    bins stimulus    = {3};
  }
  // 0 detected (EXPECTED), 1 MISSED, 2 the check was not in the run, 3 caused no mismatch
  cp_outcome: coverpoint outcome {
    bins expected    = {0};
    bins missed      = {1};
    bins not_in_run  = {2};
    bins no_mismatch = {3};
  }
  cr_kind_outcome: cross cp_kind, cp_outcome;
endgroup

// TL-UL error responses at the peripheral's port: each access's class against its opcode (per
// instance, so per peripheral).
covergroup eg2_tl_err_cg(string name) with function sample(int unsigned cls, bit [2:0] opcode);
  option.per_instance = 1;
  option.name = name;
  cp_cls: coverpoint cls {
    bins ok             = {0};
    bins expected_err   = {1};
    bins unexpected_err = {2};
    bins missing_err    = {3};
  }
  cp_opcode: coverpoint opcode {
    bins put_full    = {3'h0};
    bins put_partial = {3'h1};
    bins get         = {3'h4};
  }
  cr_cls_opcode: cross cp_cls, cp_opcode;
endgroup
