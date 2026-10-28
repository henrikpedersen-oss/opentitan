// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The vseqs of the memory-subsystem tests of the CHERIoT suite (eg2_top_verification, spec "Memory,
// tags and devices", "Revocation", "Memory subsystem in CHERIoT mode" and "in RISC-V mode"; images in
// //sw/device/tests/eg2_cheriot). Each holds its test's knobs as rand fields under the spec's
// distributions and writes them into the image (write_params); the knob names are the software's
// (kEg2Param_<name>). A test's bench steps (EG2 BENCH ...) are served by chip_sw_eg2_rand_vseq and,
// for the faults, by the classes here. `model_gaps` is never written: the chip makes every check.

// Hierarchical paths of the faults (tb/chip_hier_macros.svh's PD_MAIN_HIER).
`define EG2_VSEQ_PD_MAIN "tb.dut.top_earlgrey.earlgrey_pd_main"
`define EG2_VSEQ_CMS {`EG2_VSEQ_PD_MAIN, ".u_cheriot_mem_sys"}
`define EG2_VSEQ_CPU {`EG2_VSEQ_PD_MAIN, ".u_rv_core_ibex"}

// chip_sw_cheriot_tag_roundtrip (REQ_CMS_10).
class chip_sw_eg2_tag_roundtrip_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_tag_roundtrip_vseq)
  `uvm_object_new
  rand int unsigned n_caps, granule_pos, int_store_pct;
  constraint knobs_c {
    n_caps inside {[16:512]};
    granule_pos dist {1 := 25, 2 := 25, 3 := 50};
    int_store_pct inside {[0:50]};
  }
  virtual function void write_params();
    set_param("n_caps", n_caps);
    // Tagged 80 (main SRAM 30, sec SRAM 25, RRAM 25), untagged 20: the spec's defaults.
    set_param("w_region_main", 30);
    set_param("w_region_sec", 25);
    set_param("w_region_rram", 25);
    set_param("w_region_rom", 5);
    set_param("w_region_ret", 5);
    set_param("w_region_dbg", 5);
    set_param("w_region_dev", 5);
    set_param("granule_pos", granule_pos);
    set_param("int_store_pct", int_store_pct);
  endfunction
endclass

// chip_sw_cheriot_revocation (REQ_CMS_11).
class chip_sw_eg2_revocation_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_vseq)
  `uvm_object_new
  rand int unsigned n_marked, granule_class, outside_base, seal_pct;
  constraint knobs_c {
    n_marked inside {[1:64]};
    granule_class dist {1 := 20, 2 := 20, 3 := 20, 4 := 40};
    outside_base inside {[0:8]};
    seal_pct inside {[0:30]};
  }
  virtual function void write_params();
    set_param("n_marked", n_marked);
    set_param("granule_class", granule_class);
    set_param("outside_base", outside_base);
    set_param("seal_pct", seal_pct);
  endfunction
endclass

// chip_sw_cheriot_tbre_sweep_irq (REQ_CMS_12).
class chip_sw_eg2_tbre_sweep_irq_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_tbre_sweep_irq_vseq)
  `uvm_object_new
  rand int unsigned sweep_region, sweep_size, n_marked, wait_kind;
  constraint knobs_c {
    sweep_region inside {[1:2]};
    sweep_size dist {1 := 20, 2 := 40, 3 := 40};
    n_marked dist {0 := 10, [1:64] :/ 90};
    wait_kind inside {[0:1]};
  }
  virtual function void write_params();
    set_param("sweep_region", sweep_region);
    set_param("sweep_size", sweep_size);
    set_param("n_marked", n_marked);
    set_param("wait", wait_kind);
  endfunction
endclass

// chip_sw_cheriot_nvm_cap_validate (REQ_CMS_17).
class chip_sw_eg2_nvm_cap_validate_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_nvm_cap_validate_vseq)
  `uvm_object_new
  rand int unsigned n_stores, match_pct;
  constraint knobs_c {
    n_stores inside {[8:256]};
    match_pct dist {0 := 10, 100 := 10, [1:99] :/ 80};
  }
  virtual function void write_params();
    set_param("n_stores", n_stores);
    set_param("match_pct", match_pct);
  endfunction
endclass

// The REQ_CMS_16 writers: one granule 30, a partial granule 20, many granules 30, the whole
// region 20.
class chip_sw_eg2_cms16_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_cms16_vseq)
  `uvm_object_new
  rand int unsigned n_caps, range_class;
  constraint knobs_c {
    n_caps inside {[1:64]};
    range_class dist {1 := 30, 2 := 20, 3 := 30, 4 := 20};
  }
  virtual function void write_params();
    set_param("n_caps", n_caps);
    set_param("range_class", range_class);
  endfunction
endclass

// chip_sw_cheriot_sram_rekey_tag (REQ_CMS_16): INIT or key renewal.
class chip_sw_eg2_sram_rekey_tag_vseq extends chip_sw_eg2_cms16_vseq;
  `uvm_object_utils(chip_sw_eg2_sram_rekey_tag_vseq)
  `uvm_object_new
  rand int unsigned writer;
  constraint writer_c {writer inside {[1:2]};}
  virtual function void write_params();
    super.write_params();
    set_param("writer", writer);
  endfunction
endclass

// chip_sw_cheriot_rram_program_tag (REQ_CMS_16).
class chip_sw_eg2_rram_program_tag_vseq extends chip_sw_eg2_cms16_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_program_tag_vseq)
  `uvm_object_new
endclass

// chip_cheriot_sba_tag_clear (REQ_CMS_16; expected to fail on this RTL, rtl_tickets F23): the
// bench's SBA writes (bench op sba_fill, width drawn there).
class chip_sw_eg2_sba_tag_clear_vseq extends chip_sw_eg2_cms16_vseq;
  `uvm_object_utils(chip_sw_eg2_sba_tag_clear_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
endclass

// chip_sw_cheriot_meta_unmapped[_cheriot] (REQ_CMS_09): the SBA reads and writes are bench ops
// sba_read and sba_rmw. Mode-independent.
class chip_sw_eg2_meta_unmapped_vseq extends chip_sw_eg2_rand_vseq;
  `uvm_object_utils(chip_sw_eg2_meta_unmapped_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned n_accesses;
  constraint knobs_c {n_accesses inside {[32:256]};}
  virtual function void write_params();
    set_param("n_accesses", n_accesses);
    // target: first word 15, last word 15, the tag region 25, the bitmap region 25, anywhere 20.
    set_param("w_target_first", 15);
    set_param("w_target_last", 15);
    set_param("w_target_tags", 25);
    set_param("w_target_bitmap", 25);
    set_param("w_target_any", 20);
  endfunction
endclass

// chip_sw_cheriot_mem_sys_e2e[_cheriot] (REQ_CMS_19). Mode-independent.
class chip_sw_eg2_mem_sys_e2e_vseq extends chip_sw_eg2_rand_vseq;
  `uvm_object_utils(chip_sw_eg2_mem_sys_e2e_vseq)
  `uvm_object_new
  rand int unsigned n_scenarios, n_rekey_caps;
  constraint knobs_c {
    n_scenarios inside {[4:32]};
    n_rekey_caps inside {[0:64]};
  }
  virtual function void write_params();
    set_param("n_scenarios", n_scenarios);
    // Each block-bench scenario alike (uniform).
    foreach (scenario_names[i]) set_param(scenario_names[i], 1);
    set_param("n_rekey_caps", n_rekey_caps);
  endfunction
  static string scenario_names[] = '{"w_scenario_store_load", "w_scenario_subword_clear",
                                     "w_scenario_sweep", "w_scenario_access_check",
                                     "w_scenario_rekey"};
endclass

// chip_sw_eg2_rand_revocation (REQ_CMS_11, REQ_CMS_12, REQ_CMS_15, REQ_PBF_22).
class chip_sw_eg2_rand_revocation_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_rand_revocation_vseq)
  `uvm_object_new
  rand int unsigned n_objects, free_pct, sweep_mode, loads_per_obj;
  constraint knobs_c {
    n_objects inside {[8:256]};
    free_pct inside {[10:90]};
    sweep_mode dist {0 := 10, 1 := 30, 2 := 40, 3 := 20};
    loads_per_obj inside {[1:8]};
  }
  virtual function void write_params();
    set_param("n_objects", n_objects);
    // Sizes: one granule 20, small 40, large 30, a whole region 10; heaps: main 70, sec 30.
    set_param("w_size_granule", 20);
    set_param("w_size_small", 40);
    set_param("w_size_large", 30);
    set_param("w_size_region", 10);
    set_param("w_heap_main", 70);
    set_param("w_heap_sec", 30);
    set_param("free_pct", free_pct);
    set_param("sweep_mode", sweep_mode);
    set_param("loads_per_obj", loads_per_obj);
  endfunction
endclass

// The faults of REQ_CMS_13 and REQ_CMS_18 (eg2_fault_escalation.c), and the campaign.
class chip_sw_eg2_fault_base_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_fault_base_vseq)
  `uvm_object_new

  // sec SRAM's arena B, where the faulted capability is (eg2_cheriot_rand.h kEg2ArenaB).
  localparam bit [31:0] SlotLo = 32'h1002_e000;
  localparam bit [31:0] SlotHi = 32'h1003_0000;

  // A meta SRAM word: bits [31:0] data, [38:32] integrity.
  protected task meta_flip(int unsigned word, int unsigned site, int unsigned nbits);
    string path = $sformatf("%s.u_sram_ctrl_meta.u_prim_ram_1p_scr.u_prim_ram_1p_adv.gen_ram_inst[0].u_mem.mem[%0d]",
                            `EG2_VSEQ_PD_MAIN, word);
    uvm_hdl_data_t v;
    int unsigned lo = site == 3 ? 32 : 0, span = site == 3 ? 7 : 32;
    int unsigned b0 = lo + $urandom_range(0, span - 1), b1;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    v[b0] ^= 1'b1;
    if (nbits == 2) begin
      do b1 = lo + $urandom_range(0, span - 1); while (b1 == b0);
      v[b1] ^= 1'b1;
    end
    announce_fault(path, $sformatf("%0d bit(s) flipped", nbits));
    `uvm_info(`gfn, $sformatf("EG2 fault: meta SRAM word %0d, %0d bit(s) flipped", word, nbits),
              UVM_LOW)
    `DV_CHECK_FATAL(uvm_hdl_deposit(path, v), path)
  endtask

  // The fault sites of the campaign and the directed tests, armed in the background (they hit the
  // next matching transfer, which the software makes after the step's answer).
  protected function void arm(int unsigned site, int unsigned dir, int unsigned bitpos);
    string cored_a = {`EG2_VSEQ_CMS, ".cored_tl_d_i"};
    string cored_d = {`EG2_VSEQ_CMS, ".cored_tl_d_o"};
    string revbm_d = {`EG2_VSEQ_CPU, ".corerevbm_tl_i"};
    string meta_d = {`EG2_VSEQ_CMS, ".meta_sram_tl_i"};
    fork
      case (site)
        // command or data integrity, request (dir 1) or response (dir 2)
        0, 1: begin
          if (dir == 1) corrupt_next_a(cored_a, SlotLo, SlotHi, site, bitpos);
          else corrupt_d_after_a(cored_a, cored_d, SlotLo, SlotHi, site, bitpos);
        end
        // the tag sideband
        2: begin
          if (dir == 1) begin
            corrupt_mubi({`EG2_VSEQ_CMS, ".cored_tag_h2d_i"}, cored_a,
                         $bits(tlul_pkg::tl_h2d_t) - 1, SlotLo, SlotHi);
          end else begin
            corrupt_mubi({`EG2_VSEQ_CMS, ".cored_tag_d2h_o"}, cored_d,
                         $bits(tlul_pkg::tl_d2h_t) - 1);
          end
        end
        // a bitmap response
        3: corrupt_next_d(revbm_d, bitpos % 3, bitpos);
        // the meta SRAM port's response
        default: corrupt_next_d(meta_d, bitpos % 3, bitpos);
      endcase
    join_none
  endfunction
endclass

// chip_sw_cheriot_meta_intg_escalation (REQ_CMS_13): bench op meta_flip.
class chip_sw_eg2_meta_intg_escalation_vseq extends chip_sw_eg2_fault_base_vseq;
  `uvm_object_utils(chip_sw_eg2_meta_intg_escalation_vseq)
  `uvm_object_new
  rand int unsigned meta_site, flip_bits, access;
  constraint knobs_c {
    meta_site inside {[1:3]};
    flip_bits dist {1 := 50, 2 := 50};
    access inside {[1:3]};
  }
  virtual function void write_params();
    set_param("meta_site", meta_site);
    set_param("flip_bits", flip_bits);
    set_param("access", access);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "meta_flip") begin
      meta_flip(a, b >> 8, b & 8'hff);
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
endclass

// chip_sw_rv_core_ibex_cheriot_revbm_intg (REQ_CMS_18): bench op revbm_corrupt.
class chip_sw_eg2_revbm_intg_vseq extends chip_sw_eg2_fault_base_vseq;
  `uvm_object_utils(chip_sw_eg2_revbm_intg_vseq)
  `uvm_object_new
  rand int unsigned field;
  constraint knobs_c {field inside {[1:3]};}
  virtual function void write_params();
    set_param("field", field);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "revbm_corrupt") begin
      fork corrupt_next_d({`EG2_VSEQ_CPU, ".corerevbm_tl_i"}, b - 1, $urandom()); join_none
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
endclass

// chip_sw_cheriot_bus_intg (REQ_CMS_18, REQ_TL_04): bench op bus_corrupt.
class chip_sw_eg2_bus_intg_vseq extends chip_sw_eg2_fault_base_vseq;
  `uvm_object_utils(chip_sw_eg2_bus_intg_vseq)
  `uvm_object_new
  rand int unsigned fault_site, direction;
  constraint knobs_c {
    fault_site inside {[1:3]};
    direction inside {[1:2]};
  }
  virtual function void write_params();
    set_param("fault_site", fault_site);
    set_param("direction", direction);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "bus_corrupt") begin
      arm(a - 1, b, $urandom());
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
endclass

// chip_cheriot_fi_meta (REQ_CMS_18): one fault per round (bench op fi), the site, direction and
// bit drawn here.
class chip_sw_eg2_fi_meta_vseq extends chip_sw_eg2_fault_base_vseq;
  `uvm_object_utils(chip_sw_eg2_fi_meta_vseq)
  `uvm_object_new
  rand int unsigned campaign_size;
  constraint knobs_c {
    campaign_size inside {[100:1000]};
    soft campaign_size dist {[100:200] :/ 80, [201:1000] :/ 20};
  }
  virtual function void write_params();
    set_param("campaign_size", campaign_size);
  endfunction
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    if (op == "fi") begin
      // The round before ended without a reset: its fault's window ends here, so it covers no
      // mismatch of a later round.
      if (last_fault >= 0) eg2_inject_registry::close(last_fault);
      last_fault = -1;
      arm($urandom_range(0, 4), $urandom_range(1, 2), $urandom());
      result = 0;
    end else begin
      super.bench_op(op, a, b, result);
    end
  endtask
endclass

// chip_sw_cheriot_meta_init_after_reset (REQ_CMS_15): POR and NDM are bench ops (por,
// ndm_reset).
class chip_sw_eg2_meta_init_after_reset_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_meta_init_after_reset_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned reset_type, n_tags, sweep_running;
  constraint knobs_c {
    reset_type inside {[1:6]};
    n_tags inside {[0:256]};
    sweep_running inside {[0:1]};
  }
  virtual function void write_params();
    set_param("reset_type", reset_type);
    set_param("n_tags", n_tags);
    set_param("sweep_running", sweep_running);
  endfunction
endclass

`undef EG2_VSEQ_CPU
`undef EG2_VSEQ_CMS
`undef EG2_VSEQ_PD_MAIN
