// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Constrained-random traffic on every port, checked by the C reference model (testpoint
// cmodel_random; run with +cheriot_mem_sys_cmodel=1, the model scoreboard is the checker).
//
// Each phase runs in one mode (`cheriot_ena_i`: MuBi4True, MuBi4False, now and then another
// encoding) with two core threads and one thread on the other ports:
// - core: data stores and loads of every size and alignment to the main SRAM, the NVM and untagged
//   memory; capability stores (W0, W1 with tag 1) and loads (hinted) to the SRAM; capability stores
//   to the NVM that match its contents or not (the WTRC);
// - other: revocation bitmap writes and reads (bitmap window) and reads by the core's TRVK port;
//   sweeps (TBRE_BASE_ADDR, TBRE_NUM_CAPS, TBRE_START) polled to their end; CSR reads.
// Capabilities point into the SRAM window, so that the bitmap marks revoke some of them.
class cheriot_mem_sys_cmodel_random_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_cmodel_random_vseq)

  rand int unsigned num_phases;
  rand int unsigned ops_per_phase;
  constraint num_phases_c    { num_phases inside {[3:6]}; }
  constraint ops_per_phase_c { ops_per_phase inside {[100:300]}; }

  // Permissions in the capability's metadata word (the encoding of the unit test of the model):
  // read/write data, and a sealing capability (never revoked).
  localparam bit [31:0] PermsRw      = 32'h1f;
  localparam bit [31:0] PermsSealing = 32'h07;

  `uvm_object_new

  task body();
    for (int p = 0; p < num_phases; p++) begin
      prim_mubi_pkg::mubi4_t ena;
      randcase
        5: ena = prim_mubi_pkg::MuBi4True;
        3: ena = prim_mubi_pkg::MuBi4False;
        1: ena = prim_mubi_pkg::mubi4_t'($urandom_range(0, 15));
      endcase
      set_cheriot_ena(ena);
      `uvm_info(`gfn, $sformatf("phase %0d: cheriot_ena 0x%0h, %0d operations per thread", p, ena,
                                ops_per_phase), UVM_LOW)
      fork
        core_thread();
        core_thread();
        other_thread();
      join
      wait_sweep_end();
    end
  endtask

  // A random 8-byte-aligned address in a window.
  protected function bit [31:0] granule_in(bit [31:0] base, int unsigned win);
    return base + ($urandom_range(0, win / 8 - 1) * 8);
  endfunction

  protected function bit [31:0] any_addr();
    randcase
      6: return SramBase + $urandom_range(0, SramWin - 4);
      3: return NvmBase + $urandom_range(0, NvmWin - 4);
      1: return UntaggedBase + $urandom_range(0, UntaggedWin - 4);
    endcase
  endfunction

  // A capability's two words: the address, and its metadata with the base's low bits (exponent 0).
  protected function void make_cap(output bit [31:0] w0, output bit [31:0] w1);
    bit [31:0] base = granule_in(SramBase, SramWin);
    bit [31:0] perms = ($urandom_range(0, 4) == 0) ? PermsSealing : PermsRw;
    w0 = base;
    w1 = (perms << 25) | (base & 32'h1ff);
  endfunction

  protected task core_thread();
    for (int i = 0; i < ops_per_phase; i++) begin
      tl_seq_item rsp;
      bit [31:0]  a, w0, w1;
      randcase
        // Data load or store of any size and alignment.
        6: begin
          bit [1:0] size = $urandom_range(0, 2);
          bit [3:0] mask;
          a = any_addr() & ~32'((1 << size) - 1);
          mask = (size == 2) ? 4'hf : (size == 1) ? (4'h3 << a[1:0]) : (4'h1 << a[1:0]);
          if ($urandom_range(0, 1)) begin
            tlul_pkg::tl_a_op_e op = (size == 2 && $urandom_range(0, 3) != 0) ?
                                     tlul_pkg::PutFullData : tlul_pkg::PutPartialData;
            // A partial word write with a contiguous mask (TL-UL items require it; the core only
            // writes bytes, halfwords and words).
            if (op == tlul_pkg::PutPartialData && size == 2) begin
              bit [3:0] masks[$] = '{4'h1, 4'h2, 4'h4, 4'h8, 4'h3, 4'h6, 4'hc, 4'h7, 4'he, 4'hf};
              mask = masks[$urandom_range(0, masks.size() - 1)];
            end
            core_access(a, op, size, mask, $urandom(), 1'b0, rsp);
          end else begin
            core_access(a, tlul_pkg::Get, size, mask, 0, 1'b0, rsp);
          end
        end
        // Capability store to the SRAM window.
        3: begin
          make_cap(w0, w1);
          core_store_cap(granule_in(SramBase, SramWin), w0, w1, 1'b1);
        end
        // Capability load, hinted.
        3: core_load_cap(granule_in(SramBase, SramWin));
        // Capability store to the NVM: the same words as the NVM holds (verified, tagged), or not.
        1: begin
          a = granule_in(NvmBase, NvmWin);
          if ($urandom_range(0, 1)) begin
            w0 = cfg.data_mem.read(a);
            w1 = cfg.data_mem.read(a + 4);
          end else begin
            make_cap(w0, w1);
          end
          core_store_cap(a, w0, w1, 1'b1);
        end
        // Capability load from the NVM.
        1: core_load_cap(granule_in(NvmBase, NvmWin));
      endcase
    end
  endtask

  protected task other_thread();
    for (int i = 0; i < ops_per_phase / 4; i++) begin
      tl_seq_item    rsp;
      bit [31:0]     v;
      // Bitmap words that cover the SRAM window (one bit per 8 bytes).
      bit [31:0]     bm = MetaBase + 4 * $urandom_range(0, SramWin / 256 - 1);
      randcase
        4: tl_access(p_sequencer.revbm_sqr, bm, tlul_pkg::PutFullData, 2, 4'hf, $urandom(), rsp);
        2: tl_access(p_sequencer.revbm_sqr, bm, tlul_pkg::Get, 2, 4'hf, 0, rsp);
        1: tl_access(p_sequencer.revbm_sqr, bm, tlul_pkg::PutPartialData, 0, 4'h1, $urandom(), rsp);
        3: tl_access(p_sequencer.corerevbm_sqr, bm, tlul_pkg::Get, 2, 4'hf, 0, rsp);
        2: start_sweep();
        2: csr_rd(.ptr(ral.tbre_status), .value(v));
        1: csr_rd(.ptr(ral.tbre_epoch), .value(v));
      endcase
      cfg.clk_rst_vif.wait_clks($urandom_range(0, 20));
    end
  endtask

  // A sweep over a random run of granules in the SRAM window, started only when TBRE_STATUS reads
  // idle, as software does: the model learns that a sweep ended from that read, so a new sweep must
  // not start before it (it would run in the RTL while the model still counts the old one).
  protected task start_sweep();
    int unsigned n = $urandom_range(1, 64);
    bit [31:0]   base = granule_in(SramBase, SramWin - 8 * n);
    bit [31:0]   status;
    csr_rd(.ptr(ral.tbre_status), .value(status));
    if (status[0]) return;
    csr_wr(.ptr(ral.tbre_base_addr), .value(base));
    csr_wr(.ptr(ral.tbre_num_caps), .value(n));
    csr_wr(.ptr(ral.intr_enable), .value($urandom_range(0, 1)));
    csr_wr(.ptr(ral.tbre_start), .value(1));
  endtask

  // Poll TBRE_STATUS until the engine is idle (the model's sweep ends at that read).
  protected task wait_sweep_end();
    bit [31:0] v;
    for (int i = 0; i < 2000; i++) begin
      csr_rd(.ptr(ral.tbre_status), .value(v));
      if (!v[0]) return;
      cfg.clk_rst_vif.wait_clks(20);
    end
    `uvm_error(`gfn, "the revocation engine is still busy after 2000 polls")
  endtask

endclass
