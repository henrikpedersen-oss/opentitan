// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Random core traffic against sweeps, checked by the C model: capability stores of every kind
// (to the NVM programmed or not), capability loads, data stores and loads of every size, partial
// writes, untagged accesses, bitmap writes and reads on both bitmap ports; meanwhile sweeps of
// random ranges (some of them ignored starts), waited for by polling or by the interrupt. Most
// accesses go to five "hot" meta words, so that the core, the engine and the bitmap meet.
// +num_ops=<n> sets the number of core operations (4000).
class cheriot_mem_sys_mixed_random_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_mixed_random_vseq)
  `uvm_object_new

  protected bit [31:0] hot[5];
  protected bit        stop;

  virtual task body();
    int unsigned num_ops = 4000;
    void'($value$plusargs("num_ops=%d", num_ops));
    cheriot_on();
    foreach (hot[i]) begin
      hot[i] = (i < 4) ? MainSramBase + 256 * $urandom_range(SramCaps / 32 - 1) : NvmBase;
    end
    stop = 0;
    fork
      sweeps();
      begin
        core_ops(num_ops);
        stop = 1;
      end
    join
    wait_quiet();
    tbre_wait_idle();
    wait_quiet();
  endtask

  // Sweeps of random ranges with random gaps until `stop`.
  protected task sweeps();
    bit [31:0]   d;
    bit          accepted;
    int unsigned k;
    while (!stop) begin
      wait_cycles($urandom_range(3000, 200));
      if (stop) break;
      accepted = 1;
      case ($urandom_range(11))
        0: begin
          tbre_sweep(MainSramBase, 0, 1'b0);
          accepted = 0;
        end
        1: begin
          // k = 0 starts at MainSramTop, outside the tagged regions: ignored, no interrupt
          k = $urandom_range(8);
          tbre_sweep(MainSramTop - 8 * k, 64, 1'b0);
          accepted = (k != 0);
        end
        2, 3:    tbre_sweep(hot[4] + 8 * $urandom_range(31), 32 * (1 + $urandom_range(3)), 1'b0);
        default: tbre_sweep(hot[$urandom_range(3)], 32 * (1 + $urandom_range(3)), 1'b0);
      endcase
      // The end by polling or by the interrupt; then the epoch, and now and then start_err cleared
      if (accepted && rbit()) tbre_wait_intr();
      else                    tbre_wait_idle();
      csr_rd(.ptr(ral.tbre_epoch), .value(d));
      if ($urandom_range(3) == 0) csr_wr(.ptr(ral.tbre_status), .value(32'h1 << StatusStartErr));
    end
  endtask

  protected task core_ops(int unsigned num_ops);
    bit [31:0]  a, w0, w1, cb;
    tl_seq_item rsp;
    for (int unsigned i = 0; i < num_ops; i++) begin
      if (i % 500 == 0) randomize_timing($urandom_range(3) == 0);
      a = ($urandom_range(3) != 0) ? hot[$urandom_range(4)] + 8 * $urandom_range(31)
                                   : rand_granule(MainSramBase, MainSramTop);
      case ($urandom_range(15))
        0, 1, 2: begin
          cb = rbit() ? rand_granule(MainSramBase, MainSramTop) : NvmBase + 8 * $urandom_range(255);
          make_kind_cap(cb, rand_cap_kind(), $urandom_range(3), w0, w1);
          // To the NVM, programmed first half of the time: verified, or answered with d_error
          store_cap(a, w0, w1, $urandom_range(3) != 0, rbit());
        end
        3, 4, 5: core_issue_load_cap(a);
        6: core_issue(core_item(tlul_pkg::PutFullData, a + $urandom_range(7),
                                $urandom_range(2), $urandom));
        7: core_issue(core_item(tlul_pkg::PutPartialData, a + 4 * rbit(), 2, $urandom, 0,
                                4'($urandom_range(15, 1))));
        8: core_issue(core_item(tlul_pkg::Get, a + $urandom_range(7), $urandom_range(2)));
        9: core_issue(core_item(tlul_pkg::Get, UntaggedBase + 4 * $urandom_range(1023), 2, 0,
                                rbit()));
        10: core_issue(core_item(tlul_pkg::PutFullData, UntaggedBase + 4 * $urandom_range(1023),
                                 2, $urandom, rbit()));
        11: heap_base(rbit(), cb);  // a bitmap write
        12: bitmap_access(1, tlul_pkg::Get,
                           MetaRevbmBase + 4 * $urandom_range(RevbmBytes / 4 - 1), 2, 0, 0, rsp);
        13: bitmap_access(0, tlul_pkg::Get,
                           MetaBase + 4 * $urandom_range((MetaTop - MetaBase) / 4), 2, 0, 0, rsp);
        default: core_issue(core_item(tlul_pkg::Get, NvmBase + 8 * $urandom_range(NvmCaps - 1), 2,
                                      0, 1));
      endcase
      throttle($urandom_range(8, 1));
    end
  endtask

endclass
