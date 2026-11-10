// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A capability store to the NVM (strict CHERIoT mode, tag sideband set) is not written but read
// and compared: it sets the tag only if the NVM already holds both words; otherwise W0 is answered
// with d_error if its word differs, W1 if either does, and the tag is left as it was; no alert
// (theory_of_operation.md, "Write-to-Read-and-Compare Filter (WTRC)" and "Error Handling";
// programmers_guide.md, "Storing Capabilities in the NVM"). A plain store to the NVM is refused
// by the NVM and clears the tag. Programming the NVM (not a core write) leaves the tag. Outside
// CHERIoT mode a tagged store is a plain one. Stores the core cannot produce raise fatal_fault.
class cheriot_mem_sys_nvm_cap_store_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_nvm_cap_store_vseq)
  `uvm_object_new

  virtual task body();
    cheriot_on();
    directed_stores();
    random_stores();
    for (int unsigned c = 0; c < 4; c++) store_core_cannot_produce(c);
  endtask

  // The answer to each word, and what each store does to the tag.
  protected task directed_stores();
    bit [31:0]                g = NvmBase + 32'h100;
    bit [31:0]                w0, w1;
    bit                       t, e0, e1;
    cheriot_mem_sys_core_item it;
    make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
    nvm_program(g, w0, w1);
    nvm_cap_store_wait(g, w0, w1, e0, e1);
    core_load_cap(g, t);
    nvm_cap_store_wait(g, w0 ^ 32'h1, w1, e0, e1);
    nvm_cap_store_wait(g, w0, w1 ^ 32'h8000_0000, e0, e1);
    core_load_cap(g, t);
    nvm_program(g, ~w0, ~w1);
    core_load_cap(g, t);
    nvm_program(g, w0, w1);
    // A plain store: refused, the tag cleared
    it = core_item(tlul_pkg::PutFullData, g + 4, 2, w1);
    core_access(it);
    core_load_cap(g, t);
    nvm_cap_store_wait(g, ~w0, w1, e0, e1);
    core_load_cap(g, t);
    // ePMP mode: a tagged store is a plain one, refused, no lookup
    set_ena(prim_mubi_pkg::MuBi4False);
    nvm_cap_store_wait(g, w0, w1, e0, e1);
    cheriot_on();
    core_load_cap(g, t);
    // The NVM read of a matching W0 fails: W0 and W1 answered with d_error, the tag left
    nvm_program(g, w0, w1);
    m_cored_h_seq.inject(InjErr, g, g + 4, InjReads);
    nvm_cap_store_wait(g, w0, w1, e0, e1);
    if (!e0 || !e1) begin
      `uvm_error(`gfn, $sformatf("W0's NVM read failed: d_error %b%b, expected 11", e0, e1))
    end
    core_load_cap(g, t);
  endtask

  // Programmed or not, matching or differing in either word, among plain stores and loads.
  protected task random_stores();
    bit [31:0] g, w0, w1, x0, x1;
    for (int unsigned rep = 0; rep < 3; rep++) begin
      randomize_timing(rep == 0);
      for (int unsigned i = 0; i < 96; i++) begin
        g = rand_granule(NvmBase, NvmTop);
        make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
        // What the NVM is programmed with
        x0 = w0;
        x1 = w1;
        case ($urandom_range(9))
          0: x0 = w0 ^ (32'h1 << $urandom_range(31));
          1: x1 = w1 ^ (32'h1 << $urandom_range(31));
          2: begin x0 = ~w0; x1 = ~w1; end
          default: ;
        endcase
        case ($urandom_range(9))
          0:    core_issue(core_item(tlul_pkg::PutFullData, g + 4 * rbit(), 2, $urandom));
          1:    core_issue(core_item(tlul_pkg::PutFullData, g + $urandom_range(7), 0, $urandom));
          2, 3: core_issue_load_cap(g);
          4:    core_store_cap(g, w0, w1, 1'b1);  // whatever the NVM holds
          default: begin
            nvm_program(g, x0, x1);
            core_store_cap(g, w0, w1, 1'b1);
          end
        endcase
        throttle(4);
      end
      wait_quiet();
      load_all_caps(NvmBase, 32);
    end
  endtask

  // A capability store the core does not produce, one per reset: d_error, the tag left as it
  // was, fatal_fault.
  //   case 0: W0 a partial write;
  //   case 1: W0 a halfword;
  //   case 2: W1 alone;
  //   case 3: a load presented while W1 is taken and not yet answered: out of the capability
  //           sequence (WTRC, CapW1: any request "as soon as it is presented"), fatal_fault.
  protected task store_core_cannot_produce(int unsigned c);
    bit [31:0]                g = NvmBase + 32'h200 + 8 * c;
    bit [31:0]                w0, w1;
    bit                       t, e0, e1;
    cheriot_mem_sys_core_item it1;
    make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
    nvm_program(g, w0, w1);
    nvm_cap_store_wait(g, w0, w1, e0, e1);
    it1 = core_item(tlul_pkg::PutFullData, g + 4, 2, w1, 1);
    case (c)
      0: begin
        core_issue(core_item(tlul_pkg::PutPartialData, g, 2, w0, 1, 4'b0111));
        core_access(it1);
      end
      1: begin
        core_issue(core_item(tlul_pkg::PutFullData, g, 1, w0, 1));
        core_access(it1);
      end
      2: core_access(it1);
      default: begin
        // A verified capability store, the meta SRAM holding W1's tag write back for 100 cycles,
        // and a load of untagged memory presented right after W1 is taken: out of sequence, so
        // fatal_fault (the reference model predicts it from when the load is presented). Every
        // answer is still the usual one: W1 sets the tag, cleared first by a plain store, and the
        // load returns its data.
        core_access(core_item(tlul_pkg::PutFullData, g + 4, 2, w1));
        wait_quiet();
        wtrc_assertions(1'b0);
        meta_stall(1'b1);
        core_issue(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
        core_issue(it1);
        core_issue(core_item(tlul_pkg::Get, UntaggedBase + 32'h40));
        wait_cycles(100);
        meta_stall(1'b0);
        wait_core_quiet();
        wtrc_assertions(1'b1);
        core_load_cap(g, t);
      end
    endcase
    wait_quiet();
    do_reset();
  endtask

endclass
