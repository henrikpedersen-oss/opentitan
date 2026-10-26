// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 peripheral tests (eg2_top_verification spec REQ_PBF_13: chip_sw_eg2_pbf_periph_sb and
// chip_sw_eg2_pbf_periph_fault; software in sw/device/tests/eg2_periph). Compiled with
// EG2_PERIPH_SB only: they run in build mode en_eg2_periph, where every peripheral has its own
// scoreboard (eg2_periph_env).
//
// The bench side of the tests' contract (eg2_periph.h): the agents on the pads the software routes
// its peripherals to (chip_if: UART1 Iob4/Iob5, UART2 Ioa4/Ioa5, UART3 Ioa0/Ioa1; I2C0 Ioa7/Ioa8,
// I2C1 Iob9/Iob10, I2C2 Iob11/Iob12; spi_host0's own pads, spi_host1 on Iob0-Iob6), at the
// software's rates: the UARTs at 1 Mbaud (clk_class 0) or 1.5 Mbaud (1), the I2C agents as
// targets at `i2c_addr` with chip_sw_i2c_host_tx_rx_vseq's timing, the SPI device agents playing
// back in 4-byte units. With rx_bench = 1 each UART agent answers the software's marker byte 0xA5
// with rx_count bytes (rx_base + i). Behind secure boot the image is signed, so the knobs keep the
// image's defaults and the bench uses those.

class chip_sw_eg2_periph_base_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_periph_base_vseq)
  `uvm_object_new

  // The I2C timing of chip_sw_i2c_tx_rx_vseq: a 41 ns clock, a 1000 ns SCL period.
  int i2c_half_period_cycles = ((1000 / 2 - 1) / 41) + 1;

  // The UARTs of `mask` (bit i: UART i) at the software's baud, their RX monitors on (the RX
  // direction), declared used: a UART among them whose monitor sees nothing fails the run.
  protected function void connect_uarts(bit [1:0] clk_class, bit [NUM_UARTS-1:0] mask);
    for (int i = 1; i < NUM_UARTS; i++) begin
      if (!mask[i]) continue;
      configure_uart_agent(.uart_idx(i), .enable(1), .enable_rx_monitor(1));
      cfg.m_uart_agent_cfgs[i].set_baud_rate(clk_class == 0 ? cfg.uart_baud_rate :
                                             uart_agent_pkg::BaudRate1p5Mbps);
      cfg.eg2_sb_uart_required[i] = 1'b1;
    end
  endfunction

  protected function void connect_i2c(int idx, bit [6:0] addr);
    int tsetup = 2;
    cfg.m_i2c_agent_cfgs[idx].en_monitor = 1'b1;
    cfg.m_i2c_agent_cfgs[idx].if_mode = Device;
    cfg.m_i2c_agent_cfgs[idx].target_addr0 = addr;
    cfg.m_i2c_agent_cfgs[idx].target_addr1 = addr;
    if (cfg.en_dv_cdc) begin
      tsetup++;
      cfg.m_i2c_agent_cfgs[idx].timing_cfg.tHoldBit = 1;
    end
    cfg.m_i2c_agent_cfgs[idx].timing_cfg.tSetupBit = tsetup;
    cfg.m_i2c_agent_cfgs[idx].timing_cfg.tClockLow = i2c_half_period_cycles - tsetup;
    cfg.m_i2c_agent_cfgs[idx].timing_cfg.tClockPulse = i2c_half_period_cycles;
    cfg.chip_vif.enable_i2c(.inst_num(idx), .enable(1));
  endfunction

  protected task start_i2c_responder(int idx);
    i2c_device_response_seq seq = i2c_device_response_seq::type_id::create("seq");
    fork seq.start(p_sequencer.i2c_sequencer_hs[idx]); join_none
  endtask

  protected function void prepare_spi(int idx);
    cfg.m_spi_device_agent_cfgs[idx].csid = '0;
    cfg.m_spi_device_agent_cfgs[idx].num_bytes_per_trans_in_mon = 4;
    cfg.m_spi_device_agent_cfgs[idx].byte_order = '0;
  endfunction

  protected task start_spi_device(int idx);
    spi_device_seq seq = spi_device_seq::type_id::create("seq");
    cfg.chip_vif.enable_spi_device(.inst_num(idx), .enable(1));
    fork seq.start(p_sequencer.spi_device_sequencer_hs[idx]); join_none
  endtask

  // The UART agent `idx` answers the software's marker byte with `count` bytes from `base`.
  protected task uart_answer(int idx, int count, bit [7:0] base);
    uart_item item;
    forever begin
      p_sequencer.uart_tx_fifos[idx].get(item);
      if (item.data == 8'ha5) break;
    end
    for (int i = 0; i < count; i++) begin
      uart_default_seq s;
      `uvm_create_on(s, p_sequencer.uart_sequencer_hs[idx])
      `DV_CHECK_RANDOMIZE_WITH_FATAL(s, data == 8'(base + i);)
      `uvm_send(s)
    end
    `uvm_info(`gfn, $sformatf("UART%0d: %0d bytes sent from 0x%02x after the marker", idx, count,
                              base), UVM_LOW)
  endtask
endclass

// chip_sw_eg2_pbf_periph_sb: UART, SPI host, I2C and GPIO traffic.
class chip_sw_eg2_pbf_periph_sb_vseq extends chip_sw_eg2_periph_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_periph_sb_vseq)
  `uvm_object_new

  rand bit [3:0] devices;
  rand bit [1:0] len_class;
  rand bit [1:0] clk_class;
  rand bit       rx_bench;
  rand int unsigned rx_count;
  rand bit [7:0] rx_base;
  rand bit [6:0] i2c_addr;

  constraint knobs_c {
    devices != 0;
    // Every device in most runs, one or two in the rest.
    devices dist {4'hf :/ 60, [4'h1 : 4'he] :/ 40};
    len_class inside {[0 : 3]};
    clk_class inside {[0 : 1]};
    rx_bench dist {1 :/ 70, 0 :/ 30};
    rx_count inside {[1 : 32]};
    i2c_addr inside {[7'h08 : 7'h77]};
  }

  virtual function void write_params();
    set_param("devices", devices);
    set_param("len_class", len_class);
    set_param("clk_class", clk_class);
    set_param("rx_bench", rx_bench);
    set_param("rx_count", rx_count);
    set_param("rx_base", rx_base);
    set_param("i2c_addr", i2c_addr);
  endfunction

  protected function void pin(string name, ref bit [31:0] v);
    bit [31:0] p;
    if ($value$plusargs({"eg2_param_", name, "=%d"}, p)) v = p;
  endfunction

  virtual task body();
    bit [31:0] dev = devices, clk = clk_class, rxb = rx_bench, cnt = rx_count, base = rx_base,
               addr = i2c_addr;
    if (cfg.rom_ext_assembled) begin
      // The signed image keeps its defaults (eg2_pbf_periph_sb.c).
      dev = 15; clk = 0; rxb = 0; addr = 51;
    end else begin
      pin("devices", dev);
      pin("clk_class", clk);
      pin("rx_bench", rxb);
      pin("rx_count", cnt);
      pin("rx_base", base);
      pin("i2c_addr", addr);
    end
    if (dev[1]) begin
      prepare_spi(0);
      if (!dev[0]) prepare_spi(1);
    end
    super.body();
    if (dev[0]) begin
      connect_uarts(clk[1:0], 4'b1110);
      if (rxb) begin
        for (int i = 1; i < NUM_UARTS; i++) begin
          automatic int idx = i;
          fork uart_answer(idx, cnt, base[7:0]); join_none
        end
      end
    end
    if (dev[1]) begin
      start_spi_device(0);
      if (!dev[0]) start_spi_device(1);
    end
    if (dev[2]) begin
      for (int i = 0; i < NUM_I2CS; i++) begin
        connect_i2c(i, addr[6:0]);
        start_i2c_responder(i);
      end
    end
  endtask
endclass

// chip_sw_eg2_pbf_periph_fault: a transfer on UART1, spi_host0 or I2C0, reset in the middle.
class chip_sw_eg2_pbf_periph_fault_vseq extends chip_sw_eg2_periph_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_periph_fault_vseq)
  `uvm_object_new

  rand bit [1:0] device;
  rand bit       fault;
  rand bit [1:0] fault_point;
  rand bit [1:0] len_class;
  rand bit [6:0] i2c_addr;

  constraint knobs_c {
    // The bench draws the device, so it connects the right agent.
    device inside {[0 : 2]};
    fault_point inside {[0 : 2]};
    len_class inside {[0 : 3]};
    i2c_addr inside {[7'h08 : 7'h77]};
  }

  virtual function void write_params();
    set_param("device", device);
    set_param("fault", fault);
    set_param("fault_point", fault_point);
    set_param("len_class", len_class);
    set_param("i2c_addr", i2c_addr);
  endfunction

  virtual task body();
    bit [31:0] d = device, addr = i2c_addr;
    void'($value$plusargs("eg2_param_device=%d", d));
    void'($value$plusargs("eg2_param_i2c_addr=%d", addr));
    // Device 3 (the image draws it) and secure boot (the image's defaults): every agent.
    if (cfg.rom_ext_assembled) begin
      d = 3;
      addr = 51;
    end
    if (d inside {1, 3}) prepare_spi(0);
    super.body();
    if (d inside {0, 3}) connect_uarts(0, 4'b0010);
    if (d inside {1, 3}) start_spi_device(0);
    if (d inside {2, 3}) begin
      connect_i2c(0, addr[6:0]);
      start_i2c_responder(0);
    end
  endtask
endclass
