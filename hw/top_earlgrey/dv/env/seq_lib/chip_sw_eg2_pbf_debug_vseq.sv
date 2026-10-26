// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Debug after boot (eg2_top_verification spec REQ_PBF_20, "Debug Controller":
// chip_sw_eg2_pbf_debug_sb and chip_sw_eg2_pbf_debug_fault; software
// sw/device/tests/eg2_pbf/eg2_pbf_debug_*.c). The JTAG agent drives rv_dm through the debugger
// model (cfg.debugger); the runs need +use_jtag_dmi=1. The mailbox is the first word of retention
// SRAM above retention_sram_t (base + 0x1000), the halt count the word after it.

// chip_sw_eg2_pbf_debug_sb: halt, an operation, resume, while the owner image computes.
class chip_sw_eg2_pbf_debug_sb_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_debug_sb_vseq)
  `uvm_object_new

  rand bit [1:0]  halt_point;  // 0 early, 1 middle, 2 late, 3 drawn
  rand bit [1:0]  operation;   // 0 halt/resume, 1 register read, 2 SBA read, 3 SBA write
  rand bit [31:0] mailbox_value;
  rand int unsigned halt_delay;

  constraint knobs_c {
    mailbox_value != 0;
    halt_delay inside {[0 : 60000]};
  }

  localparam bit [31:0] Mailbox = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 'h1000;

  virtual function void write_params();
    set_param("halt_point", halt_point);
    set_param("operation", operation);
    set_param("mailbox_value", mailbox_value);
  endfunction

  virtual task pre_start();
    cfg.chip_vif.tap_straps_if.drive(JtagTapRvDm);
    super.pre_start();
  endtask

  virtual task body();
    bit [31:0] hp = pinned("halt_point", halt_point), op = pinned("operation", operation),
               mv = pinned("mailbox_value", mailbox_value);
    if (cfg.rom_ext_assembled) begin
      // The signed image's defaults (eg2_pbf_debug_sb.c).
      hp = 1; op = 3; mv = 32'h0dbeef01;
    end
    if (lc_debug_enabled()) begin
      add_activity("core in debug mode", {PdMain, ".u_rv_core_ibex.u_core.u_ibex_core.debug_mode"});
      add_activity("rv_dm debug request", {PdMain, ".u_rv_dm.debug_req_o"});
    end
    super.body();
    fork debug_session(hp, op, mv); join_none
  endtask

  protected task debug_session(bit [31:0] hp, bit [31:0] op, bit [31:0] mv);
    rv_core_csr_dcsr_t dcsr;
    sba_access_err_e sba_status;
    abstract_cmd_err_e status;
    logic [31:0] q[$];
    wait_bench_step(1);
    if (!lc_debug_enabled()) begin
      `uvm_info(`gfn, $sformatf({"EG2_PBF debug disabled in %s: no access; the refusal is ",
                                 "chip_rv_dm_lc_disabled's check"}, cfg.use_otp_image.name()),
                UVM_LOW)
      return;
    end
    case (hp)
      0: ;
      1: cfg.chip_vif.cpu_clk_rst_if.wait_clks(20000);
      2: cfg.chip_vif.cpu_clk_rst_if.wait_clks(60000);
      default: cfg.chip_vif.cpu_clk_rst_if.wait_clks(halt_delay);
    endcase
    cfg.debugger.set_dmactive(1);
    cfg.debugger.set_haltreq(1);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_haltreq(0);
    cfg.debugger.read_dcsr(dcsr);
    `DV_CHECK_EQ(dcsr.cause, RvDebugCauseHaltReq, "EG2_PBF dcsr.cause after haltreq")
    case (op)
      1: begin
        cfg.debugger.abstract_cmd_reg_read(.regno('h1000), .value_q(q), .status(status), .size(16));
        `DV_CHECK_EQ(status, AbstractCmdErrNone, "EG2_PBF abstract register read")
      end
      2: begin
        cfg.debugger.sba_read(Mailbox, q, sba_status);
        `DV_CHECK_EQ(sba_status, SbaErrNone, "EG2_PBF SBA read")
        `DV_CHECK_EQ(q[0], 0, "EG2_PBF mailbox before the debugger wrote it")
      end
      3: begin
        logic [31:0] v[$];
        v.push_back(mv ^ 32'hffff_ffff);
        cfg.debugger.sba_write(Mailbox + 8, v, sba_status);
        `DV_CHECK_EQ(sba_status, SbaErrNone, "EG2_PBF SBA write")
        cfg.debugger.sba_read(Mailbox + 8, q, sba_status);
        `DV_CHECK_EQ(q[0], v[0], "EG2_PBF SBA write read back")
      end
      default: ;
    endcase
    cfg.debugger.set_resumereq(1);
    cfg.debugger.wait_cpu_resumed();
    cfg.debugger.set_resumereq(0);
    begin
      logic [31:0] v[$];
      v.push_back(1);
      cfg.debugger.sba_write(Mailbox + 4, v, sba_status);
      v[0] = mv;
      cfg.debugger.sba_write(Mailbox, v, sba_status);
      `DV_CHECK_EQ(sba_status, SbaErrNone, "EG2_PBF SBA mailbox write")
    end
    `uvm_info(`gfn, "EG2_PBF debug session done: halted, operation, resumed, mailbox written",
              UVM_LOW)
  endtask
endclass

// chip_sw_eg2_pbf_debug_fault: an ndm reset or an escalation while halted or while SBA is busy.
class chip_sw_eg2_pbf_debug_fault_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_debug_fault_vseq)
  `uvm_object_new

  rand bit        fault_point;  // 0 halted, 1 SBA write in flight
  rand bit        fault;        // 0 ndm reset, 1 escalation
  rand bit [31:0] esc_cycles;
  rand bit [31:0] mailbox_value;

  constraint knobs_c {
    // Long enough for the bench to halt the core first (io_div4 cycles, about 41 ns).
    esc_cycles inside {[20000 : 50000]};
    mailbox_value != 0;
  }

  localparam bit [31:0] Mailbox = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 'h1000;

  virtual function void write_params();
    set_param("fault_point", fault_point);
    set_param("fault", fault);
    set_param("esc_cycles", esc_cycles);
    set_param("mailbox_value", mailbox_value);
  endfunction

  virtual task pre_start();
    cfg.chip_vif.tap_straps_if.drive(JtagTapRvDm);
    super.pre_start();
  endtask

  virtual task body();
    bit [31:0] fp = pinned("fault_point", fault_point), f = pinned("fault", fault),
               mv = pinned("mailbox_value", mailbox_value);
    super.body();
    fork session(fp, f, mv); join_none
  endtask

  protected task session(bit [31:0] fp, bit [31:0] f, bit [31:0] mv);
    uvm_reg_data_t data;
    wait_bench_step(1);
    if (!lc_debug_enabled()) begin
      `uvm_info(`gfn, "EG2_PBF debug disabled: the software resets itself", UVM_LOW)
      return;
    end
    cfg.debugger.set_dmactive(1);
    cfg.debugger.set_haltreq(1);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_haltreq(0);
    fork
      begin : sba
        if (fp) begin
          sba_access_err_e st;
          logic [31:0] v[$];
          v.push_back(mv);
          cfg.debugger.sba_write(Mailbox, v, st);
        end
      end
      begin : fault_thread
        if (f == 0) begin
          if (fp) cfg.chip_vif.cpu_clk_rst_if.wait_clks($urandom_range(0, 20));
          void'(announce_fault("chip_sw_eg2_pbf_debug_fault_vseq", "insn", "rv_dm ndm reset"));
          csr_wr(.ptr(jtag_dmi_ral.dmcontrol.ndmreset), .value(1), .blocking(1), .predict(1));
        end
        // The reset: the ndm reset, or the escalation the software armed.
        wait (cfg.chip_vif.cpu_rst_n === 1'b0);
      end
    join_any
    wait (cfg.chip_vif.cpu_rst_n === 1'b0);
    disable fork;
    cfg.sw_test_status_vif.sw_test_status = SwTestStatusUnderReset;
    if (f == 0) begin
      csr_wr(.ptr(jtag_dmi_ral.dmcontrol.ndmreset), .value(0), .blocking(1), .predict(1));
    end
    wait (cfg.chip_vif.cpu_rst_n === 1'b1);
    csr_rd(.ptr(jtag_dmi_ral.dmcontrol), .value(data), .blocking(1));
    if (f == 1) begin
      // After the escalation reset the debug module is back at its reset state (spec row).
      `DV_CHECK_EQ(dv_base_reg_pkg::get_field_val(jtag_dmi_ral.dmcontrol.dmactive, data), 0,
                   "EG2_PBF dmcontrol.dmactive after the escalation reset")
    end else begin
      // An ndm reset resets everything but the debug module (RISC-V debug spec, ndmreset).
      `DV_CHECK_EQ(dv_base_reg_pkg::get_field_val(jtag_dmi_ral.dmcontrol.dmactive, data), 1,
                   "EG2_PBF dmcontrol.dmactive after the ndm reset")
      `DV_SPINWAIT(
        do csr_rd(.ptr(jtag_dmi_ral.dmstatus), .value(data), .blocking(1));
        while (dv_base_reg_pkg::get_field_val(jtag_dmi_ral.dmstatus.allrunning, data) == 0);
      )
    end
    `uvm_info(`gfn, "EG2_PBF debug module state after the fault checked", UVM_LOW)
  endtask
endclass
