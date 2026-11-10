// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The memory behind a host port of the subsystem. tl_device_seq's update_mem() addresses mask lane i
// at a_addr + i, which is the right byte only when a_addr is word aligned; the subsystem forwards the
// core's sub-word accesses with their byte address (e.g. a halfword at ...2 in lanes 2 and 3), so the
// memory has to be addressed by byte lane from the word-aligned address, as TL-UL defines it.
//
// cip_tl_device_seq computes the response's integrity (d_user) in randomize_rsp(), before
// update_mem() puts the memory's data in, so a read would carry the integrity of the random data:
// the subsystem's WTRC compares data integrity and rightly rejects a capability store against it.
// It is computed again here, last, over the data each response carries.
class cheriot_mem_sys_mem_device_seq extends cip_tl_device_seq;
  `uvm_object_utils(cheriot_mem_sys_mem_device_seq)
  `uvm_object_new

  virtual function void update_mem(cip_tl_seq_item rsp);
    bit [31:0] word_addr = {rsp.a_addr[31:2], 2'b00};
    if (mem == null) return;
    if (rsp.a_opcode inside {tlul_pkg::PutFullData, tlul_pkg::PutPartialData}) begin
      for (int i = 0; i < 4; i++) begin
        if (rsp.a_mask[i]) mem.write_byte(word_addr + i, rsp.a_data[8*i +: 8]);
      end
      // A write is acknowledged with no data, as OpenTitan's memories answer (tlul_adapter_sram).
      rsp.d_data = '0;
    end else begin
      for (int i = 0; i < 4; i++) rsp.d_data[8*i +: 8] = mem.read_byte(word_addr + i);
    end
    // Every response, write acknowledgements included: the subsystem's RMW filter checks the data
    // integrity of each response from the meta SRAM.
    rsp.d_user = rsp.compute_d_user();
  endfunction

endclass
