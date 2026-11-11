// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// One access of the core on the data port: the request with its tag, and the response with its
// tag. The data sits in the byte lanes of its address, as on TL-UL.
class cheriot_mem_sys_core_item extends uvm_sequence_item;

  // Request
  rand tlul_pkg::tl_a_op_e opcode;
  rand bit [31:0]          addr;
  rand bit [1:0]           size;
  rand bit [3:0]           mask;
  rand bit [31:0]          data;
  // A store: the tag to write. A load: the capability-load hint.
  rand bit                 tag;

  // Response
  bit [31:0]               rdata;
  bit                      rtag;
  bit                      err;
  tlul_pkg::tl_d_op_e       d_opcode;
  bit [top_pkg::TL_SZW-1:0] d_size;
  tlul_pkg::tl_d_user_t     d_user;

  // Seen by the monitor at the request's handshake
  bit [top_pkg::TL_AIW-1:0] source;
  prim_mubi_pkg::mubi4_t    ena;

  // Set by the driver when the response is in (or a reset dropped the access: aborted).
  bit                       done;
  bit                       aborted;

  constraint opcode_c { opcode inside {tlul_pkg::Get, tlul_pkg::PutFullData,
                                       tlul_pkg::PutPartialData}; }
  constraint size_c   { size <= 2; }
  constraint align_c  { (size == 1) -> addr[0] == 1'b0;
                        (size == 2) -> addr[1:0] == 2'b00; }
  // The mask stays inside the lanes of the access; only PutPartialData may leave some out, and
  // TL-UL lets it leave gaps.
  constraint mask_c   { mask != 0;
                        (mask & ~lanes(addr, size)) == 0;
                        opcode != tlul_pkg::PutPartialData -> mask == lanes(addr, size); }

  `uvm_object_utils_begin(cheriot_mem_sys_core_item)
    `uvm_field_enum(tlul_pkg::tl_a_op_e, opcode, UVM_DEFAULT)
    `uvm_field_int (addr,    UVM_DEFAULT)
    `uvm_field_int (size,    UVM_DEFAULT)
    `uvm_field_int (mask,    UVM_DEFAULT)
    `uvm_field_int (data,    UVM_DEFAULT)
    `uvm_field_int (tag,     UVM_DEFAULT)
    `uvm_field_int (rdata,   UVM_DEFAULT)
    `uvm_field_int (rtag,    UVM_DEFAULT)
    `uvm_field_int (err,     UVM_DEFAULT)
    `uvm_field_enum(tlul_pkg::tl_d_op_e, d_opcode, UVM_DEFAULT)
    `uvm_field_int (d_size,  UVM_DEFAULT)
    `uvm_field_int (d_user,  UVM_DEFAULT)
    `uvm_field_int (source,  UVM_DEFAULT)
    `uvm_field_enum(prim_mubi_pkg::mubi4_t, ena, UVM_DEFAULT)
    `uvm_field_int (aborted, UVM_DEFAULT)
  `uvm_object_utils_end

  `uvm_object_new

  // The byte lanes an access of `size` at `addr` occupies.
  static function bit [3:0] lanes(bit [31:0] addr, bit [1:0] size);
    case (size)
      2'd0:    return 4'b0001 << addr[1:0];
      2'd1:    return 4'b0011 << {addr[1], 1'b0};
      default: return 4'b1111;
    endcase
  endfunction

  function bit is_write();
    return opcode != tlul_pkg::Get;
  endfunction

  virtual function string convert2string();
    return $sformatf("%s 0x%08x size %0d mask %h data %08x tag %0d -> rdata %08x rtag %0d err %0d",
                     opcode.name(), addr, size, mask, data, tag, rdata, rtag, err);
  endfunction

endclass
