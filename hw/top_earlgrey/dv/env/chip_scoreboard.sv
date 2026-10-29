// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class chip_scoreboard #(type RAL_T = chip_ral_pkg::chip_reg_block) extends cip_base_scoreboard #(
    .CFG_T(chip_env_cfg),
    .RAL_T(RAL_T),
    .COV_T(chip_env_cov)
  );
  `uvm_component_param_utils(chip_scoreboard#(RAL_T))

  // local variables

  // TLM agent fifos
  uvm_tlm_analysis_fifo #(jtag_riscv_item) jtag_fifo;

  // local queues to hold incoming packets pending comparison
  uart_item       uart_tx_q[$];
  uart_item       uart_rx_q[$];
  jtag_riscv_item jtag_q[$];

  `uvm_component_new

  // The execution mode as software sets it (rv_core_ibex CHERIOT_ENA, then CHERIOT_LOCK; the
  // register descriptions): MuBi4True written to CHERIOT_LOCK while CHERIOT_ENA holds MuBi4True
  // enters CHERIoT mode until reset; otherwise the system stays in ePMP mode. Followed from the
  // bus writes, not read from the DUT. CHERIOT_ENA's value is kept from its own write: the register
  // model's mirror is not updated by the core's writes at chip level (2026-10-27: read from the
  // mirror, the mode never switched and chip_sw_eg2_mem_test_cheriot failed).
  protected bit eg2_cheriot_mode;
  protected bit [3:0] eg2_cheriot_ena_written = prim_mubi_pkg::MuBi4False;

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    jtag_fifo = new("jtag_fifo", this);
  endfunction

  function void connect_phase(uvm_phase phase);
    super.connect_phase(phase);
  endfunction

  task run_phase(uvm_phase phase);
    // Disable alert signal integrity check to avoid false alert on low_power_group_en or
    // alert_init. Alert signal integrity can be checked by assertions.
    check_alert_sig_int_err = 0;
    // Disable individual alert checking in scb. Alerts should be checked in SW or DV sequence.
    do_alert_check = 0;
    super.run_phase(phase);
    fork
      process_jtag_fifo();
      if (cfg.en_cov) process_alerts_for_cov();
    join_none
  endtask

`ifdef EG2_PERIPH_SB
  // EG2 (build mode en_eg2_periph, +eg2_periph_sb=1 only): every alert the alert agents see is
  // logged, so a run shows which alerts fired (REQ_SEC_19) and a mutation the hardware must detect
  // (lockstep_shadow_output_glitch) can be graded on it. Other runs keep OpenTitan's behaviour.
  virtual function void on_alert(string alert_name, alert_agent_pkg::alert_seq_item item);
    super.on_alert(alert_name, item);
    if (eg2_inject_registry::active()) begin
      `uvm_info(`gfn, $sformatf("EG2_ALERT %0s received", alert_name), UVM_LOW)
    end
  endfunction
`endif

  virtual task process_alerts_for_cov();
    int alert_cg_size = 0;
    forever @cfg.chip_vif.alerts_if.alerts_cb.alerts begin
      foreach (cfg.chip_vif.alerts_if.alerts_cb.alerts[i]) begin
        if (cfg.chip_vif.alerts_if.alerts_cb.alerts[i]) cov.alert_cg_wrap[i].sample(1'b1);
      end
    end
  endtask

  virtual task process_jtag_fifo();
    jtag_riscv_item item;
    forever begin
      jtag_fifo.get(item);
      `uvm_info(`gfn, $sformatf("received jtag item:\n%0s", item.sprint()), UVM_HIGH)
    end
  endtask

  // TODO, may add some checking later
  virtual task process_tl_access(tl_seq_item item, tl_channels_e channel, string ral_name);
    return;
  endtask

  virtual function void reset(string kind = "HARD");
    super.reset(kind);
    eg2_cheriot_mode = 1'b0;
    eg2_cheriot_ena_written = prim_mubi_pkg::MuBi4False;

    // If there is a JTAG DTM register model (created by calling cfg.set_use_jtag_dmi) then reset
    // its values to match the reset event.
    if (cfg.m_jtag_dtm_ral != null) begin
      cfg.m_jtag_dtm_ral.reset(kind);
    end

    // reset local fifos queues and variables
  endfunction

  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    // post test checks - ensure that all local fifos and queues are empty
  endfunction

  virtual protected function bit predict_tl_err(tl_seq_item item,
                                                tl_channels_e channel,
                                                string ral_name);
    uvm_reg_addr_t addr = cfg.ral_models[ral_name].get_normalized_addr(item.a_addr);
    uvm_mem mem = cfg.ral_models[ral_name].default_map.get_mem_by_offset(addr);

    // any access to dv_sim_window returns d_error since this is a DV mem
    // if stub_cpu is off, we force connect this DV mem with a sim_sram, accessing the DV mem is to
    // passing info from C to SV, which won't set d_error
    if (mem != null && mem.get_name() == "dv_sim_window" && cfg.chip_vif.stub_cpu) begin
      if (channel == DataChannel) `DV_CHECK_EQ(item.d_error, 1)
      return 1;
    end
    // The execution mode: CHERIOT_ENA's written value, then the CHERIOT_LOCK write that locks it.
    if (channel == AddrChannel && item.is_write() &&
        item.a_addr == ral.rv_core_ibex_cfg.cheriot_ena.get_address()) begin
      eg2_cheriot_ena_written = item.a_data[3:0];
    end
    if (channel == AddrChannel && item.is_write() &&
        item.a_addr == ral.rv_core_ibex_cfg.cheriot_lock.get_address() &&
        item.a_data[3:0] == prim_mubi_pkg::MuBi4True &&
        eg2_cheriot_ena_written == prim_mubi_pkg::MuBi4True) begin
      eg2_cheriot_mode = 1'b1;
    end
    // cheriot_mem_sys theory of operation: the revocation bitmap window "returns a TL-UL error
    // while the system is in ePMP mode" (2026-10-27: chip_sw_eg2_mem_test's bitmap load and store
    // in RISC-V mode fault as documented, and the base scoreboard predicted no error).
    if (mem != null && mem.get_name() == "revbm" && !eg2_cheriot_mode) begin
      if (channel == DataChannel) begin
        `DV_CHECK_EQ(item.d_error, 1,
                     "cheriot_mem_sys.revbm access in ePMP mode: a TL-UL error is documented")
      end
      return 1;
    end
    // A bench fault behind this monitor (chip_sw_eg2_rand_vseq::announce_fault, target "chip_tl"):
    // the memory answers the corrupted request with an error that the monitor's clean copy of the
    // request does not explain (2026-10-29, chip_cheriot_fi_meta: a data-integrity flip at
    // cheriot_mem_sys.cored_tl_d_i). Decided before the base prediction, which compares d_error
    // itself and reports the mismatch inside (48bd34e111 asked afterwards, too late).
    if (channel == DataChannel && item.d_error && eg2_inject_registry::in_use()) begin
      string src = eg2_inject_registry::consult("chip_tl");
      if (src != "") begin
        `uvm_info(`gfn, $sformatf("TL-UL error at 0x%08x EXPECTED (injected by %s)", item.a_addr,
                                  src), UVM_LOW)
        return 1'b1;
      end
    end
    return super.predict_tl_err(item, channel, ral_name);
  endfunction

endclass
