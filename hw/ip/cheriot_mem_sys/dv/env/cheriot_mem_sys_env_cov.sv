// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of the accesses the scoreboard sees.
class cheriot_mem_sys_env_cov extends cip_base_env_cov #(.CFG_T(cheriot_mem_sys_env_cfg));
  `uvm_component_utils(cheriot_mem_sys_env_cov)

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
    cp_tag: coverpoint it.tag;
    cp_mode: coverpoint it.ena {
      bins cheriot = {prim_mubi_pkg::MuBi4True};
      bins epmp    = {prim_mubi_pkg::MuBi4False};
      bins invalid = default;
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
    cx_op_tag_region_mode: cross cp_opcode, cp_tag, cp_region, cp_mode;
    // A capability load (hinted word) returning each tag in each tagged region.
    cx_load_rtag_region: cross cp_rtag, cp_region {
      ignore_bins untagged = binsof(cp_region) intersect {RegionUntagged, RegionError};
    }
    cx_op_err_region: cross cp_opcode, cp_err, cp_region;
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
    cx_port_op_err: cross cp_port, cp_opcode, cp_err;
  endgroup

  function new(string name, uvm_component parent);
    super.new(name, parent);
    core_access_cg   = new();
    bitmap_access_cg = new();
  endfunction

endclass
