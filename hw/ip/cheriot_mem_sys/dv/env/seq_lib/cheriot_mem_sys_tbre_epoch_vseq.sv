// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// TBRE_EPOCH bit 0 is set exactly while the engine is active, and bits 31:1 count the sweeps that
// ended without an error, advancing in the cycle bit 0 clears; a sweep with an error is not
// counted, so the epoch returns to its even value; ignored starts and starts while active leave
// it; TBRE_STATUS.sweep_err is set by an error during a sweep and, like start_err, stays set until
// written 1 (registers.md, TBRE_EPOCH and TBRE_STATUS; theory_of_operation.md, "Revocation
// Engine"; programmers_guide.md, "Tracking Completed Sweeps" and "Errors").
class cheriot_mem_sys_tbre_epoch_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_epoch_vseq)
  `uvm_object_new

  localparam bit [31:0] StartErr = 32'h1 << StatusStartErr;
  localparam bit [31:0] SweepErr = 32'h1 << StatusSweepErr;

  virtual task body();
    bit [31:0]   d, epoch, b, a, cb, w0, w1;
    int unsigned k;
    cheriot_on();
    randomize_timing(0);
    fill_caps(MainSramBase, 128);
    fill_caps(NvmBase, 64);

    // Sweeps of either region, waited for by polling busy, by the epoch, or by the interrupt
    epoch = 0;
    for (int unsigned i = 0; i < 9; i++) begin
      b = rbit() ? MainSramBase + 8 * $urandom_range(96) : NvmBase + 8 * $urandom_range(32);
      tbre_sweep(b, 1 + $urandom_range(31), 1'b0);
      csr_rd(.ptr(ral.tbre_epoch), .value(d));  // odd, or already the next even value
      case (i % 3)
        0: tbre_wait_idle();
        1: for (int unsigned p = 0; p < 100000 && d[0]; p++) csr_rd(.ptr(ral.tbre_epoch), .value(d));
        default: tbre_wait_intr();
      endcase
      epoch += 2;
      csr_rd(.ptr(ral.tbre_epoch), .value(d));
    end
    wait_quiet();

    // A start while the engine is active, and ignored starts, leave the epoch alone
    tbre_sweep(MainSramBase, SramCaps, 1'b0);
    csr_wr(.ptr(ral.tbre_start), .value(32'h1));
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    tbre_wait_idle();
    epoch += 2;
    tbre_sweep(MainSramBase, 0);
    tbre_sweep(NvmTop, 4);
    set_ena(prim_mubi_pkg::MuBi4False);
    tbre_sweep(MainSramBase, 4);
    cheriot_on();
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_wr(.ptr(ral.tbre_status), .value(StartErr));

    // A sweep with an error is not counted: the epoch goes back to its even value. The error is
    // an error response to one of the engine's reads, which fails the sweep without an alert.
    b = MainSramBase + 32'h800;
    for (int unsigned i = 0; i < 4; i++) begin
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, w0, w1);
      core_store_cap(b + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    k = 1 + $urandom_range(2);
    a = b + 8 * k + 4;
    m_tbre_h_seq.inject(InjErr, a, a + 4, InjReads);
    tbre_sweep(b, 4, 1'b0);
    tbre_wait_idle();
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    wait_cycles(200);

    // sweep_err stays set over a sweep without an error, which counts again
    tbre_sweep(MainSramBase, 16);
    epoch += 2;
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_wr(.ptr(ral.tbre_status), .value(SweepErr));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    wait_quiet();
    do_reset();
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
  endtask

endclass
