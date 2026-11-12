// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The backdoor into the memory behind the crossbar: the sequences preload it, and program the
// NVM as its controller would. Every write is published on analysis_port, as a monitor would
// publish a bus write, so that the reference model sees the same memory as the DUT.
class cheriot_mem_sys_backdoor_item extends uvm_sequence_item;
  bit [31:0] addr;
  bit [31:0] data;

  `uvm_object_utils_begin(cheriot_mem_sys_backdoor_item)
    `uvm_field_int(addr, UVM_DEFAULT)
    `uvm_field_int(data, UVM_DEFAULT)
  `uvm_object_utils_end

  `uvm_object_new
endclass

class cheriot_mem_sys_mem_backdoor extends uvm_component;
  `uvm_component_utils(cheriot_mem_sys_mem_backdoor)

  mem_model#(.AddrWidth(32))                     mem;
  uvm_analysis_port #(cheriot_mem_sys_backdoor_item) analysis_port;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    analysis_port = new("analysis_port", this);
  endfunction

  function void write_word(bit [31:0] addr, bit [31:0] data);
    cheriot_mem_sys_backdoor_item it = cheriot_mem_sys_backdoor_item::type_id::create("it");
    mem.write(addr, data);
    it.addr = addr;
    it.data = data;
    analysis_port.write(it);
  endfunction

endclass
