// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Constrained-random traffic for rstmgr against the reset manager reference model
// (rstmgr_cmodel_sb, +rstmgr_cmodel=1). Each round is one of:
//   register traffic: random writes of RESET_INFO (clear), ALERT_INFO_CTRL / CPU_INFO_CTRL (EN,
//     INDEX) and SW_RST_CTRL_N_<i>, reads of every register (both dumps at every INDEX), at random
//     times;
//   a hardware reset with random requests (sysrst_ctrl, aon_timer, main power glitch, escalation,
//     NDM, several);
//   a low-power entry reset;
//   a software reset (RESET_REQ true, then the reset pwrmgr makes for it);
//   new crash dumps at the inputs with capture enabled or not, then a hardware reset and reads of
//     every dump word;
//   a power-on reset.
// The REGWENs (ALERT_REGWEN, CPU_REGWEN, SW_RST_REGWEN_<i>) are cleared only in the last rounds. The
// checking is the model's (+en_scb=0 in the random tests); this sequence only checks that the reset
// tree comes back.
class rstmgr_cmodel_random_vseq extends rstmgr_base_vseq;
  `uvm_object_utils(rstmgr_cmodel_random_vseq)

  rand int unsigned num_rounds;
  constraint num_rounds_c {num_rounds inside {[20:50]};}

  typedef enum int {RndRegs, RndHw, RndLowPower, RndSw, RndDump, RndPor} round_e;

  // Always-on cycles to wait after a reset, so the model's quiet window has passed before reads.
  int unsigned settle_wait = 80;

  `uvm_object_new

  protected task settle();
    cfg.aon_clk_rst_vif.wait_clks(settle_wait);
  endtask

  protected task read_all();
    uvm_reg regs[$];
    uvm_reg_data_t v;
    ral.get_registers(regs);
    regs.shuffle();
    foreach (regs[i]) begin
      if (cfg.under_reset) return;
      if (regs[i].get_name() inside {"alert_info", "cpu_info", "alert_test"}) continue;
      csr_rd(.ptr(regs[i]), .value(v));
    end
    read_dumps();
  endtask

  protected task read_dumps();
    uvm_reg_data_t v, attr;
    csr_rd(.ptr(ral.alert_info_attr), .value(attr));
    for (int i = 0; i <= attr && i < 16; i++) begin
      if (ral.alert_regwen.get_mirrored_value() == 0) break;
      csr_wr(.ptr(ral.alert_info_ctrl.index), .value(i));
      csr_rd(.ptr(ral.alert_info), .value(v));
    end
    csr_rd(.ptr(ral.cpu_info_attr), .value(attr));
    for (int i = 0; i <= attr && i < 16; i++) begin
      if (ral.cpu_regwen.get_mirrored_value() == 0) break;
      csr_wr(.ptr(ral.cpu_info_ctrl.index), .value(i));
      csr_rd(.ptr(ral.cpu_info), .value(v));
    end
  endtask

  protected task reg_traffic(bit lock);
    int unsigned n = $urandom_range(3, 12);
    repeat (n) begin
      randcase
        2: csr_wr(.ptr(ral.reset_info), .value($urandom));
        2: csr_wr(.ptr(ral.alert_info_ctrl), .value($urandom & 'hf1));
        2: csr_wr(.ptr(ral.cpu_info_ctrl), .value($urandom & 'hf1));
        4: csr_wr(.ptr(ral.sw_rst_ctrl_n[$urandom_range(0, NumSwResets - 1)]), .value($urandom));
        1: if (lock) csr_wr(.ptr(ral.alert_regwen), .value(0));
        1: if (lock) csr_wr(.ptr(ral.cpu_regwen), .value(0));
        2: if (lock) csr_wr(.ptr(ral.sw_rst_regwen[$urandom_range(0, NumSwResets - 1)]), .value(0));
      endcase
      cfg.io_div4_clk_rst_vif.wait_clks($urandom_range(0, 20));
    end
    settle();
    read_all();
  endtask

  protected task hw_reset();
    rstreqs_t req;
    randcase
      1: req = 5'b00001;
      1: req = 5'b00010;
      1: req = 5'b00100;
      1: req = 5'b01000;
      1: req = 5'b10000;
      2: req = rstreqs_t'($urandom_range(1, 31));
    endcase
    send_hw_reset(req);
  endtask

  protected task new_dumps(bit enable);
    alert_crashdump_t a;
    cpu_crash_dump_t c;
    `DV_CHECK_STD_RANDOMIZE_FATAL(a)
    `DV_CHECK_STD_RANDOMIZE_FATAL(c)
    set_alert_info_for_capture(a, enable && ral.alert_regwen.get_mirrored_value());
    set_cpu_info_for_capture(c, enable && ral.cpu_regwen.get_mirrored_value());
  endtask

  task body();
    round_e r;
    // With OpenTitan's scoreboard off (+en_scb=0) nothing updates the RAL mirror, yet its reset
    // handler still writes the POR-domain fields (ALERT_INFO_CTRL / CPU_INFO_CTRL INDEX, and EN after
    // a low-power exit) back into rstmgr by backdoor from that mirror (rstmgr_scoreboard
    // restore_por_csr_fields). 2026-11-07: a stale mirror INDEX 0 overwrote the DUT's INDEX 9 behind
    // the reference model's back. Let every frontdoor access update the mirror.
    if (!cfg.en_scb) ral.default_map.set_auto_predict(1);
    settle();
    read_all();
    for (int unsigned i = 0; i < num_rounds; i++) begin
      bit lock = i + 3 >= num_rounds;
      `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(r, r dist {RndRegs := 4, RndHw := 3, RndLowPower := 2,
                                                     RndSw := 2, RndDump := 3, RndPor := 1};)
      `uvm_info(`gfn, $sformatf("round %0d of %0d: %0s", i + 1, num_rounds, r.name()), UVM_MEDIUM)
      case (r)
        RndRegs: reg_traffic(lock);
        RndHw: hw_reset();
        RndLowPower: send_lowpower_reset();
        RndSw: begin
          csr_wr(.ptr(ral.reset_req), .value(prim_mubi_pkg::MuBi4True));
          cfg.io_div4_clk_rst_vif.wait_clks($urandom_range(1, 20));
          send_sw_reset();
        end
        RndDump: begin
          new_dumps($urandom_range(0, 3) != 0);
          cfg.io_div4_clk_rst_vif.wait_clks($urandom_range(1, 20));
          send_hw_reset(rstreqs_t'($urandom_range(1, 31)));
        end
        RndPor: por_reset();
      endcase
      settle();
      read_all();
      `DV_CHECK_EQ(cfg.rstmgr_vif.rst_ni_inactive, 1'b1, "the main domain's reset is released")
    end
  endtask
endclass
