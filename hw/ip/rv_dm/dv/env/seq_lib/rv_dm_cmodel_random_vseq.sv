// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Constrained-random traffic for rv_dm against the debug module reference model (rv_dm_cmodel_sb,
// +rv_dm_cmodel=1): random DMI writes and reads of every implemented DMI register, with dmcontrol,
// command, sbcs and the SBA registers constrained towards meaningful values; a core emulated on the
// debug memory as the Debug ROM's protocol has it (Halted on debug_req; Going, an occasional
// Exception and Halted for a command; Resuming for a resume), with random reads of data, program
// buffer and flags; random system bus responses with errors. Only life-cycle states the late debug
// enable table documents as fully ungated: TEST_UNLOCKED/RMA, and DEV with late debug enabled by
// the OTP fuse or by LATE_DEBUG_ENABLE. In the CHERIoT build (RV_DM_CHERIOT_SUPPORT) cheriot_ena
// is random and commands use aarsize 3 too.
class rv_dm_cmodel_random_vseq extends rv_dm_base_vseq;
  `uvm_object_utils(rv_dm_cmodel_random_vseq)

  rand int unsigned num_ops;
  constraint num_ops_c {
    num_ops inside {[200:800]};
  }

  // TEST_UNLOCKED/RMA (lc_dft_en with lc_hw_debug_en) or DEV (lc_hw_debug_en only); late debug as
  // the base vseq sets it (on), from the fuse or the register.
  constraint lc_dft_en_c {
    lc_dft_en dist {1 :/ 1, 0 :/ 1};
  }
`ifdef RV_DM_CHERIOT_SUPPORT
  constraint no_cheriot_ena_c {
    cheriot_ena dist {1 :/ 3, 0 :/ 1};
  }
`endif

  // The emulated core: halted as far as it has told the debug module.
  protected bit core_halted;
  protected bit stop_core, core_stopped;  // body asks emulate_core to stop between two accesses

  // +rv_dm_cmodel_mask_known=1: rv_dm starts a command (written, or by autoexec) while cmderr is not
  // 0, which the Debug spec forbids (3.12.7, finding "A command runs while cmderr is not 0"); after
  // one the model and rv_dm no longer agree on anything. With the known findings masked, the
  // debugger clears cmderr first (as a debugger following the spec would) and the run counts it.
  protected bit          mask_known;
  protected bit          autoexec_on;  // the last abstractauto write set an autoexec bit
  protected int unsigned n_cmderr_clears;
  protected int unsigned n_sberror_whole;  // sbcs writes whose sberror field was made all-or-nothing
  protected int unsigned n_cmd_changed;    // commands whose aarsize (transfer 0) or postincrement was changed

  `uvm_object_new

  virtual task sba_tl_device_seq_start(int min_rsp_delay = 0,
                                       int max_rsp_delay = 80,
                                       int rsp_abort_pct = 25,
                                       int d_error_pct = 0,
                                       int d_chan_intg_err_pct = 0);
    // Every request answered (an abort would leave sbbusy set for good), one in ten with an error.
    super.sba_tl_device_seq_start(min_rsp_delay, max_rsp_delay, 0, 10, 0);
  endtask

  task body();
    uvm_reg dmi_regs[$];
    void'($value$plusargs("rv_dm_cmodel_mask_known=%b", mask_known));
    jtag_dmi_ral.get_registers(dmi_regs);
    fork
      emulate_core();
    join_none
    repeat (num_ops) begin
      if (!cfg.clk_rst_vif.rst_n) break;
      randcase
        5: random_dmi_write(dmi_regs);
        4: random_dmi_read(dmi_regs);
        2: core_read();
        1: cfg.clk_rst_vif.wait_clks($urandom_range(1, 200));
      endcase
    end
    // Let the core and the bus finish what is in flight before the end-of-test checks.
    cfg.clk_rst_vif.wait_clks(2000);
    // One more DMI access: the JTAG DMI monitor reports the last one only when the next scan reads
    // its status, so without it the model would never see the final write.
    begin
      uvm_reg_data_t v;
      csr_rd(.ptr(jtag_dmi_ral.dmstatus), .value(v), .blocking(1'b1));
    end
    stop_core = 1'b1;
    `DV_SPINWAIT(wait(core_stopped);, "the emulated core did not stop", 1_000_000)
    if (mask_known) begin
      `uvm_info(`gfn, $sformatf({"known RTL findings masked: cmderr cleared before %0d commands or ",
                                 "buffer accesses (a command runs while cmderr is not 0); %0d sbcs ",
                                 "writes clear all of sberror or none (sberror is cleared as a whole); ",
                                 "%0d commands with aarsize 2 for transfer 0 or no aarpostincrement"},
                                n_cmderr_clears, n_sberror_whole, n_cmd_changed), UVM_NONE)
    end
    disable fork;
  endtask

  // ---- The debugger ----

  protected function bit [31:0] dmcontrol_value();
    bit [31:0] v = $urandom();
    v[0]     = $urandom_range(0, 19) != 0;   // dmactive: rarely 0 (resets the module)
    v[1]     = $urandom_range(0, 19) == 0;   // ndmreset: rarely
    v[25:6]  = 0;                            // hartsel: hart 0 (the one hart)
    v[26]    = 0;                            // hasel
    return v;
  endfunction

  protected function bit [31:0] command_value();
    bit [31:0] v = 0;
    int unsigned regno_kind = $urandom_range(0, 9);
    v[31:24] = $urandom_range(0, 9) == 0 ? $urandom_range(1, 255) : 0;  // cmdtype
`ifdef RV_DM_CHERIOT_SUPPORT
    v[22:20] = $urandom_range(0, 9) < 6 ? 2 : $urandom_range(0, 9) < 8 ? 3 : $urandom_range(0, 7);
`else
    v[22:20] = $urandom_range(0, 9) < 8 ? 2 : $urandom_range(0, 7);
`endif
    v[19]    = $urandom_range(0, 3) == 0;  // aarpostincrement
    v[18]    = $urandom_range(0, 3) == 0;  // postexec
    v[17]    = $urandom_range(0, 9) != 0;  // transfer
    v[16]    = $urandom();                 // write
    v[15:0]  = regno_kind < 6 ? 16'h1000 + $urandom_range(0, 31) :
               regno_kind < 8 ? $urandom_range(0, 31) : $urandom_range(0, 16'h0fff);
    if (mask_known) begin
      // rv_dm refuses aarsize above the register even with transfer 0 (finding "aarsize is checked
      // when transfer is 0"; Debug spec 3.6.1.1: aarsize does not matter then), and whether it
      // supports aarpostincrement is not documented (rv_dm refuses it): neither can be predicted.
      if (!v[17] && v[22:20] > 2) begin
        v[22:20] = 2;
        n_cmd_changed++;
      end
      if (v[19]) begin
        v[19] = 1'b0;
        n_cmd_changed++;
      end
    end
    return v;
  endfunction

  protected function bit [31:0] sbcs_value();
    bit [31:0] v = $urandom();
    v[19:17] = $urandom_range(0, 9) < 8 ? $urandom_range(0, 2) : $urandom_range(3, 7);
    // rv_dm clears all of sberror when any of its bits is written 1; the Debug spec clears only the
    // bits written 1 (finding "sberror is cleared as a whole"). With the known findings masked the
    // debugger clears all or none, and the run counts the writes it changed.
    if (mask_known && v[14:12] != 0 && v[14:12] != 3'b111) begin
      v[14:12] = 3'b111;
      n_sberror_whole++;
    end
    return v;
  endfunction

  protected function bit [31:0] sbaddress_value();
    bit [31:0] a = $urandom();
    if ($urandom_range(0, 3) != 0) a[1:0] = 0;
    return a;
  endfunction

  // A DMI register, weighted towards the ones that drive the debug module: dmcontrol, command and
  // the abstract and system bus registers; the rest of the map one time in six.
  protected function uvm_reg pick_reg(uvm_reg dmi_regs[$]);
    string name;
    randcase
      20: name = "dmcontrol";
      12: name = "command";
      5:  name = "abstractcs";
      4:  name = "abstractauto";
      8:  name = $sformatf("abstractdata_%0d", $urandom_range(0, 1));
      8:  name = $sformatf("progbuf_%0d", $urandom_range(0, 7));
      7:  name = "sbcs";
      7:  name = "sbaddress0";
      7:  name = "sbdata0";
      6:  name = "dmstatus";
      16: name = "";
    endcase
    foreach (dmi_regs[i]) if (dmi_regs[i].get_name() == name) return dmi_regs[i];
    return dmi_regs[$urandom_range(0, dmi_regs.size() - 1)];
  endfunction

  // data0-11 or progbuf0-15: an access may start the command again (autoexec).
  protected function bit is_buffer(string name);
    return name.len() > 8 && (name.substr(0, 11) == "abstractdata" || name.substr(0, 6) == "progbuf");
  endfunction

  // Clears cmderr once no command is running (a write of abstractcs while busy sets cmderr busy).
  protected task clear_cmderr();
    uvm_reg_data_t v;
    for (int i = 0; i < 50; i++) begin
      csr_rd(.ptr(jtag_dmi_ral.abstractcs), .value(v), .blocking(1'b1));
      if (!v[12]) break;
      cfg.clk_rst_vif.wait_clks(20);
    end
    if (v[10:8] == 0) return;
    csr_wr(.ptr(jtag_dmi_ral.abstractcs), .value(32'h0000_0700), .predict(1'b0), .blocking(1'b1));
    n_cmderr_clears++;
  endtask

  protected task random_dmi_write(uvm_reg dmi_regs[$]);
    uvm_reg r = pick_reg(dmi_regs);
    bit [31:0] v;
    if (mask_known && (r.get_name() == "command" || (autoexec_on && is_buffer(r.get_name())))) begin
      clear_cmderr();
    end
    case (r.get_name())
      "dmcontrol":  v = dmcontrol_value();
      "command":    v = command_value();
      "sbcs":       v = sbcs_value();
      "sbaddress0": v = sbaddress_value();
      "abstractauto": v = $urandom_range(0, 3) == 0 ? $urandom() : 0;
      default:      v = $urandom();
    endcase
    if (r.get_name() == "abstractauto") autoexec_on = (v & 32'hffff_0fff) != 0;
    csr_wr(.ptr(r), .value(v), .predict(1'b0), .blocking(1'b1));
  endtask

  protected task random_dmi_read(uvm_reg dmi_regs[$]);
    uvm_reg_data_t v;
    uvm_reg r = pick_reg(dmi_regs);
    if (mask_known && autoexec_on && is_buffer(r.get_name())) clear_cmderr();
    csr_rd(.ptr(r), .value(v), .blocking(1'b1));
  endtask

  // ---- The core, on the debug memory ----

  protected task core_read();
    uvm_reg_data_t v;
    int unsigned   pick = $urandom_range(0, 9);
    // The core is in reset during an NDM reset, and rv_dm does not answer then (see emulate_core).
    if (cfg.rv_dm_vif.ndmreset_req === 1'b1) return;
    if (pick < 3) csr_rd(.ptr(tl_mem_ral.dataaddr[$urandom_range(0, 1)]), .value(v));
    else if (pick < 6) csr_rd(.ptr(tl_mem_ral.program_buffer[$urandom_range(0, 7)]), .value(v));
    else csr_rd(.ptr(tl_mem_ral.flags[0]), .value(v));
  endtask

  protected task emulate_core();
    uvm_reg_data_t flags;
    forever begin
      cfg.clk_rst_vif.wait_clks($urandom_range(5, 60));
      // Stopped between two accesses: killing it inside one left csr_utils' count of outstanding
      // accesses up, and the end of the test waited for it until the phase timeout (2026-11-07,
      // rv_dm_cmodel_random seed 8).
      if (stop_core) begin
        core_stopped = 1'b1;
        return;
      end
      // An NDM reset resets the hart (Debug spec 3.12.2): while rv_dm requests it the core is in
      // reset, does not access the debug memory, and is not halted afterwards. rv_dm blocks the
      // debug memory during the request (its TL gate's flush), so a read then is never answered
      // (2026-11-07: rv_dm_cmodel_random seed 8 timed out on a flags read).
      if (!cfg.clk_rst_vif.rst_n || cfg.rv_dm_vif.ndmreset_req === 1'b1) begin
        core_halted = 1'b0;
        continue;
      end
      if (!core_halted) begin
        if (cfg.rv_dm_vif.debug_req[0] === 1'b1) begin
          csr_wr(.ptr(tl_mem_ral.halted), .value(0));
          core_halted = 1'b1;
        end
        continue;
      end
      csr_rd(.ptr(tl_mem_ral.flags[0]), .value(flags));
      if (flags[0]) begin
        // go: the command runs; the core may write data0/data1 as a register read would.
        csr_wr(.ptr(tl_mem_ral.going), .value(0));
        if ($urandom_range(0, 1)) csr_wr(.ptr(tl_mem_ral.dataaddr[0]), .value($urandom()));
        if ($urandom_range(0, 3) == 0) csr_wr(.ptr(tl_mem_ral.dataaddr[1]), .value($urandom()));
        cfg.clk_rst_vif.wait_clks($urandom_range(1, 40));
        if ($urandom_range(0, 9) == 0) csr_wr(.ptr(tl_mem_ral.exception), .value(0));
        csr_wr(.ptr(tl_mem_ral.halted), .value(0));
      end else if (flags[1]) begin
        csr_wr(.ptr(tl_mem_ral.resuming), .value(0));
        core_halted = 1'b0;
      end
    end
  endtask
endclass
