// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 per-peripheral scoreboards and what feeds them (status/decisions.md 2026-10-24; EG2
// spec "Chip-Level Scoreboard"). Compiled with EG2_PERIPH_SB only (build mode en_eg2_periph),
// built by chip_env only with +eg2_periph_sb=1 (which that build mode's run_opts set); without
// both, chip_env has exactly OpenTitan's and the earlier EG2 components.
//
// It holds:
//   - a passive tl_monitor on each peripheral's TL-UL port (the taps of tb/eg2_periph_tl.svh);
//   - the GPIO and USB pin monitors (eg2_periph_monitors.sv), which OpenTitan's bench lacks;
//   - the spi_device and usbdev conversions (eg2_device_monitors.sv);
//   - the system monitor: one monitor per block (eg2_sysmon_monitors.sv: sysrst_ctrl, adc_ctrl,
//     sensor_ctrl, the wake-up detectors with pwrmgr's record; status/decisions.md 19);
//   - one scoreboard per peripheral instance (eg2_periph_sbs.sv): uart0-3, spi_host0-1,
//     spi_device, i2c0-2, gpio, usbdev, jtag, sysrst_ctrl, adc_ctrl, sensor_ctrl, wkup;
//   - the path check (eg2_periph_path_check.sv).
// The extended agent monitors (eg2_uart_monitor, eg2_spi_monitor, eg2_i2c_monitor) replace
// OpenTitan's by factory override (set_overrides, called by chip_env before it builds the agents).
// The run's checker corruptions: +sb_corrupt=<check>.<field> (announced, must be detected) and
// +eg2_sb_flip=<instance>.item / +eg2_tl_err_flip=<port> (not announced, must fail the run).
// I3C waits for the I3C agent (decision of 2026-10-24); pattgen is not in Earl Grey 2.

class eg2_periph_env extends uvm_env;
  `uvm_component_utils(eg2_periph_env)

  chip_env_cfg cfg;

  // The TL-UL taps, by instance name, with their windows.
  typedef struct {
    string     name;
    bit [31:0] base;
    bit [31:0] size;
  } tap_t;
  tap_t taps[$];

  tl_monitor            m_tl_mon[string];
  protected tl_agent_cfg m_tl_cfg[string];
  eg2_gpio_monitor      m_gpio_mon;
  eg2_usbdev_monitor    m_usb_mon;
  eg2_spi_device_monitor m_spid_mon;
  eg2_sysrst_ctrl_monitor m_sysrst_mon;
  eg2_adc_ctrl_monitor  m_adc_mon;
  eg2_sensor_ctrl_monitor m_sensor_mon;
  eg2_wkup_monitor      m_wkup_mon;

  eg2_uart_sb           m_uart_sb[NUM_UARTS];
  eg2_spi_host_sb       m_spi_host_sb[NUM_SPI_HOSTS];
  eg2_spi_device_sb     m_spi_device_sb;
  eg2_i2c_sb            m_i2c_sb[NUM_I2CS];
  eg2_gpio_sb           m_gpio_sb;
  eg2_usbdev_sb         m_usbdev_sb;
  eg2_jtag_sb           m_jtag_sb;
  eg2_tl_port_sb        m_pinmux_sb;
  eg2_tl_port_sb        m_pwrmgr_sb;
  eg2_sysrst_ctrl_sb    m_sysrst_sb;
  eg2_adc_ctrl_sb       m_adc_sb;
  eg2_sensor_ctrl_sb    m_sensor_sb;
  eg2_wkup_sb           m_wkup_sb;
  // Each port's accesses, classified against the documents' TL-UL error prediction (eg2_tl_err)
  // and handed to that peripheral's scoreboard.
  protected uvm_tlm_analysis_fifo #(tl_seq_item)        m_tl_chk_fifo[string];
  protected uvm_tlm_analysis_fifo #(eg2_tl_access_item) m_tl_dest[string];
  eg2_periph_path_check m_path;
  // Coverage (eg2_periph_cov.sv): the taps' TL-UL coverage and the injection registry's.
  protected tl_agent_cov m_tl_cov[string];
  eg2_inject_cg          inject_cg;

  `uvm_component_new

  // +eg2_periph_sb=1 in a build with EG2_PERIPH_SB.
  static function bit enabled();
    return eg2_inject_registry::active();
  endfunction

  static function void set_overrides();
    uart_monitor::type_id::set_type_override(eg2_uart_monitor::get_type());
    spi_monitor::type_id::set_type_override(eg2_spi_monitor::get_type());
    i2c_monitor::type_id::set_type_override(eg2_i2c_monitor::get_type());
  endfunction

  protected function void add_tap(string name, bit [31:0] base, bit [31:0] size);
    tap_t t;
    t.name = name;
    t.base = base;
    t.size = size;
    taps.push_back(t);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    add_tap("uart0", TOP_EARLGREY_UART0_BASE_ADDR, TOP_EARLGREY_UART0_SIZE_BYTES);
    add_tap("uart1", TOP_EARLGREY_UART1_BASE_ADDR, TOP_EARLGREY_UART1_SIZE_BYTES);
    add_tap("uart2", TOP_EARLGREY_UART2_BASE_ADDR, TOP_EARLGREY_UART2_SIZE_BYTES);
    add_tap("uart3", TOP_EARLGREY_UART3_BASE_ADDR, TOP_EARLGREY_UART3_SIZE_BYTES);
    add_tap("gpio", TOP_EARLGREY_GPIO_BASE_ADDR, TOP_EARLGREY_GPIO_SIZE_BYTES);
    add_tap("spi_device", TOP_EARLGREY_SPI_DEVICE_BASE_ADDR, TOP_EARLGREY_SPI_DEVICE_SIZE_BYTES);
    add_tap("i2c0", TOP_EARLGREY_I2C0_BASE_ADDR, TOP_EARLGREY_I2C0_SIZE_BYTES);
    add_tap("i2c1", TOP_EARLGREY_I2C1_BASE_ADDR, TOP_EARLGREY_I2C1_SIZE_BYTES);
    add_tap("i2c2", TOP_EARLGREY_I2C2_BASE_ADDR, TOP_EARLGREY_I2C2_SIZE_BYTES);
    add_tap("spi_host0", TOP_EARLGREY_SPI_HOST0_BASE_ADDR, TOP_EARLGREY_SPI_HOST0_SIZE_BYTES);
    add_tap("spi_host1", TOP_EARLGREY_SPI_HOST1_BASE_ADDR, TOP_EARLGREY_SPI_HOST1_SIZE_BYTES);
    add_tap("usbdev", TOP_EARLGREY_USBDEV_BASE_ADDR, TOP_EARLGREY_USBDEV_SIZE_BYTES);
    add_tap("pinmux", TOP_EARLGREY_PINMUX_BASE_ADDR, TOP_EARLGREY_PINMUX_SIZE_BYTES);
    add_tap("sysrst_ctrl", TOP_EARLGREY_SYSRST_CTRL_BASE_ADDR, TOP_EARLGREY_SYSRST_CTRL_SIZE_BYTES);
    add_tap("adc_ctrl", TOP_EARLGREY_ADC_CTRL_BASE_ADDR, TOP_EARLGREY_ADC_CTRL_SIZE_BYTES);
    add_tap("sensor_ctrl", TOP_EARLGREY_SENSOR_CTRL_BASE_ADDR, TOP_EARLGREY_SENSOR_CTRL_SIZE_BYTES);
    add_tap("pwrmgr", TOP_EARLGREY_PWRMGR_BASE_ADDR, TOP_EARLGREY_PWRMGR_SIZE_BYTES);

    foreach (taps[i]) begin
      string       n = taps[i].name;
      tl_agent_cfg c = tl_agent_cfg::type_id::create({"m_tl_cfg_", n});
      if (!uvm_config_db#(virtual tl_if)::get(this, "", {"tl_", n}, c.vif)) begin
        `uvm_fatal(`gfn, $sformatf("no TL-UL tap tl_%s (tb/eg2_periph_tl.svh)", n))
      end
      c.is_active = 1'b0;
      c.en_cov = 1'b0;
      c.if_mode = dv_utils_pkg::Host;
      c.valid_a_source_width = top_pkg::TL_AIW;
      // A register port answers one request at a time (tlul_adapter_reg); the memory windows of
      // spi_device and usbdev (tlul_adapter_sram) take a second one.
      c.max_outstanding_req = 2;
      c.ok_to_end_delay_ns = 0;
      // TL-UL coverage per tap (tl_agent_cov, per instance), in device mode: the responses the
      // peripheral gave (opcode, size, error), outstanding requests, requests pending at a reset.
      c.if_mode = dv_utils_pkg::Device;
      c.en_cov = cfg.en_cov;
      m_tl_cfg[n] = c;
      m_tl_mon[n] = tl_monitor::type_id::create({"m_tl_mon_", n}, this);
      m_tl_mon[n].cfg = c;
      m_tl_mon[n].agent_name = n;
      if (c.en_cov) begin
        m_tl_cov[n] = tl_agent_cov::type_id::create({"m_tl_cov_", n}, this);
        m_tl_cov[n].cfg = c;
        m_tl_mon[n].cov = m_tl_cov[n];
      end
    end

    m_gpio_mon = eg2_gpio_monitor::type_id::create("m_gpio_mon", this);
    if (!uvm_config_db#(virtual eg2_pads_if)::get(this, "", "pads_vif", m_gpio_mon.pads_vif)) begin
      `uvm_fatal(`gfn, "no pads_vif (tb/eg2_periph_tl.svh)")
    end
    m_usb_mon = eg2_usbdev_monitor::type_id::create("m_usb_mon", this);
    m_spid_mon = eg2_spi_device_monitor::type_id::create("m_spid_mon", this);
    m_sysrst_mon = eg2_sysrst_ctrl_monitor::type_id::create("m_sysrst_mon", this);
    m_adc_mon = eg2_adc_ctrl_monitor::type_id::create("m_adc_mon", this);
    m_sensor_mon = eg2_sensor_ctrl_monitor::type_id::create("m_sensor_mon", this);
    m_wkup_mon = eg2_wkup_monitor::type_id::create("m_wkup_mon", this);
    begin
      virtual eg2_sysmon_if sv;
      if (!uvm_config_db#(virtual eg2_sysmon_if)::get(this, "", "sysmon_vif", sv)) begin
        `uvm_fatal(`gfn, "no sysmon_vif (tb/eg2_periph_tl.svh)")
      end
      m_sysrst_mon.vif = sv;
      m_adc_mon.vif = sv;
      m_sensor_mon.vif = sv;
      m_wkup_mon.vif = sv;
      m_sysrst_mon.inst = "sysrst_ctrl";
      m_adc_mon.inst = "adc_ctrl";
      m_sensor_mon.inst = "sensor_ctrl";
      m_wkup_mon.inst = "wkup";
    end

    foreach (m_uart_sb[i]) begin
      m_uart_sb[i] = eg2_uart_sb::type_id::create($sformatf("m_uart%0d_sb", i), this);
      m_uart_sb[i].idx = i;
      describe(m_uart_sb[i], $sformatf("uart%0d", i), $sformatf("port %0d", i), "uart",
               "Serial ports: the test log on uart0, host communication, bootstrap",
               "eg2/hw/ip/uart/README.md");
    end
    foreach (m_spi_host_sb[i]) begin
      m_spi_host_sb[i] = eg2_spi_host_sb::type_id::create($sformatf("m_spi_host%0d_sb", i), this);
      m_spi_host_sb[i].idx = i;
      describe(m_spi_host_sb[i], $sformatf("spi_host%0d", i), $sformatf("port %0d", i), "spi_host",
               "SPI controller for external flash and devices", "eg2/hw/ip/spi_host/README.md");
    end
    m_spi_device_sb = eg2_spi_device_sb::type_id::create("m_spi_device_sb", this);
    describe(m_spi_device_sb, "spi_device", "port 0", "spi_device",
             "SPI target for a host: flash emulation, passthrough to flash, TPM",
             "eg2/hw/ip/spi_device/README.md");
    foreach (m_i2c_sb[i]) begin
      m_i2c_sb[i] = eg2_i2c_sb::type_id::create($sformatf("m_i2c%0d_sb", i), this);
      m_i2c_sb[i].idx = i;
      describe(m_i2c_sb[i], $sformatf("i2c%0d", i), $sformatf("port %0d", i), "i2c",
               "I2C host and target for sensors and other devices", "eg2/hw/ip/i2c/README.md");
    end
    m_gpio_sb = eg2_gpio_sb::type_id::create("m_gpio_sb", this);
    describe(m_gpio_sb, "gpio", "port 0", "gpio",
             "General-purpose pins: inputs with interrupts, outputs, straps",
             "eg2/hw/top_earlgrey/ip_autogen/gpio/README.md");
    m_usbdev_sb = eg2_usbdev_sb::type_id::create("m_usbdev_sb", this);
    describe(m_usbdev_sb, "usbdev", "port 0", "usbdev", "USB full-speed device",
             "eg2/hw/ip/usbdev/README.md");
    m_pinmux_sb = eg2_tl_port_sb::type_id::create("m_pinmux_sb", this);
    describe(m_pinmux_sb, "pinmux", "port 0", "pinmux",
             "Pad multiplexing, pad attributes, wake-up detectors, pad retention in sleep",
             "eg2/hw/top_earlgrey/ip_autogen/pinmux/README.md");
    m_pwrmgr_sb = eg2_tl_port_sb::type_id::create("m_pwrmgr_sb", this);
    describe(m_pwrmgr_sb, "pwrmgr", "port 0", "pwrmgr",
             "Power manager: low-power entry, wake-up and reset requests, wake-up record",
             "eg2/hw/top_earlgrey/ip_autogen/pwrmgr/README.md");
    // The system monitor's blocks (decision 19).
    m_sysrst_sb = eg2_sysrst_ctrl_sb::type_id::create("m_sysrst_sb", this);
    describe(m_sysrst_sb, "sysrst_ctrl", "port 0", "sysrst_ctrl",
             "System reset controller: key combos, EC reset, pin pass-through and override, ULP wake-up",
             "eg2/hw/ip/sysrst_ctrl/README.md");
    m_adc_sb = eg2_adc_ctrl_sb::type_id::create("m_adc_sb", this);
    describe(m_adc_sb, "adc_ctrl", "port 0", "adc_ctrl",
             "ADC controller: filtered, debounced AST ADC samples, interrupts and wake-ups",
             "eg2/hw/ip/adc_ctrl/README.md");
    m_sensor_sb = eg2_sensor_ctrl_sb::type_id::create("m_sensor_sb", this);
    describe(m_sensor_sb, "sensor_ctrl", "port 0", "sensor_ctrl",
             "Sensor control: AST alert events forwarded as alerts, AST status",
             "eg2/hw/top_earlgrey/ip/sensor_ctrl/README.md");
    m_wkup_sb = eg2_wkup_sb::type_id::create("m_wkup_sb", this);
    describe(m_wkup_sb, "wkup", "pinmux wake-up detectors and pwrmgr", "pinmux, pwrmgr",
             "Wake-up pads: pinmux's wake-up detectors and the wake reason pwrmgr records",
             "eg2/hw/top_earlgrey/ip_autogen/pinmux/README.md, eg2/hw/top_earlgrey/ip_autogen/pwrmgr/README.md");
    m_sysrst_sb.mon = m_sysrst_mon;
    m_adc_sb.mon = m_adc_mon;
    m_sensor_sb.mon = m_sensor_mon;
    m_wkup_sb.mon = m_wkup_mon;
    m_spi_device_sb.mon = m_spid_mon;
    foreach (taps[i]) begin
      m_tl_chk_fifo[taps[i].name] = new({"m_tl_chk_fifo_", taps[i].name}, this);
    end
    m_jtag_sb = eg2_jtag_sb::type_id::create("m_jtag_sb", this);
    describe(m_jtag_sb, "jtag", "the rv_dm and life-cycle TAPs", "rv_dm",
             "Debug and life-cycle access over JTAG", "eg2/hw/ip/rv_dm/README.md");

    m_path = eg2_periph_path_check::type_id::create("m_path", this);
    // add_port creates the port's TLM fifo, a component: only legal in build_phase.
    foreach (taps[i]) m_path.add_port(taps[i].name, taps[i].base, taps[i].size);
    if (cfg.en_cov) inject_cg = new({get_full_name(), "::eg2_inject_cg"});
  endfunction

  protected function void describe(uvm_component sb, string inst, string port, string block,
                                   string used_for, string spec);
    // Every scoreboard class is an eg2_periph_sb_base of some item type: set the common fields
    // through the typed handles.
    eg2_uart_sb       u;
    eg2_spi_host_sb   sh;
    eg2_spi_device_sb sd;
    eg2_i2c_sb        ic;
    eg2_gpio_sb       g;
    eg2_usbdev_sb     us;
    eg2_jtag_sb       j;
    eg2_tl_port_sb    tp;
    eg2_sysmon_sb_base sm;
    if ($cast(sm, sb)) begin
      sm.inst = inst; sm.port = port; sm.block = block; sm.used_for = used_for; sm.spec = spec;
      sm.cfg = cfg;
    end else if ($cast(u, sb)) begin
      u.inst = inst; u.port = port; u.block = block; u.used_for = used_for; u.spec = spec;
      u.cfg = cfg;
    end else if ($cast(sh, sb)) begin
      sh.inst = inst; sh.port = port; sh.block = block; sh.used_for = used_for; sh.spec = spec;
      sh.cfg = cfg;
    end else if ($cast(sd, sb)) begin
      sd.inst = inst; sd.port = port; sd.block = block; sd.used_for = used_for; sd.spec = spec;
      sd.cfg = cfg;
    end else if ($cast(ic, sb)) begin
      ic.inst = inst; ic.port = port; ic.block = block; ic.used_for = used_for; ic.spec = spec;
      ic.cfg = cfg;
    end else if ($cast(g, sb)) begin
      g.inst = inst; g.port = port; g.block = block; g.used_for = used_for; g.spec = spec;
      g.cfg = cfg;
    end else if ($cast(us, sb)) begin
      us.inst = inst; us.port = port; us.block = block; us.used_for = used_for; us.spec = spec;
      us.cfg = cfg;
    end else if ($cast(tp, sb)) begin
      tp.inst = inst; tp.port = port; tp.block = block; tp.used_for = used_for; tp.spec = spec;
      tp.cfg = cfg;
    end else if ($cast(j, sb)) begin
      j.inst = inst; j.port = port; j.block = block; j.used_for = used_for; j.spec = spec;
      j.cfg = cfg;
    end
  endfunction

  // Each port's accesses, classified once (eg2_tl_err::classify), to its peripheral's scoreboard.
  virtual task run_phase(uvm_phase phase);
    foreach (taps[i]) begin
      automatic string n = taps[i].name;
      fork
        forever begin
          tl_seq_item t;
          m_tl_chk_fifo[n].get(t);
          if (m_tl_dest.exists(n)) m_tl_dest[n].write(eg2_tl_err::classify(n, t));
        end
      join_none
    end
  endtask

  // Called by chip_env's connect_phase, once the agents exist.
  function void connect_env(uart_agent       uart_agents[NUM_UARTS],
                            spi_agent        spi_device_agents[NUM_SPI_HOSTS],
                            spi_agent        spi_host_agent,
                            i2c_agent        i2c_agents[NUM_I2CS],
                            jtag_riscv_agent jtag_agent,
                            eg2_rvfi_agent   rvfi_agent);
    foreach (taps[i]) begin
      m_tl_mon[taps[i].name].d_chan_port.connect(m_path.tl_fifo[taps[i].name].analysis_export);
    end
    m_path.enabled = rvfi_agent.enabled() && !cfg.chip_vif.stub_cpu;

    // TL-UL error classification: every tap to its peripheral's scoreboard.
    foreach (m_uart_sb[i]) m_tl_dest[m_uart_sb[i].inst] = m_uart_sb[i].tl_fifo;
    foreach (m_spi_host_sb[i]) m_tl_dest[m_spi_host_sb[i].inst] = m_spi_host_sb[i].tl_fifo;
    foreach (m_i2c_sb[i]) m_tl_dest[m_i2c_sb[i].inst] = m_i2c_sb[i].tl_fifo;
    m_tl_dest["spi_device"] = m_spi_device_sb.tl_fifo;
    m_tl_dest["gpio"]       = m_gpio_sb.tl_fifo;
    m_tl_dest["usbdev"]     = m_usbdev_sb.tl_fifo;
    m_tl_dest["pinmux"]     = m_pinmux_sb.tl_fifo;
    m_tl_dest["pwrmgr"]     = m_pwrmgr_sb.tl_fifo;
    m_tl_dest["sysrst_ctrl"] = m_sysrst_sb.tl_fifo;
    m_tl_dest["adc_ctrl"]   = m_adc_sb.tl_fifo;
    m_tl_dest["sensor_ctrl"] = m_sensor_sb.tl_fifo;
    foreach (taps[i]) begin
      m_tl_mon[taps[i].name].d_chan_port.connect(m_tl_chk_fifo[taps[i].name].analysis_export);
    end
    rvfi_agent.monitor.analysis_port.connect(m_path.rvfi_fifo.analysis_export);

    foreach (uart_agents[i]) begin
      eg2_uart_monitor m;
      string n = $sformatf("uart%0d", i);
      if (!$cast(m, uart_agents[i].monitor)) `uvm_fatal(`gfn, "uart monitor not extended")
      m.inst = n;
      m.tl_vif = m_tl_cfg[n].vif;
      m_tl_mon[n].d_chan_port.connect(m.tl_fifo.analysis_export);
      m.tx_analysis_port.connect(m_uart_sb[i].act_fifo.analysis_export);
      m.tl_exp_port.connect(m_uart_sb[i].exp_fifo.analysis_export);
      m.rx_exp_port.connect(m_uart_sb[i].drv_fifo.analysis_export);
      m.rx_rd_port.connect(m_uart_sb[i].rd_fifo.analysis_export);
`ifdef EG2_SYSMODEL
      m.rx_analysis_port.connect(m_uart_sb[i].model_rx_fifo.analysis_export);
`endif
    end

    foreach (spi_device_agents[i]) begin
      eg2_spi_monitor m;
      string n = $sformatf("spi_host%0d", i);
      if (!$cast(m, spi_device_agents[i].monitor)) `uvm_fatal(`gfn, "spi monitor not extended")
      m.inst = n;
      m.tl_vif = m_tl_cfg[n].vif;
      m_tl_mon[n].d_chan_port.connect(m.tl_fifo.analysis_export);
      m.host_analysis_port.connect(m_spi_host_sb[i].act_fifo.analysis_export);
      m.tl_exp_port.connect(m_spi_host_sb[i].exp_fifo.analysis_export);
      m.rx_exp_port.connect(m_spi_host_sb[i].drv_fifo.analysis_export);
      m.rx_rd_port.connect(m_spi_host_sb[i].rd_fifo.analysis_export);
    end

    begin
      eg2_spi_monitor m;
      if (!$cast(m, spi_host_agent.monitor)) `uvm_fatal(`gfn, "spi monitor not extended")
      m.inst = "spi_device";
      // The host agent's transactions (flash and TPM at CSB high, the data phase's start, and the
      // chip's bytes in generic mode) go to spi_device's conversion with spi_device's tap; the
      // passthrough announcement goes to spi_host0's expected stream.
      m_spid_mon.tl_vif = m_tl_cfg["spi_device"].vif;
      m_spid_mon.agent_cfg = cfg.m_spi_host_agent_cfg;
      m_tl_mon["spi_device"].d_chan_port.connect(m_spid_mon.tl_fifo.analysis_export);
      m.host_analysis_port.connect(m_spid_mon.host_fifo.analysis_export);
      m.req_analysis_port.connect(m_spid_mon.req_fifo.analysis_export);
      m.device_analysis_port.connect(m_spid_mon.gen_fifo.analysis_export);
      m_spid_mon.pin_port.connect(m_spi_device_sb.act_fifo.analysis_export);
      m_spid_mon.tl_exp_port.connect(m_spi_device_sb.exp_fifo.analysis_export);
      m_spid_mon.rx_exp_port.connect(m_spi_device_sb.drv_fifo.analysis_export);
      m_spid_mon.rx_rd_port.connect(m_spi_device_sb.rd_fifo.analysis_export);
      m_spid_mon.passthrough_port.connect(m_spi_host_sb[0].exp_fifo.analysis_export);
    end

    foreach (i2c_agents[i]) begin
      eg2_i2c_monitor m;
      string n = $sformatf("i2c%0d", i);
      if (!$cast(m, i2c_agents[i].monitor)) `uvm_fatal(`gfn, "i2c monitor not extended")
      m.inst = n;
      m.tl_vif = m_tl_cfg[n].vif;
      m_tl_mon[n].d_chan_port.connect(m.tl_fifo.analysis_export);
      m.pin_port.connect(m_i2c_sb[i].act_fifo.analysis_export);
      m.tl_exp_port.connect(m_i2c_sb[i].exp_fifo.analysis_export);
      m.rx_exp_port.connect(m_i2c_sb[i].drv_fifo.analysis_export);
      m.rx_rd_port.connect(m_i2c_sb[i].rd_fifo.analysis_export);
    end

    m_gpio_mon.tl_vif = m_tl_cfg["gpio"].vif;
    m_gpio_mon.pinmux_vif = m_tl_cfg["pinmux"].vif;
    m_tl_mon["gpio"].d_chan_port.connect(m_gpio_mon.tl_fifo.analysis_export);
    m_tl_mon["pinmux"].d_chan_port.connect(m_gpio_mon.pinmux_fifo.analysis_export);
    m_gpio_mon.pin_port.connect(m_gpio_sb.act_fifo.analysis_export);
    m_gpio_mon.tl_exp_port.connect(m_gpio_sb.exp_fifo.analysis_export);

    m_usb_mon.vif = cfg.usb20_vif;
    m_usb_mon.tl_vif = m_tl_cfg["usbdev"].vif;
    m_tl_mon["usbdev"].d_chan_port.connect(m_usb_mon.tl_fifo.analysis_export);
    m_usb_mon.pin_port.connect(m_usbdev_sb.act_fifo.analysis_export);
    m_usb_mon.tl_exp_port.connect(m_usbdev_sb.exp_fifo.analysis_export);
    m_usb_mon.rx_exp_port.connect(m_usbdev_sb.drv_fifo.analysis_export);
    m_usb_mon.rx_rd_port.connect(m_usbdev_sb.rd_fifo.analysis_export);

    // The system monitor's blocks: each monitor takes its block's tap (the wake-up monitor takes
    // pinmux's and pwrmgr's) and writes to its block's scoreboard.
    m_tl_mon["sysrst_ctrl"].d_chan_port.connect(m_sysrst_mon.tl_fifo.analysis_export);
    m_tl_mon["adc_ctrl"].d_chan_port.connect(m_adc_mon.tl_fifo.analysis_export);
    m_tl_mon["sensor_ctrl"].d_chan_port.connect(m_sensor_mon.tl_fifo.analysis_export);
    m_tl_mon["pinmux"].d_chan_port.connect(m_wkup_mon.tl_fifo.analysis_export);
    m_tl_mon["pwrmgr"].d_chan_port.connect(m_wkup_mon.pwrmgr_fifo.analysis_export);
    connect_sysmon(m_sysrst_mon, m_sysrst_sb);
    connect_sysmon(m_adc_mon, m_adc_sb);
    connect_sysmon(m_sensor_mon, m_sensor_sb);
    connect_sysmon(m_wkup_mon, m_wkup_sb);
    // sensor_ctrl's forwarded alerts, from the bench's alert agents (chip_env's children).
    begin
      string names[2] = '{"sensor_ctrl_recov_alert", "sensor_ctrl_fatal_alert"};
      foreach (names[i]) begin
        uvm_component               c = get_parent().lookup({"m_alert_agent_", names[i]});
        alert_agent_pkg::alert_agent a;
        if (c == null || !$cast(a, c)) begin
          `uvm_fatal(`gfn, $sformatf("no alert agent for %s", names[i]))
        end
        a.m_alert_port.connect(m_sensor_mon.alert_fifo[i].analysis_export);
      end
    end

    // JTAG: chip_scoreboard keeps its own copy (it prints the items); this one logs and counts.
    jtag_agent.monitor.analysis_port.connect(m_jtag_sb.act_fifo.analysis_export);

    // Functional coverage, per instance (eg2_periph_cov.sv).
    if (cfg.en_cov) begin
      foreach (uart_agents[i]) begin
        eg2_uart_monitor m;
        void'($cast(m, uart_agents[i].monitor));
        m.create_cov(m.get_full_name());
        m_uart_sb[i].create_cov(m_uart_sb[i].get_full_name());
      end
      foreach (spi_device_agents[i]) begin
        eg2_spi_monitor m;
        void'($cast(m, spi_device_agents[i].monitor));
        m.create_cov(m.get_full_name());
      end
      foreach (i2c_agents[i]) begin
        eg2_i2c_monitor m;
        void'($cast(m, i2c_agents[i].monitor));
        m.create_cov(m.get_full_name());
        m_i2c_sb[i].create_cov(m_i2c_sb[i].get_full_name());
      end
      m_gpio_mon.create_cov(m_gpio_mon.get_full_name());
      m_usbdev_sb.create_cov(m_usbdev_sb.get_full_name());
      m_usb_mon.create_cov(m_usb_mon.get_full_name());
      m_spid_mon.create_cov(m_spid_mon.get_full_name());
      m_sysrst_mon.create_cov(m_sysrst_mon.get_full_name());
      m_adc_mon.create_cov(m_adc_mon.get_full_name());
      m_sensor_mon.create_cov(m_sensor_mon.get_full_name());
      m_wkup_mon.create_cov(m_wkup_mon.get_full_name());
      m_pwrmgr_sb.create_tl_cov(m_pwrmgr_sb.get_full_name());
      m_sysrst_sb.create_tl_cov(m_sysrst_sb.get_full_name());
      m_adc_sb.create_tl_cov(m_adc_sb.get_full_name());
      m_sensor_sb.create_tl_cov(m_sensor_sb.get_full_name());
      // TL-UL error class x opcode, per peripheral.
      foreach (m_uart_sb[i]) m_uart_sb[i].create_tl_cov(m_uart_sb[i].get_full_name());
      foreach (m_spi_host_sb[i]) m_spi_host_sb[i].create_tl_cov(m_spi_host_sb[i].get_full_name());
      foreach (m_i2c_sb[i]) m_i2c_sb[i].create_tl_cov(m_i2c_sb[i].get_full_name());
      m_spi_device_sb.create_tl_cov(m_spi_device_sb.get_full_name());
      m_gpio_sb.create_tl_cov(m_gpio_sb.get_full_name());
      m_usbdev_sb.create_tl_cov(m_usbdev_sb.get_full_name());
      m_pinmux_sb.create_tl_cov(m_pinmux_sb.get_full_name());
    end
  endfunction

  protected function void connect_sysmon(eg2_sysmon_monitor m, eg2_sysmon_sb_base sb);
    m.pin_port.connect(sb.act_fifo.analysis_export);
    m.tl_exp_port.connect(sb.exp_fifo.analysis_export);
    m.rx_exp_port.connect(sb.drv_fifo.analysis_export);
    m.rx_rd_port.connect(sb.rd_fifo.analysis_export);
  endfunction

  // The register models of the tapped blocks (the chip RAL's blocks, generated from the IPs' hjson)
  // for the TL-UL error prediction; +sb_corrupt and +eg2_tl_err_flip name checks of this run: a
  // typo must not pass as a dead check.
  function void end_of_elaboration_phase(uvm_phase phase);
    string sb_corrupt, flip, ok[$], names[$];
    super.end_of_elaboration_phase(phase);
    foreach (taps[i]) begin
      uvm_reg_block     b = cfg.ral.get_block_by_name(taps[i].name);
      dv_base_reg_block db;
      names.push_back(taps[i].name);
      if (b != null && $cast(db, b)) begin
        eg2_tl_err::set_block(taps[i].name, db);
      end else begin
        `uvm_info(`gfn, $sformatf({"no register model for %s: its TL-UL errors are predicted from ",
                                   "the protocol and integrity rules only"}, taps[i].name), UVM_LOW)
      end
    end
    if ($value$plusargs("eg2_tl_err_flip=%s", flip) && !(flip inside {names})) begin
      `uvm_fatal(`gfn, $sformatf("+eg2_tl_err_flip=%s: no such port", flip))
    end
    ok = '{"rvfi.count", "insn.rd", "insn.cheriot_rd", "path.data"};
    foreach (taps[i]) begin
      ok.push_back({taps[i].name, ".tl_err"});
      // Ports without a scoreboard of their own items.
      if (taps[i].name inside {"pinmux", "pwrmgr"}) continue;
      ok.push_back({taps[i].name, ".count"});
      ok.push_back({taps[i].name, ".item"});
    end
    ok.push_back("jtag.count");
    ok.push_back("jtag.item");
    ok.push_back("wkup.count");
    ok.push_back("wkup.item");
    if ($value$plusargs("eg2_sb_flip=%s", flip) && !(flip inside {ok})) begin
      `uvm_fatal(`gfn, $sformatf("+eg2_sb_flip=%s: no such check.field in an EG2 periph run", flip))
    end
    if (!$value$plusargs("sb_corrupt=%s", sb_corrupt)) return;
    if (!(sb_corrupt inside {ok})) begin
      `uvm_fatal(`gfn, $sformatf("+sb_corrupt=%s: no such check.field in an EG2 periph run", sb_corrupt))
    end
  endfunction

  function void report_phase(uvm_phase phase);
    string s;
    super.report_phase(phase);
    foreach (m_uart_sb[i]) s = {s, "\n  ", m_uart_sb[i].counts()};
    foreach (m_spi_host_sb[i]) s = {s, "\n  ", m_spi_host_sb[i].counts()};
    s = {s, "\n  ", m_spi_device_sb.counts()};
    foreach (m_i2c_sb[i]) s = {s, "\n  ", m_i2c_sb[i].counts()};
    s = {s, "\n  ", m_gpio_sb.counts(), "\n  ", m_usbdev_sb.counts(), "\n  ", m_jtag_sb.counts(),
         "\n  ", m_pinmux_sb.counts(), "\n  ", m_pwrmgr_sb.counts()};
    s = {s, "\n  ", m_sysrst_sb.counts(), "\n  ", m_adc_sb.counts(), "\n  ", m_sensor_sb.counts(),
         "\n  ", m_wkup_sb.counts()};
    `uvm_info(`gfn, {"EG2_SB periph counts (one scoreboard per peripheral):", s}, UVM_LOW)
    // Every announced injection must have been detected by the checks it targets.
    eg2_inject_registry::report();
    if (inject_cg != null) begin
      int unsigned kinds[$], outs[$];
      eg2_inject_registry::outcomes(kinds, outs);
      foreach (kinds[i]) inject_cg.sample(kinds[i], outs[i]);
    end
  endfunction
endclass
