// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The vseqs of the CHERIoT suite's mode-switch, core and framework tests (eg2_top_verification,
// spec "Mode switch and handover", "Framework and mode switch", "Core in CHERIoT mode", "TestRIG
// streams in the chip"; images in //sw/device/tests/eg2_cheriot). Each holds its test's knobs as
// rand fields under the spec's distributions and writes them into the image (write_params); the
// knob names and encodings are the software's (kEg2Param_<name>; the test's PARAMS list). Bench
// steps (EG2 BENCH ...) not in chip_sw_eg2_rand_vseq are served here. `model_gaps` is never
// written: the chip makes every check.

// chip_sw_cheriot_handover (REQ_SOC_07, REQ_BOOT_03). The spec's `boot_mode` (secure boot 20 %)
// has no image yet: this test runs from the test ROM.
class chip_sw_eg2_handover_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_handover_vseq)
  `uvm_object_new
  rand int unsigned switch_point, gpr_fill, icache, dummy_instr;
  constraint knobs_c {
    switch_point inside {[0:4]};  // store, WFI, taken branch, CSR write, load
    gpr_fill inside {[0:3]};      // zero, all ones, random, pointer-like
    icache inside {[0:1]};
    dummy_instr inside {[0:1]};
  }
  virtual function void write_params();
    set_param("switch_point", switch_point);
    set_param("gpr_fill", gpr_fill);
    set_param("icache", icache);
    set_param("dummy_instr", dummy_instr);
  endfunction
endclass

// chip_sw_rv_core_ibex_cheriot_switch (REQ_SOC_03): the SBA lock is bench op sba_wr32. The
// life-cycle state is the OTP image the run loads (the dvsim entry).
class chip_sw_eg2_switch_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_switch_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned lock_source, ena_writes, final_mode;
  constraint knobs_c {
    lock_source dist {1 := 70, 2 := 30};  // the core, SBA
    ena_writes inside {[0:8]};
    final_mode inside {[1:2]};            // CHERIoT, ePMP
  }
  virtual function void write_params();
    set_param("lock_source", lock_source);
    set_param("ena_writes", ena_writes);
    // ena_class 0: the software draws each write's class (50 : 25 : 25), as the spec asks per write.
    set_param("ena_class", 0);
    set_param("final_mode", final_mode);
  endfunction
endclass

// chip_sw_rv_core_ibex_cheriot_switch_err (REQ_SOC_04, REQ_CIB_01): bench op switch_fault flips
// one bit of the switch's sparse state register.
class chip_sw_eg2_switch_err_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_switch_err_vseq)
  `uvm_object_new
  rand int unsigned lock_value, ena_class, state_fault;
  constraint knobs_c {
    lock_value inside {[0:15]};
    lock_value != prim_mubi_pkg::MuBi4True;
    ena_class inside {[1:3]};             // MuBi4True, MuBi4False, another encoding
    state_fault dist {0 := 50, 1 := 50};
  }
  virtual function void write_params();
    set_param("lock_value", lock_value);
    set_param("ena_class", ena_class);
    set_param("state_fault", state_fault);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "switch_fault") begin
      string path = {"tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_core_ibex.u_cheriot_switch",
                     ".u_state_regs.u_state_flop.q_o"};
      uvm_hdl_data_t v;
      int unsigned bitpos = $urandom_range(0, 5);
      if (!uvm_hdl_read(path, v)) `uvm_fatal(`gfn, {"cannot read ", path})
      v[bitpos] = ~v[bitpos];
      // TODO(inject-registry): announce this fault (the switch's state register, now).
      `uvm_info(`gfn, $sformatf("EG2 fault: CHERIoT switch state bit %0d flipped", bitpos),
                UVM_LOW)
      if (!uvm_hdl_deposit(path, v)) `uvm_fatal(`gfn, {"cannot deposit ", path})
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
endclass

// chip_sw_cheriot_epmp_handover (REQ_SOC_06).
class chip_sw_eg2_epmp_handover_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_epmp_handover_vseq)
  `uvm_object_new
  rand int unsigned epmp_extra, mml, first_access;
  constraint knobs_c {
    epmp_extra inside {[0:8]};
    mml inside {[0:1]};
    first_access inside {[1:3]};  // fetch, load, store
  }
  virtual function void write_params();
    set_param("epmp_extra", epmp_extra);
    set_param("mml", mml);
    set_param("first_access", first_access);
  endfunction
endclass

// chip_sw_cheriot_reset_state (REQ_SOC_02): POR and NDM are bench ops (por, ndm_reset).
class chip_sw_eg2_reset_state_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_reset_state_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned reset_type, session_decade, session_len, tags_set;
  constraint knobs_c {
    reset_type inside {[1:6]};
    // log-uniform 10-10,000: a decade drawn, then uniform inside it.
    session_decade inside {[1:3]};
    solve session_decade before session_len;
    session_decade == 1 -> session_len inside {[10:99]};
    session_decade == 2 -> session_len inside {[100:999]};
    session_decade == 3 -> session_len inside {[1000:10000]};
    tags_set inside {[0:64]};
  }
  virtual function void write_params();
    set_param("reset_type", reset_type);
    set_param("session_len", session_len);
    set_param("tags_set", tags_set);
  endfunction
endclass

// chip_sw_cheriot_lock_policy (REQ_SOC_08): the SBA attempts are bench op sba_wr32.
class chip_sw_eg2_lock_policy_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_lock_policy_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned attempt, n_attempts, lock_when;
  constraint knobs_c {
    attempt inside {[1:2]};       // from the core, over SBA (the software picks the register)
    n_attempts inside {[1:32]};
    lock_when inside {[1:3]};     // right after the lock, after a sleep, after a software reset
  }
  virtual function void write_params();
    set_param("attempt", attempt);
    set_param("n_attempts", n_attempts);
    set_param("when", lock_when);
  endfunction
endclass

// chip_sw_cheriot_periph_lock (REQ_SOC_08): the same knobs; `lock_first` stays 0 (stimulus only:
// +eg2_param_lock_first=1 stands in for a compliant boot stage while rtl_tickets F29 is open).
class chip_sw_eg2_periph_lock_vseq extends chip_sw_eg2_lock_policy_vseq;
  `uvm_object_utils(chip_sw_eg2_periph_lock_vseq)
  `uvm_object_new
  virtual function void write_params();
    super.write_params();
    set_param("lock_first", 0);
  endfunction
endclass

// chip_sw_cheriot_periph_cap_random (REQ_PER_01, REQ_PER_02, REQ_PBF_13).
class chip_sw_eg2_periph_cap_random_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_periph_cap_random_vseq)
  `uvm_object_new
  localparam int NumFaults = 6;
  localparam int unsigned FaultDefault[NumFaults] = '{40, 15, 15, 15, 10, 5};
  rand int unsigned n_accesses;
  rand int unsigned w_fault[NumFaults];
  constraint knobs_c {
    n_accesses inside {[128:2048]};
    // Default weights, +-50 % per seed.
    foreach (w_fault[i]) w_fault[i] inside {[FaultDefault[i] / 2 : FaultDefault[i] * 3 / 2]};
  }
  virtual function void write_params();
    string names[NumFaults] = '{"w_fault_legal", "w_fault_above", "w_fault_below",
                                "w_fault_perm", "w_fault_misaligned", "w_fault_untagged"};
    set_param("n_accesses", n_accesses);
    foreach (names[i]) set_param(names[i], w_fault[i]);
  endfunction
endclass

// chip_sw_rv_core_ibex_cheriot_addr_translation (REQ_CIB_04).
class chip_sw_eg2_addr_translation_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_addr_translation_vseq)
  `uvm_object_new
  rand int unsigned slot, region, size, tagged_pct;
  constraint knobs_c {
    slot inside {[1:3]};     // instruction, data, both
    region inside {[1:4]};   // main SRAM, sec SRAM, RRAM, an untagged region
    size inside {[1:3]};     // smallest, middle, largest
    tagged_pct inside {[0:100]};
  }
  virtual function void write_params();
    set_param("slot", slot);
    set_param("region", region);
    set_param("size", size);
    set_param("tagged_pct", tagged_pct);
  endfunction
endclass

// chip_sw_cheriot_ottf_smoke (REQ_STE_02). The spec's `boot_mode` and `icache` have no knob in
// this OTTF image (test ROM, OTTF's own cache setting).
class chip_sw_eg2_ottf_smoke_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_ottf_smoke_vseq)
  `uvm_object_new
  rand int unsigned n_windows, window_order;
  constraint knobs_c {
    // 50 % every window (any value above the table's count), 50 % uniform.
    n_windows dist {255 :/ 50, [1:64] :/ 50};
    window_order inside {[1:3]};  // address order, reverse, random
  }
  virtual function void write_params();
    set_param("n_windows", n_windows);
    set_param("window_order", window_order);
  endfunction
endclass

// chip_sw_eg2_rand_isa[_cheriot] (REQ_CIB_06): the CHERIoT twin's knobs (the RISC-V image reads
// cap_reg_pct and w_csr_* not).
class chip_sw_eg2_rand_isa_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_rand_isa_vseq)
  `uvm_object_new
  rand int unsigned cap_reg_pct, w_csr_scr, w_csr_machine, icache, dummy_instr;
  constraint knobs_c {
    cap_reg_pct inside {[0:50]};
    w_csr_scr inside {[15:45]};       // 30, +-50 %
    w_csr_machine inside {[35:100]};  // 70, +-50 % (at most 100)
    icache inside {[0:1]};
    dummy_instr inside {[0:1]};
  }
  virtual function void write_params();
    set_param("cap_reg_pct", cap_reg_pct);
    set_param("w_csr_scr", w_csr_scr);
    set_param("w_csr_machine", w_csr_machine);
    set_param("icache", icache);
    set_param("dummy_instr", dummy_instr);
  endfunction
endclass

// chip_sw_eg2_testrig_seq (REQ_CIB_06): bench op gpio_edges drives rising edges on the GPIO pads
// every `a` CPU cycles while a stream runs.
class chip_sw_eg2_testrig_seq_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_testrig_seq_vseq)
  `uvm_object_new
  localparam int NumGens = 20;
  localparam int NumIrqs = 5;
  localparam int unsigned IrqDefault[NumIrqs] = '{40, 15, 15, 15, 15};
  rand int unsigned n_streams, code_region, icache, sweep_during, dummy_instr;
  rand int unsigned irq_decade, irq_period;
  rand bit [7:0]    w_gen[NumGens];
  rand int unsigned w_irq[NumIrqs];
  constraint knobs_c {
    n_streams inside {[16:256]};
    code_region dist {1 := 50, 2 := 25, 3 := 25};  // main SRAM, sec SRAM, RRAM
    icache inside {[0:1]};
    sweep_during dist {0 := 70, 1 := 30};
    dummy_instr inside {[0:1]};
    // Equal default weights (10), +-50 % per seed.
    foreach (w_gen[i]) w_gen[i] inside {[5:15]};
    foreach (w_irq[i]) w_irq[i] == IrqDefault[i];
    // log-uniform 50-5,000 cycles.
    irq_decade inside {[1:2]};
    solve irq_decade before irq_period;
    irq_decade == 1 -> irq_period inside {[50:499]};
    irq_decade == 2 -> irq_period inside {[500:5000]};
  }
  virtual function void write_params();
    string irq_names[NumIrqs] = '{"w_irq_none", "w_irq_rv_timer", "w_irq_plic_sw",
                                  "w_irq_aon_timer", "w_irq_gpio"};
    set_param("n_streams", n_streams);
    // Four generator weights per knob, generator g in byte g % 4 of w_gens_<g / 4>.
    for (int k = 0; k < NumGens / 4; k++) begin
      set_param($sformatf("w_gens_%0d", k),
                {w_gen[4 * k + 3], w_gen[4 * k + 2], w_gen[4 * k + 1], w_gen[4 * k]});
    end
    set_param("code_region", code_region);
    set_param("icache", icache);
    foreach (irq_names[i]) set_param(irq_names[i], w_irq[i]);
    set_param("irq_period", irq_period);
    set_param("sweep_during", sweep_during);
    set_param("dummy_instr", dummy_instr);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "gpio_edges") begin
      fork
        begin
          cfg.chip_vif.gpios_if.drive_en({NUM_GPIOS{1'b1}});
          for (int i = 0; i < 16; i++) begin
            cfg.chip_vif.cpu_clk_rst_if.wait_clks(a);
            cfg.chip_vif.gpios_if.drive({NUM_GPIOS{1'b1}});
            cfg.chip_vif.cpu_clk_rst_if.wait_clks(10);
            cfg.chip_vif.gpios_if.drive({NUM_GPIOS{1'b0}});
          end
          cfg.chip_vif.gpios_if.drive_en({NUM_GPIOS{1'b0}});
        end
      join_none
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
endclass

// chip_sw_rv_core_ibex_alerts_cheriot (REQ_CIB_02; image eg2_rf_intg, eg2_fault_escalation.c):
// bench op rf_flip flips one storage bit of a register (a = site | reg << 8 | bit << 16) `b` CPU
// cycles after the step. Storage (ibex_register_file_ff, g_cheriot_rf): in the main register file
// (ibex_top register_file_i) g_rf_data_flops[i] is x<i>'s 32-bit data word and g_rf_shared_flops[i]
// its 35-bit capability metadata (cap_t, the tag at bit 32); in the shadow register file
// (ibex_lockstep register_file_shadow_i) the same flops hold their 7-bit ECC.
class chip_sw_eg2_rf_intg_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_rf_intg_vseq)
  `uvm_object_new
  rand int unsigned fault_site, fault_reg, fault_bit, time_decade, fault_time;
  constraint knobs_c {
    fault_site inside {[1:4]};  // data, the shadow copy (ECC), metadata, the tag
    fault_reg inside {[1:15]};
    fault_bit inside {[0:63]};  // reduced to the site's width by rf_flip
    // log-uniform 100-10,000 cycles.
    time_decade inside {[1:2]};
    solve time_decade before fault_time;
    time_decade == 1 -> fault_time inside {[100:999]};
    time_decade == 2 -> fault_time inside {[1000:10000]};
  }
  virtual function void write_params();
    set_param("fault_site", fault_site);
    set_param("fault_reg", fault_reg);
    set_param("fault_bit", fault_bit);
    set_param("fault_time", fault_time);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "rf_flip") begin
      fork
        rf_flip(a[7:0], a[15:8], a[23:16], b);
      join_none
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
  protected task rf_flip(int unsigned site, int unsigned regidx, int unsigned bitsel,
                         int unsigned cycles);
    string core = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_core_ibex.u_core";
    string rf = {core, ".gen_regfile_ff.register_file_i.g_cheriot_rf"};
    string shadow = {core, ".gen_lockstep.u_ibex_lockstep.gen_shadow_regfile_ff",
                     ".register_file_shadow_i.g_cheriot_rf"};
    string path;
    int unsigned idx;
    uvm_hdl_data_t v;
    case (site)
      1: begin  // the data word
        path = $sformatf("%s.g_rf_data_flops[%0d].rf_reg_q", rf, regidx);
        idx = bitsel % 32;
      end
      2: begin  // the shadow copy: the data's or the metadata's ECC
        idx = bitsel % 14;
        path = $sformatf("%s.%s[%0d].rf_reg_q", shadow,
                         idx < 7 ? "g_rf_data_flops" : "g_rf_shared_flops", regidx);
        idx = idx % 7;
      end
      3: begin  // the metadata but the tag
        path = $sformatf("%s.g_rf_shared_flops[%0d].rf_reg_q", rf, regidx);
        idx = bitsel % 34;
        if (idx >= 32) idx++;
      end
      default: begin  // the tag
        path = $sformatf("%s.g_rf_shared_flops[%0d].rf_reg_q", rf, regidx);
        idx = 32;
      end
    endcase
    repeat (cycles) cpu_clk();
    if (!uvm_hdl_read(path, v)) `uvm_fatal(`gfn, {"cannot read ", path})
    v[idx] = ~v[idx];
    // TODO(inject-registry): announce this fault (register x<regidx>, site, bit, now, until written).
    `uvm_info(`gfn, $sformatf("EG2 fault: x%0d site %0d bit %0d flipped (%s)", regidx, site, idx,
                              path), UVM_LOW)
    if (!uvm_hdl_deposit(path, v)) `uvm_fatal(`gfn, {"cannot deposit ", path})
  endtask
endclass
