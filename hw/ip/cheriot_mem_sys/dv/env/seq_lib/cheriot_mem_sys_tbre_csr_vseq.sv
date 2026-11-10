// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The revocation engine's registers (registers.md; theory_of_operation.md, "Revocation Engine"):
// - TBRE_START starts a sweep only with TBRE_NUM_CAPS != 0, TBRE_BASE_ADDR in the main SRAM or the
//   NVM and strict CHERIoT mode; any other start is ignored and sets TBRE_STATUS.start_err; a
//   write of 0 and a start while the engine is active are dropped without it;
// - TBRE_STATUS.busy is high until every capability is resolved, and TBRE_REGWEN locks BASE, NUM
//   and START while the engine is active;
// - a sweep past the top of its region ends there, and TBRE_NUM_CAPS keeps the value written;
// - writes that leave out a byte holding a field, and unmapped offsets, are answered with
//   d_error; writes to the read-only registers are ignored.
class cheriot_mem_sys_tbre_csr_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_csr_vseq)
  `uvm_object_new

  localparam bit [31:0] StartErr = 32'h1 << StatusStartErr;

  virtual task body();
    bit [31:0]   d;
    int unsigned tbre0;
    tl_seq_item  rsp;
    cheriot_on();

    // Reset values
    csr_rd(.ptr(ral.tbre_regwen), .value(d));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_rd(.ptr(ral.tbre_base_addr), .value(d));
    csr_rd(.ptr(ral.tbre_num_caps), .value(d));
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_rd(.ptr(ral.alert_test), .value(d));
    csr_rd(.ptr(ral.tbre_start), .value(d));
    csr_rd(.ptr(ral.intr_test),  .value(d));

    // Field widths (the C model checks what reads back)
    csr_wr(.ptr(ral.tbre_base_addr), .value(32'hffff_ffff));
    csr_rd(.ptr(ral.tbre_base_addr), .value(d));
    csr_wr(.ptr(ral.tbre_num_caps),  .value(32'hffff_ffff));
    csr_rd(.ptr(ral.tbre_num_caps),  .value(d));
    csr_wr(.ptr(ral.intr_enable),    .value(32'hffff_ffff));
    csr_rd(.ptr(ral.intr_enable),    .value(d));
    csr_wr(.ptr(ral.intr_enable),    .value(32'h0));

    // Refused writes: a byte holding a field left out
    partial_write(ral.tbre_base_addr, 2, 32'h0,   4'b0011);
    partial_write(ral.tbre_num_caps,  1, 32'h0,   4'b0011);
    partial_write(ral.tbre_status,    2, 32'h300, 4'b0001);
    partial_write(ral.tbre_epoch,     2, 32'h0,   4'b0111);
    partial_write(ral.intr_enable,    2, 32'h1,   4'b1110);
    // ALERT_TEST without the byte of regwen (bit 31): refused, so regwen stays set
    partial_write(ral.alert_test,     2, 32'h0,   4'b0001);
    csr_rd(.ptr(ral.alert_test), .value(d));
    // Unmapped offsets
    csr_raw_access(tlul_pkg::Get, 32'h28, 2, 0, 0, 0, rsp);
    csr_raw_access(tlul_pkg::PutFullData, 32'h3c, 2, 32'h1, 0, 0, rsp);
    // Read-only registers: writes ignored
    csr_raw_access(tlul_pkg::PutFullData, ral.tbre_status.get_offset(), 2, 32'h1, 0, 0, rsp);
    csr_raw_access(tlul_pkg::PutFullData, ral.tbre_epoch.get_offset(), 2, 32'h5, 0, 0, rsp);
    csr_raw_access(tlul_pkg::PutFullData, ral.tbre_regwen.get_offset(), 2, 32'h0, 0, 0, rsp);
    csr_rd(.ptr(ral.tbre_regwen), .value(d));
    csr_rd(.ptr(ral.tbre_epoch), .value(d));

    // Ignored starts: no sweep, busy stays low, start_err set until cleared
    fill_caps(MainSramBase, SramCaps);
    tbre0 = m_tbre_h_seq.n_reads;
    tbre_sweep(MainSramBase, 0);
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_wr(.ptr(ral.tbre_status), .value(StartErr));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    tbre_sweep(MainSramBase - 8, 4);
    tbre_sweep(MainSramTop, 4);
    tbre_sweep(NvmBase - 8, 4);
    tbre_sweep(NvmTop, 4);
    tbre_sweep(32'h0, 4);
    set_ena(prim_mubi_pkg::MuBi4False);
    tbre_sweep(MainSramBase, 4);
    tbre_sweep(NvmBase, 4);
    cheriot_on();
    csr_wr(.ptr(ral.tbre_status), .value(StartErr));
    // TBRE_START written 0, and with bit 0 clear: dropped
    csr_wr(.ptr(ral.tbre_base_addr), .value(MainSramBase));
    csr_wr(.ptr(ral.tbre_num_caps), .value(4));
    csr_wr(.ptr(ral.tbre_start), .value(32'h0));
    csr_wr(.ptr(ral.tbre_start), .value(32'h2));
    wait_cycles(100);
    tbre_wait_idle();

    // A sweep past the top of its region ends there; TBRE_NUM_CAPS keeps the value written
    tbre_sweep(MainSramTop - 8 * 5, 1000);
    csr_rd(.ptr(ral.tbre_num_caps), .value(d));
    tbre_sweep(MainSramBase + 8, 32'h7fff_ffff);
    fill_caps(NvmTop - 8 * 5, 5);
    tbre_sweep(NvmTop - 8 * 5, 1000);
    tbre_sweep(NvmBase + 8, 32'h7fff_ffff);
    wait_quiet();

    // While a long sweep runs: busy high, REGWEN low, the epoch odd, the registers locked, and a
    // second start dropped
    fill_caps(MainSramBase, SramCaps);
    tbre_sweep(MainSramBase, SramCaps, 1'b0);
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_rd(.ptr(ral.tbre_regwen), .value(d));
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_wr(.ptr(ral.tbre_base_addr), .value(MainSramBase + 32'h800));
    csr_wr(.ptr(ral.tbre_num_caps), .value(3));
    csr_wr(.ptr(ral.tbre_start), .value(32'h1));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    tbre_wait_idle();
    csr_rd(.ptr(ral.tbre_base_addr), .value(d));
    csr_rd(.ptr(ral.tbre_num_caps), .value(d));
    csr_rd(.ptr(ral.tbre_regwen), .value(d));
    // The writes while locked left the RAL's mirror behind; write the values back
    csr_wr(.ptr(ral.tbre_base_addr), .value(MainSramBase));
    csr_wr(.ptr(ral.tbre_num_caps), .value(SramCaps));
    wait_cycles(200);
    wait_quiet();
  endtask

  // A write of `data` to `csr` with only the bytes in `mask`.
  protected task partial_write(uvm_reg csr, bit [1:0] size, bit [31:0] data, bit [3:0] mask);
    tl_seq_item rsp;
    csr_raw_access(tlul_pkg::PutPartialData, csr.get_offset(), size, data, mask, 0, rsp);
  endtask

endclass
