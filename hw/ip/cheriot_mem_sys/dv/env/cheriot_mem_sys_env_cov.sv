// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of the accesses the scoreboard sees.
class cheriot_mem_sys_env_cov extends cip_base_env_cov #(.CFG_T(cheriot_mem_sys_env_cfg));
  `uvm_component_utils(cheriot_mem_sys_env_cov)
  typedef bit [3:0] mode_bits_t;

  // Every kind of core access, in every mode, to every region, with its outcome.
  covergroup core_access_cg with function sample(cheriot_mem_sys_core_item it, region_e region);
    cp_opcode: coverpoint it.opcode {
      bins get          = {tlul_pkg::Get};
      bins put_full     = {tlul_pkg::PutFullData};
      bins put_partial  = {tlul_pkg::PutPartialData};
    }
    cp_size: coverpoint it.size {
      bins byte_ = {0};
      bins half  = {1};
      bins word  = {2};
    }
    cp_aligned: coverpoint (it.addr[2:0] == 0);
    cp_tag: coverpoint it.tag;
    // Cast away the enum type: Xcelium otherwise prunes all non-enumerator
    // values (OBINVE), even though mode_gating drives all 16 bit encodings.
    cp_mode: coverpoint mode_bits_t'(it.ena) {
      bins cheriot = {prim_mubi_pkg::MuBi4True};
      bins epmp    = {prim_mubi_pkg::MuBi4False};
      bins invalid[] = {0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15};
    }
    // Operation crosses distinguish the three documented mode classes; the
    // encoding point above separately proves every invalid encoding was used.
    cp_mode_class: coverpoint (it.ena == prim_mubi_pkg::MuBi4True ? 0 :
                              it.ena == prim_mubi_pkg::MuBi4False ? 1 : 2) {
      bins cheriot = {0};
      bins epmp = {1};
      bins invalid = {2};
    }
    cp_region: coverpoint region {
      bins sram     = {RegionSram};
      bins nvm      = {RegionNvm};
      bins untagged = {RegionUntagged};
      bins error    = {RegionError};
    }
    cp_err: coverpoint it.err;
    cp_rtag: coverpoint it.rtag iff (it.opcode == tlul_pkg::Get);

    // A store with each tag, and a load with and without the hint, everywhere, in each mode.
    cx_op_tag_region_mode: cross cp_opcode, cp_tag, cp_region, cp_mode_class {
      // The core cannot produce a tagged partial write (WTRC fault tests are separate).
      ignore_bins tagged_partial = binsof(cp_opcode.put_partial) && binsof(cp_tag) intersect {1};
    }
    x_write_size_region: cross cp_opcode, cp_size, cp_region {
      ignore_bins reads = binsof(cp_opcode.get);
    }
    // A capability load (hinted word) returning each tag in each tagged region.
    cx_load_rtag_region: cross cp_rtag, cp_region, cp_mode_class {
      // Tag-1 responses require strict CHERIoT mode.
      ignore_bins tag_off = binsof(cp_rtag) intersect {1} &&
                            (binsof(cp_mode_class.epmp) || binsof(cp_mode_class.invalid));
      ignore_bins untagged = binsof(cp_region) intersect {RegionUntagged, RegionError};
    }
    cx_op_err_region: cross cp_opcode, cp_err, cp_region {
      // RegionError is the bench memory's documented always-error region.
      ignore_bins error_region_success = binsof(cp_region.error) && binsof(cp_err) intersect {0};
      // A successful partial NVM store cannot happen: the NVM is read-only on the interconnect
      // (partial capability stores are refused by the WTRC; plain ones by the read-only NVM, the
      // bench's start_mem_side ro range).
      ignore_bins nvm_partial_success = binsof(cp_opcode.put_partial) &&
          binsof(cp_region.nvm) && binsof(cp_err) intersect {0};
    }
  endgroup

  // The bitmap ports: the software window and the core's TRVK port.
  covergroup bitmap_access_cg with function sample(bit core_port, tl_seq_item it);
    cp_port: coverpoint core_port {
      bins window    = {0};
      bins core_trvk = {1};
    }
    cp_opcode: coverpoint tlul_pkg::tl_a_op_e'(it.a_opcode) {
      bins get         = {tlul_pkg::Get};
      bins put_full    = {tlul_pkg::PutFullData};
      bins put_partial = {tlul_pkg::PutPartialData};
    }
    cp_err: coverpoint it.d_error;
    cx_port_op_err: cross cp_port, cp_opcode, cp_err {
      // Both bitmap access checkers refuse PutPartialData.
      ignore_bins partial_success = binsof(cp_opcode.put_partial) && binsof(cp_err) intersect {0};
    }
  endgroup

    // Set-valued tag/bitmap/error samples use 01=false, 10=true, 11=either.
    // One swept capability
    logic [1:0] s_tag;
    logic       s_sealing;
    logic [1:0] s_base_class;  // 0 in bitmap range, 1 below the heap, 2 above the bitmap
    logic [1:0] s_bit;
    logic [1:0] s_revoked;
    int         s_exp;
    logic       s_corr;        // a[E+8:E] < B: the base's block is below the address's

    covergroup cms_sweep_cap_cg;
      cp_tag:     coverpoint s_tag { bins clear = {1}; bins set_ = {2}; bins either_ = {3}; }
      cp_sealing: coverpoint s_sealing iff (s_tag == 2'b10);
      cp_base:    coverpoint s_base_class iff (s_tag == 2'b10) {
        bins in_range = {0}; bins below = {1}; bins above = {2};
      }
      cp_bit:     coverpoint s_bit iff (s_tag == 2'b10 && s_base_class == 0) { bins clear = {1}; bins set_ = {2}; bins either_ = {3}; }
      cp_revoked: coverpoint s_revoked { bins clear = {1}; bins set_ = {2}; bins either_ = {3}; }
      cp_exp:     coverpoint s_exp iff (s_tag == 2'b10) {
        bins e0 = {0}; bins low = {[1:4]}; bins mid = {[5:13]}; bins e14 = {14}; bins e24 = {24};
      }
      cp_corr:    coverpoint s_corr iff (s_tag == 2'b10);
      x_outcome: cross cp_sealing, cp_base, cp_bit {
        // Bitmap samples exist only for bases in the window; no cross outside it.
        ignore_bins no_bitmap = binsof(cp_base.below) || binsof(cp_base.above);
      }
    endgroup

    // TBRE_START outcome
    logic [2:0] st_outcome;  // 0 accepted, 1 num 0, 2 base below the SRAM, 3 base above it and
                             // not in the NVM, 4 not CHERIoT, 5 while busy, 6 wrote 0
    logic       st_clamped;
    logic       st_nvm;      // accepted sweep of the NVM

    covergroup cms_tbre_start_cg;
      cp_outcome: coverpoint st_outcome {
        bins accepted = {0}; bins num_zero = {1}; bins base_low = {2}; bins base_high = {3};
        bins not_cheriot = {4}; bins while_busy = {5}; bins wrote_zero = {6};
      }
      cp_clamped: coverpoint st_clamped iff (st_outcome == 0);
      cp_nvm:     coverpoint st_nvm iff (st_outcome == 0);
      x_region_clamped: cross cp_nvm, cp_clamped;
    endgroup

    // End of a sweep, as the reference model saw it
    logic [1:0] e_failed;     // a fault the test injected ended it with TBRE_STATUS.sweep_err
    logic e_denied;     // ... and that fault was only an error response to a read of the swept
                        // memory, which raises no alert
    logic e_nvm;
    logic e_intr_en;    // INTR_ENABLE.tbre_done set as it ended

    covergroup cms_tbre_end_cg;
      cp_failed:  coverpoint e_failed { bins clear = {1}; bins set_ = {2}; bins either_ = {3}; }
      cp_denied:  coverpoint e_denied iff (e_failed == 2'b10);
      cp_nvm:     coverpoint e_nvm;
      cp_intr_en: coverpoint e_intr_en;
      x_failed_intr: cross cp_failed, cp_intr_en;
    endgroup

    // A core write to a swept capability, by when it was answered relative to the engine's reads
    // of that capability (theory_of_operation.md "Revocation Engine": watched from the
    // presentation of the lower word's read)
    logic [1:0] w_window;  // 0 before the lower word's read could be presented, 1 around it
                           // (either outcome), 2 after it and before the upper word's read,
                           // 3 after the upper word's read was presented
    logic       w_upper;   // the write is to the upper word
    logic [1:0] w_rev;     // it stores a tagged capability the sweep would revoke
    logic       w_nvm;

    covergroup cms_sweep_snoop_cg;
      cp_window: coverpoint w_window {
        // External handshakes cannot distinguish before-presentation from an
        // unaccepted presented request: both are classified as around (either).
        ignore_bins early_unobservable = {0}; bins around = {1}; bins between = {2}; bins late = {3};
      }
      cp_upper:  coverpoint w_upper;
      cp_rev:    coverpoint w_rev { bins clear = {1}; bins set_ = {2}; bins either_ = {3}; }
      cp_nvm:    coverpoint w_nvm;
      x_window_word: cross cp_window, cp_upper;
      x_window_rev:  cross cp_window, cp_rev;
    endgroup

    // A capability store to the NVM (theory_of_operation.md "Write-to-Read-and-Compare Filter")
    logic       n_upper;
    logic [1:0] n_outcome;  // 0 matched, 1 data differs, 2 not a full-word PutFullData,
                            // 3 out of the W0-W1 sequence
    logic [1:0] n_old_tag;  // the tag before the store (as predicted)

    covergroup cms_nvm_cap_cg;
      cp_upper:   coverpoint n_upper;
      cp_outcome: coverpoint n_outcome {
        bins match = {0}; bins differs = {1}; bins partial = {2}; bins out_of_seq = {3};
      }
      cp_old_tag: coverpoint n_old_tag { bins clear = {1}; bins set_ = {2}; bins either_ = {3}; }
      x_word_outcome: cross cp_upper, cp_outcome;
      x_outcome_tag:  cross cp_outcome, cp_old_tag;
    endgroup

    // Meta SRAM window access (revbm / corerevbm)
    logic       a_port;      // 0 revbm, 1 corerevbm
    logic [2:0] a_outcome;   // 0 allowed, 1 not CHERIoT, 2 below, 3 tag region / above, 4 misaligned,
                             // 5 sub-word, 6 PutPartialData
    logic       a_write;

    covergroup cms_meta_access_cg;
      cp_port:    coverpoint a_port;
      cp_outcome: coverpoint a_outcome {
        bins allowed = {0}; bins not_cheriot = {1}; bins below = {2}; bins above = {3};
        bins misaligned = {4}; bins subword = {5}; bins partial = {6};
      }
      cp_write:   coverpoint a_write;
      x_all:      cross cp_port, cp_outcome, cp_write {
        ignore_bins partial_read = binsof(cp_outcome.partial) && binsof(cp_write) intersect {0};
      }
    endgroup

    // Core store during a sweep, relative to the sweep
    logic [1:0] r_kind;  // 0 outside the sweep range, 1 in range other meta word, 2 same meta word,
                         // 3 a capability the sweep revokes
    covergroup cms_sweep_race_cg;
      cp_kind: coverpoint r_kind {
        bins outside = {0}; bins in_range = {1}; bins same_word = {2}; bins revoked_cap = {3};
      }
    endgroup


  int unsigned model_samples[string];

  function void sample_model(cheriot_mem_sys_cov_item it);
    s_tag = it.s_tag;
    s_sealing = it.s_sealing;
    s_base_class = it.s_base_class;
    s_bit = it.s_bit;
    s_revoked = it.s_revoked;
    s_exp = it.s_exp;
    s_corr = it.s_corr;
    st_outcome = it.st_outcome;
    st_clamped = it.st_clamped;
    st_nvm = it.st_nvm;
    e_failed = it.e_failed;
    e_denied = it.e_denied;
    e_nvm = it.e_nvm;
    e_intr_en = it.e_intr_en;
    w_window = it.w_window;
    w_upper = it.w_upper;
    w_rev = it.w_rev;
    w_nvm = it.w_nvm;
    n_upper = it.n_upper;
    n_outcome = it.n_outcome;
    n_old_tag = it.n_old_tag;
    a_port = it.a_port;
    a_outcome = it.a_outcome;
    a_write = it.a_write;
    r_kind = it.r_kind;
    case (it.kind)
      cheriot_mem_sys_cov_item::SweepCap: begin
        cms_sweep_cap_cg.sample();
        model_samples["cms_sweep_cap_cg"]++;
      end
      cheriot_mem_sys_cov_item::TbreStart: begin
        cms_tbre_start_cg.sample();
        model_samples["cms_tbre_start_cg"]++;
      end
      cheriot_mem_sys_cov_item::TbreEnd: begin
        cms_tbre_end_cg.sample();
        model_samples["cms_tbre_end_cg"]++;
      end
      cheriot_mem_sys_cov_item::SweepSnoop: begin
        cms_sweep_snoop_cg.sample();
        model_samples["cms_sweep_snoop_cg"]++;
      end
      cheriot_mem_sys_cov_item::NvmCap: begin
        cms_nvm_cap_cg.sample();
        model_samples["cms_nvm_cap_cg"]++;
      end
      cheriot_mem_sys_cov_item::MetaAccess: begin
        cms_meta_access_cg.sample();
        model_samples["cms_meta_access_cg"]++;
      end
      cheriot_mem_sys_cov_item::SweepRace: begin
        cms_sweep_race_cg.sample();
        model_samples["cms_sweep_race_cg"]++;
      end
    endcase
  endfunction

  function void report_phase(uvm_phase phase);
    super.report_phase(phase);
    foreach (model_samples[name])
      `uvm_info("CMS_COVERAGE", $sformatf("%s samples=%0d", name, model_samples[name]), UVM_LOW)
  endfunction

  function new(string name, uvm_component parent);
    super.new(name, parent);
    core_access_cg   = new();
    bitmap_access_cg = new();
    cms_sweep_cap_cg = new();
    cms_tbre_start_cg = new();
    cms_tbre_end_cg = new();
    cms_sweep_snoop_cg = new();
    cms_nvm_cap_cg = new();
    cms_meta_access_cg = new();
    cms_sweep_race_cg = new();

  endfunction

endclass
