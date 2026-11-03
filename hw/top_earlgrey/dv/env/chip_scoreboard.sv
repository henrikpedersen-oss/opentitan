// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

typedef class eg2_iss_wrapper;

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

  // ---- The bus errors of RRAM host reads (rram_ctrl documentation) ----
  // rram_ctrl's memory-protection registers as software wrote them (registers.md: reset values,
  // REGION_CFG_REGWEN / EMUL_INFO_REGWEN rw0c locks, DIS rw1s), followed from the core's writes
  // that were answered without an error, cleared when rram_ctrl is reset. A host read is answered
  // with a TL-UL error (theory_of_operation.md "Access attributes", "Data partition", "RRAM
  // Escalation & Disable"; programmers_guide.md "Memory-Mapped Reads via host_tl", "Error
  // Encountered by Host Direct Read"; README.md "Address Map") when:
  //   - RRAM is disabled: DIS.SW_DIS not MuBi4False, the global escalation (lc_escalate_en), or a
  //     latched local escalation;
  //   - the word was made uncorrectable (cfg.rram_uncorr_words);
  //   - the page is in the OTP region (the top 6 pages, 4090-4095: "not reachable");
  //   - the page is in an enabled emulated info window ("Host access to this window is always
  //     blocked");
  //   - the page's rule has no RD_EN: the lowest-indexed enabled MP region holding the page, else
  //     DEFAULT_REGION ("host_tl reads are subject to the same memory protection").
  // MuBi4 fields count as set only at MuBi4True; DIS's fields as set at anything but MuBi4False
  // (registers.md DIS).
  localparam bit [31:0] RramHostBase = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR;
  localparam bit [31:0] RramHostSize = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES;
  localparam bit [31:0] RramCoreBase = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR;
  localparam bit [31:0] RramCoreSize = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_SIZE_BYTES;
  localparam bit [31:0] SramMainBase = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
  localparam bit [31:0] SramMainSize = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES;
  localparam int unsigned RramPageBytes    = 512;   // README.md "Address Map": 32 words of 16 bytes
  localparam int unsigned RramOtpFirstPage = 4090;  // README.md "Address Map": OTP integrity page
  localparam int unsigned NumMpRegions = rram_ctrl_reg_pkg::NumRegions;
  localparam int unsigned NumEmulWins  = rram_ctrl_reg_pkg::NumEmulInfoRegions;

  protected bit        rram_region_regwen[NumMpRegions];
  protected bit [19:0] rram_mp_region_cfg[NumMpRegions];
  protected bit [23:0] rram_mp_region[NumMpRegions];
  protected bit [15:0] rram_default_region;
  protected bit        rram_emul_regwen[NumEmulWins];
  protected bit [27:0] rram_emul_region[NumEmulWins];
  protected bit [7:0]  rram_dis;
  // A local escalation, latched until rram_ctrl's reset ("RRAM Escalation & Disable"): an
  // uncorrectable host read once DIS.LOCAL_ESC_RELBL_ERR is set (FAULT_STATUS.PHY_RELBL_ERR), or a
  // bench fault its sequence reports (cfg.tl_fault_blocks_rram).
  protected bit        rram_local_esc;
  // The error each outstanding core request is expected to get from the RRAM or main SRAM rules,
  // decided when the request was made (its A channel), by source ID (the monitor hands a copy to the
  // D channel); the reason.
  protected string     eg2_exp_err[bit [top_pkg::TL_AIW-1:0]];

  // ---- Capability stores to the NVM (cheriot_mem_sys theory_of_operation.md "Write-to-Read-and-
  // Compare Filter") ----
  // In CHERIoT mode a write to the NVM with the tag sideband set (cfg.chip_vif.cored_tag_by_source)
  // is not written but compared with the NVM's word: W0 (the lower word) is answered with an error
  // if the NVM read fails, the store is not a full-word PutFullData, is out of sequence, or its data
  // differ from the NVM's; W1 (W0's address + 4) with an error unless W0 and W1 both matched. The
  // NVM's words are the reference model's (eg2_iss.nv_word_now). Without one, no prediction is
  // made here and the base scoreboard predicts the read-only window's error, as before.
  eg2_iss_wrapper      eg2_iss;
  protected bit        wtrc_open;      // a W0 taken, its W1 not yet
  protected bit [31:0] wtrc_w0_addr;
  protected bit        wtrc_w0_known;  // W0's answer was predicted
  protected bit        wtrc_w0_ok;
  // The no-error answer each outstanding capability store is expected to get, by source ID.
  protected string     eg2_exp_ok[bit [top_pkg::TL_AIW-1:0]];

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    jtag_fifo = new("jtag_fifo", this);
    rram_model_reset();
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
      follow_device_resets();
    join_none
  endtask

  // rram_ctrl's and main sram_ctrl's own resets, which need not be a power-on reset (an escalation
  // reset, a low-power exit): they clear what the TL-UL error prediction keeps for each.
  virtual task follow_device_resets();
    fork
      forever begin
        @(negedge cfg.chip_vif.rram_ctrl_rst_n);
        rram_model_reset();
      end
      forever begin
        @(negedge cfg.chip_vif.sram_main_rst_n);
        cfg.tl_fault_blocks_sram_main = 1'b0;
      end
    join
  endtask

  protected function void rram_model_reset();
    foreach (rram_region_regwen[i]) begin
      rram_region_regwen[i] = 1'b1;
      rram_mp_region_cfg[i] = 20'h99999;
      rram_mp_region[i]     = '0;
    end
    rram_default_region = 16'h9999;
    foreach (rram_emul_regwen[i]) begin
      rram_emul_regwen[i] = 1'b1;
      rram_emul_region[i] = 28'h9000000;
    end
    rram_dis = 8'h99;
    rram_local_esc = 1'b0;
    cfg.tl_fault_blocks_rram = 1'b0;
  endfunction

  // A core write to rram_ctrl's registers that was answered without an error.
  protected function void rram_follow_write(tl_seq_item item);
    bit [31:0] off = item.a_addr - RramCoreBase;
    bit [31:0] d = item.a_data;
    if (item.a_mask != 4'hf) return;  // the registers are written whole by the software
    for (int i = 0; i < NumMpRegions; i++) begin
      if (off == rram_ctrl_reg_pkg::RRAM_CTRL_REGION_CFG_REGWEN_0_OFFSET + 4 * i) begin
        rram_region_regwen[i] &= d[0];  // rw0c
      end else if (off == rram_ctrl_reg_pkg::RRAM_CTRL_MP_REGION_CFG_0_OFFSET + 4 * i) begin
        if (rram_region_regwen[i]) rram_mp_region_cfg[i] = d[19:0];
      end else if (off == rram_ctrl_reg_pkg::RRAM_CTRL_MP_REGION_0_OFFSET + 4 * i) begin
        if (rram_region_regwen[i]) rram_mp_region[i] = d[23:0];
      end
    end
    for (int i = 0; i < NumEmulWins; i++) begin
      if (off == rram_ctrl_reg_pkg::RRAM_CTRL_EMUL_INFO_REGWEN_0_OFFSET + 4 * i) begin
        rram_emul_regwen[i] &= d[0];  // rw0c
      end else if (off == rram_ctrl_reg_pkg::RRAM_CTRL_EMUL_INFO_REGION_0_OFFSET + 4 * i) begin
        if (rram_emul_regwen[i]) rram_emul_region[i] = d[27:0];
      end
    end
    if (off == rram_ctrl_reg_pkg::RRAM_CTRL_DEFAULT_REGION_OFFSET) rram_default_region = d[15:0];
    if (off == rram_ctrl_reg_pkg::RRAM_CTRL_DIS_OFFSET) rram_dis |= d[7:0];  // rw1s
  endfunction

  protected function bit in_page_range(int unsigned page, bit [11:0] base, bit [11:0] size);
    return page >= base && page <= int'(base) + int'(size);
  endfunction

  // Why a host read at `addr` (in the host window) is answered with an error; "" if it is not.
  protected function string rram_host_rd_err(bit [31:0] addr);
    bit [31:0] off = addr - RramHostBase;
    int unsigned page = off / RramPageBytes;
    if (rram_dis[3:0] != prim_mubi_pkg::MuBi4False) return "RRAM disabled by DIS.SW_DIS";
    if (cfg.chip_vif.rram_ctrl_escalated) return "RRAM disabled by the global escalation";
    if (rram_local_esc || cfg.tl_fault_blocks_rram) return "RRAM disabled by a local escalation";
    if (cfg.rram_uncorr_words.exists({off[31:4], 4'h0})) return "uncorrectable RRAM word";
    if (page >= RramOtpFirstPage) return $sformatf("page %0d is in the OTP region", page);
    for (int i = 0; i < NumEmulWins; i++) begin
      if (rram_emul_region[i][27:24] == prim_mubi_pkg::MuBi4True &&
          in_page_range(page, rram_emul_region[i][11:0], rram_emul_region[i][23:12])) begin
        return $sformatf("page %0d is in emulated info window %0d", page, i);
      end
    end
    for (int i = 0; i < NumMpRegions; i++) begin
      if (rram_mp_region_cfg[i][3:0] == prim_mubi_pkg::MuBi4True &&
          in_page_range(page, rram_mp_region[i][11:0], rram_mp_region[i][23:12])) begin
        return rram_mp_region_cfg[i][7:4] == prim_mubi_pkg::MuBi4True ? "" :
               $sformatf("page %0d: MP region %0d has no RD_EN", page, i);
      end
    end
    return rram_default_region[3:0] == prim_mubi_pkg::MuBi4True ? "" :
           $sformatf("page %0d: DEFAULT_REGION has no RD_EN", page);
  endfunction

  // One word of a capability store to the NVM: whether its answer can be predicted (`known`), and
  // why it fails ("" if it matches).
  protected function string wtrc_word_err(tl_seq_item item, output bit known);
    bit [31:0] nv;
    string     rd_err;
    known = 1'b1;
    if (item.a_opcode != tlul_pkg::PutFullData || item.a_mask != 4'hf) begin
      return "not a full-word PutFullData";
    end
    // The store becomes a Get of the word through the host port.
    rd_err = rram_host_rd_err(item.a_addr);
    if (rd_err != "") return {"the NVM read fails: ", rd_err};
    if (eg2_iss == null || !eg2_iss.nv_word_now({item.a_addr[31:2], 2'b00}, nv)) begin
      known = 1'b0;
      return "";
    end
    if (nv != item.a_data) return $sformatf("its data %08x differ from the NVM's %08x", item.a_data, nv);
    return "";
  endfunction

  // A core write into the RRAM host window, at its request: the capability store's answer (above).
  protected function void wtrc_predict(tl_seq_item item);
    bit    known;
    string why;
    if (!(eg2_cheriot_mode && cfg.chip_vif.cored_tag_by_source.exists(item.a_source) &&
          cfg.chip_vif.cored_tag_by_source[item.a_source])) begin
      return;  // an integer store: the read-only window's error (the base prediction)
    end
    if (item.a_addr[2] == 1'b0) begin  // W0 opens a capability
      why = wtrc_word_err(item, known);
      wtrc_open     = 1'b1;
      wtrc_w0_addr  = item.a_addr;
      wtrc_w0_known = known;
      wtrc_w0_ok    = known && why == "";
    end else begin                     // W1
      if (!wtrc_open || item.a_addr != wtrc_w0_addr + 4) begin
        known = 1'b1;
        why   = "W1 out of sequence";
      end else begin
        why = wtrc_word_err(item, known);
        known &= wtrc_w0_known;
        if (why == "" && !wtrc_w0_ok) why = "its W0 did not match";
      end
      wtrc_open = 1'b0;
    end
    if (!known) begin
      `uvm_info(`gfn, $sformatf({"capability store to the NVM at 0x%08x: the reference model has no ",
                                 "word there, the read-only window's error is predicted"},
                                item.a_addr), UVM_LOW)
      return;
    end
    if (why != "") eg2_exp_err[item.a_source] = {"capability store to the NVM: ", why};
    else eg2_exp_ok[item.a_source] = "capability store to the NVM matching its words";
  endfunction

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
    rram_model_reset();
    cfg.tl_fault_blocks_sram_main = 1'b0;
    eg2_exp_err.delete();
    eg2_exp_ok.delete();
    wtrc_open = 1'b0;

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
    // The RRAM host-read rules, capability stores to the NVM and main SRAM's escalation (above):
    // decided at the request, checked at its answer.
    if (channel == AddrChannel) begin
      string why;
      eg2_exp_ok.delete(item.a_source);
      eg2_exp_err.delete(item.a_source);
      if (item.a_opcode == tlul_pkg::Get && item.a_addr >= RramHostBase &&
          item.a_addr - RramHostBase < RramHostSize) begin
        why = rram_host_rd_err(item.a_addr);
      end else if (item.is_write() && item.a_addr >= RramHostBase &&
                   item.a_addr - RramHostBase < RramHostSize) begin
        wtrc_predict(item);  // fills eg2_exp_err / eg2_exp_ok itself
      end else if (item.a_addr >= SramMainBase && item.a_addr - SramMainBase < SramMainSize) begin
        // sram_ctrl interfaces.md "Global and Local Escalation": once lc_escalate_en is not Off,
        // or after a bus integrity fault (local escalation), every later memory request is blocked
        // until the controller's reset.
        if (cfg.chip_vif.sram_main_escalated) begin
          why = "main SRAM blocked by the global escalation";
        end else if (cfg.tl_fault_blocks_sram_main) begin
          why = "main SRAM blocked by its local escalation after a bus integrity fault";
        end
      end
      if (why != "") eg2_exp_err[item.a_source] = why;
    end
    if (channel == DataChannel) begin
      string why = eg2_exp_err.exists(item.d_source) ? eg2_exp_err[item.d_source] : "";
      string ok  = eg2_exp_ok.exists(item.d_source) ? eg2_exp_ok[item.d_source] : "";
      eg2_exp_err.delete(item.d_source);
      eg2_exp_ok.delete(item.d_source);
      // A bench fault behind this monitor (chip_sw_eg2_rand_vseq::announce_fault, target
      // "chip_tl"): the memory answers the corrupted request with an error that the monitor's clean
      // copy of the request does not explain (2026-10-29, chip_cheriot_fi_meta: a data-integrity
      // flip at cheriot_mem_sys.cored_tl_d_i). Decided before the base prediction, which compares
      // d_error itself and reports the mismatch inside (48bd34e111 asked afterwards, too late).
      if (item.d_error && eg2_inject_registry::in_use()) begin
        string src = eg2_inject_registry::consult("chip_tl");
        if (src != "") begin
          `uvm_info(`gfn, $sformatf("TL-UL error at 0x%08x EXPECTED (injected by %s)", item.a_addr,
                                    src), UVM_LOW)
          return 1'b1;
        end
      end
      if (why != "") begin
        `DV_CHECK_EQ(item.d_error, 1,
                     $sformatf("core access at 0x%08x: %s, a TL-UL error is documented",
                               item.a_addr, why))
        `uvm_info(`gfn, $sformatf("TL-UL error at 0x%08x expected: %s", item.a_addr, why),
                  UVM_MEDIUM)
        // FAULT_STATUS.PHY_RELBL_ERR with DIS.LOCAL_ESC_RELBL_ERR set: RRAM disabled until reset.
        if (why == "uncorrectable RRAM word" && rram_dis[7:4] != prim_mubi_pkg::MuBi4False) begin
          rram_local_esc = 1'b1;
          `uvm_info(`gfn, "RRAM local escalation (PHY_RELBL_ERR, LOCAL_ESC_RELBL_ERR set)", UVM_LOW)
        end
        return 1'b1;
      end
      if (ok != "") begin
        `DV_CHECK_EQ(item.d_error, 0,
                     $sformatf("core write at 0x%08x: %s, no TL-UL error is documented",
                               item.a_addr, ok))
        return 1'b1;
      end
      if (item.is_write() && !item.d_error && item.a_addr >= RramCoreBase &&
          item.a_addr - RramCoreBase < RramCoreSize) begin
        rram_follow_write(item);
      end
    end
    return super.predict_tl_err(item, channel, ral_name);
  endfunction

endclass
