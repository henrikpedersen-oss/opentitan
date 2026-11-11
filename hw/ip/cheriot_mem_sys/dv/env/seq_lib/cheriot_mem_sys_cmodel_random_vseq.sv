// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Constrained-random traffic on every port, on Earl Grey 2's address map, checked by the C model
// (testpoint cmodel_random). Each phase runs in one mode (cheriot_ena_i: MuBi4True, MuBi4False,
// now and then another encoding) with two core threads and one thread on the other ports:
// - core: data stores and loads of every size and alignment to the main SRAM, the NVM and
//   untagged memory; capability stores and hinted loads in the SRAM; capability stores to the NVM
//   that match its contents or not;
// - other: bitmap writes and reads on the software window, reads on the core's TRVK port, sweeps
//   (started only when the engine is idle), register reads.
// Capabilities point into the SRAM window, so that the bitmap revokes some of them.
// Requirements: REQ_TAG_01–06, REQ_CMS_04, REQ_CMS_06; REQ_WTRC_01–09; REQ_TBRE_01–21.
class cheriot_mem_sys_cmodel_random_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_cmodel_random_vseq)

  rand int unsigned num_phases;
  rand int unsigned ops_per_phase;
  constraint num_phases_c    { num_phases inside {[3:6]}; }
  constraint ops_per_phase_c { ops_per_phase inside {[100:300]}; }

  `uvm_object_new

  virtual task body();
    prim_mubi_pkg::mubi4_t ena;
    for (int unsigned p = 0; p < num_phases; p++) begin
      randcase
        5: ena = prim_mubi_pkg::MuBi4True;
        3: ena = prim_mubi_pkg::MuBi4False;
        1: ena = prim_mubi_pkg::mubi4_t'($urandom_range(15));
      endcase
      set_ena(ena);
      `uvm_info(`gfn, $sformatf("phase %0d: cheriot_ena_i 0x%0h, %0d operations per thread", p,
                                ena, ops_per_phase), UVM_LOW)
      fork
        core_thread();
        core_thread();
        other_thread();
      join
      wait_quiet();
      tbre_wait_idle();
    end
  endtask

  // A random 8-byte granule in the preloaded part of a region.
  protected function bit [31:0] granule_in(bit [31:0] base, int unsigned win);
    return base + 8 * $urandom_range(win / 8 - 1);
  endfunction

  protected function bit [31:0] any_addr();
    randcase
      6: return MainSramBase + $urandom_range(SramWin - 4);
      3: return NvmBase + $urandom_range(NvmWin - 4);
      1: return UntaggedBase + $urandom_range(UntaggedTop - UntaggedBase - 4);
    endcase
  endfunction

  // A capability into the SRAM window; one in five a sealing capability (never revoked).
  protected function void sram_cap(output bit [31:0] w0, output bit [31:0] w1);
    make_cap(granule_in(MainSramBase, SramWin), 0,
             ($urandom_range(4) == 0) ? PermsSealing : PermsMemRw, w0, w1);
  endfunction

  protected task core_thread();
    bit [31:0] a, w0, w1;
    bit        t;
    for (int unsigned i = 0; i < ops_per_phase; i++) begin
      randcase
        // A data load or store of any size and alignment
        6: begin
          bit [1:0] size = $urandom_range(2);
          a = any_addr();
          if (rbit()) begin
            // As the core writes: a byte or a halfword is a PutPartialData; a word mostly a
            // PutFullData, else a PutPartialData with a contiguous mask
            if (size == 2 && $urandom_range(3) != 0) begin
              core_access(core_item(tlul_pkg::PutFullData, a, 2, $urandom));
            end else if (size == 2) begin
              bit [3:0] masks[10] = '{4'h1, 4'h2, 4'h4, 4'h8, 4'h3, 4'h6, 4'hc, 4'h7, 4'he, 4'hf};
              core_access(core_item(tlul_pkg::PutPartialData, a, 2, $urandom, 0,
                                    masks[$urandom_range(9)]));
            end else begin
              core_access(core_item(tlul_pkg::PutPartialData, a, size, $urandom));
            end
          end else begin
            core_access(core_item(tlul_pkg::Get, a, size));
          end
        end
        // A capability store to the SRAM window
        3: begin
          sram_cap(w0, w1);
          core_store_cap(granule_in(MainSramBase, SramWin), w0, w1, 1'b1);
        end
        // A capability load
        3: core_load_cap(granule_in(MainSramBase, SramWin), t);
        // A capability store to the NVM: the words the NVM holds (verified, tagged), or others
        1: begin
          a = granule_in(NvmBase, NvmWin);
          if (rbit()) begin
            w0 = cfg.data_mem.read(a);
            w1 = cfg.data_mem.read(a + 4);
          end else begin
            sram_cap(w0, w1);
          end
          core_store_cap(a, w0, w1, 1'b1);
        end
        // A capability load from the NVM
        1: core_load_cap(granule_in(NvmBase, NvmWin), t);
      endcase
    end
  endtask

  protected task other_thread();
    bit [31:0]  bm, v;
    tl_seq_item rsp;
    for (int unsigned i = 0; i < ops_per_phase / 4; i++) begin
      // A bitmap word that covers the SRAM window (one bit per 8 bytes)
      bm = MetaRevbmBase + 4 * $urandom_range(SramWin / 256 - 1);
      randcase
        4: bitmap_access(0, tlul_pkg::PutFullData, bm, 2, $urandom, 0, rsp);
        2: bitmap_access(0, tlul_pkg::Get, bm, 2, 0, 0, rsp);
        1: bitmap_access(0, tlul_pkg::PutPartialData, bm, 0, $urandom, 4'h1, rsp);
        3: bitmap_access(1, tlul_pkg::Get, bm, 2, 0, 0, rsp);
        2: start_sweep();
        2: csr_rd(.ptr(ral.tbre_status), .value(v));
        1: csr_rd(.ptr(ral.tbre_epoch), .value(v));
      endcase
      wait_cycles($urandom_range(20));
    end
  endtask

  // A sweep over a random run of granules in the SRAM window, started only when TBRE_STATUS
  // reads idle, as software does: the model learns that a sweep ended from that read, so a new
  // sweep must not start before it.
  protected task start_sweep();
    int unsigned n = $urandom_range(1, 64);
    bit [31:0]   status;
    csr_rd(.ptr(ral.tbre_status), .value(status));
    if (status[StatusBusy]) return;
    csr_wr(.ptr(ral.tbre_base_addr), .value(granule_in(MainSramBase, SramWin - 8 * n)));
    csr_wr(.ptr(ral.tbre_num_caps), .value(n));
    csr_wr(.ptr(ral.intr_enable), .value($urandom_range(1)));
    csr_wr(.ptr(ral.tbre_start), .value(1));
  endtask

endclass
