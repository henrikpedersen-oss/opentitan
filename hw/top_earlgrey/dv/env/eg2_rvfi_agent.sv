// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 RVFI agent: passive, a monitor only. Its monitor writes one eg2_rvfi_item per retired
// instruction (and per interrupt-only notification) to its analysis port. Modelled on Ibex's
// ibex_rvfi_monitor, with the whole RVFI (instruction, memory access, capabilities).
//
// Without the RVFI build (en_ibex_tracer) tb.sv sets no interface: the agent is then disabled and
// says so once.

class eg2_rvfi_monitor extends uvm_monitor;
  `uvm_component_utils(eg2_rvfi_monitor)

  eg2_rvfi_vif vif;
  uvm_analysis_port #(eg2_rvfi_item) analysis_port;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    analysis_port = new("analysis_port", this);
  endfunction

  // Data-side bus transactions: requested and waiting for their responses (Ibex's responses come
  // in request order), and completed since the last item. Review R2, 2026-10-25.
  protected eg2_dside_access_t dside_pending[$];
  protected eg2_dside_access_t dside_done[$];
  // Completed and given to items already, but not yet to a load or store (item.mem_acc). Review
  // R14, 2026-10-26.
  protected eg2_dside_access_t unowned[$];

  // Instructions that left the ID stage with a fetch error, by RVFI order, and the word whose fetch
  // failed: as the Ibex UVM bench ties an instruction-side error to the RVFI item that took it
  // (ibex_cosim_scoreboard.sv run_cosim_imem_errors / run_cosim_prune_imem_errors), not to the next
  // access to the word, which a discarded prefetch also makes. Review R11, 2026-10-25.
  typedef struct {
    bit [63:0] order;
    bit [31:0] addr;
  } iside_err_t;
  protected iside_err_t  iside_q[$];
  protected bit          iside_prune;
  protected bit [63:0]   last_id_order = '1;

  protected bit          in_reset = 1'b1;
  protected realtime     reset_t;
  // A data access completes before its instruction retires, by at most this many accesses (a
  // misaligned or capability access has two): more left over at the end means an access no
  // instruction accounts for. Review R9, 2026-10-25.
  localparam int unsigned MaxInFlight = 2;

  virtual task run_phase(uvm_phase phase);
    if (vif == null) return;
    forever begin
      @(vif.monitor_cb);
      if (vif.monitor_cb.rst_n !== 1'b1) begin
        if (!in_reset) reset_t = $realtime;
        in_reset = 1'b1;
        continue;
      end
      if (in_reset) begin
        // Out of reset: tell the subscribers before the first instruction.
        eg2_rvfi_item item = eg2_rvfi_item::type_id::create("item");
        item.reset       = 1'b1;
        item.boot_addr   = vif.monitor_cb.boot_addr;
        item.reset_start = reset_t;
        analysis_port.write(item);
        in_reset      = 1'b0;
        // Responses still outstanding at the reset never come.
        dside_pending.delete();
        dside_done.delete();
        unowned.delete();  // R14, 2026-10-26
        iside_q.delete();
        iside_prune   = 1'b0;
        last_id_order = '1;
      end
      sample_id();
      if (vif.monitor_cb.valid === 1'b1 || vif.monitor_cb.ext_irq_valid === 1'b1) begin
        eg2_rvfi_item item = sample();
        // The bus transactions completed in earlier cycles. Not this cycle's: RVFI shows an
        // instruction a cycle after its last response at the earliest, so a response in the cycle
        // of the item is a later instruction's, which the next item carries.
        item.dside = dside_done;
        dside_done.delete();
        // The transactions belong to the next load or store to retire (the access's instruction
        // retires after its responses; Ibex issues no data request while one is outstanding), not
        // simply to the next item: RVFI is a cycle late, so the instruction before a store can
        // retire in the cycle the store's error response arrives (chip_sw_eg2_rand_regs on nyx,
        // 2026-10-24). That load or store gets them, and mem_error from their own error bits. It
        // was the RVFI masks that picked the instruction, and with the writeback stage Ibex zeroes
        // them on a load/store error (ibex_core.sv rvfi_trap_wb): the error went to the next
        // access, stop_after_fault never fired and CHERIoT-Sail was never told of a data bus
        // error (chip_sw_eg2_connectivity_cheriot, _mem_test_cheriot, _rand_regs_cheriot on nyx,
        // 2026-10-25). Review R14, 2026-10-26.
        foreach (item.dside[i]) unowned.push_back(item.dside[i]);
        if (!item.irq_only && is_data_access(item.insn)) begin
          item.mem_acc = unowned;
          unowned.delete();
          foreach (item.mem_acc[i]) if (item.mem_acc[i].err) item.mem_error = 1'b1;
        end
        if (!item.irq_only) attach_iside_err(item);
        analysis_port.write(item);
      end
      sample_dside();
    end
  endtask

  // This cycle's ID stage: an instruction leaving it with a fetch error is remembered by its RVFI
  // order, unless a writeback exception flushes it before the next one leaves (the Ibex UVM bench's
  // pruning). Review R11, 2026-10-25.
  protected function void sample_id();
    if (iside_prune) begin
      if (vif.monitor_cb.id_done === 1'b1) begin
        iside_prune = 1'b0;
      end else if (vif.monitor_cb.wb_exception === 1'b1) begin
        void'(iside_q.pop_back());
        iside_prune = 1'b0;
      end
    end
    if (vif.monitor_cb.id_done !== 1'b1 || vif.monitor_cb.id_order == last_id_order) return;
    last_id_order = vif.monitor_cb.id_order;
    if (vif.monitor_cb.id_fetch_err === 1'b1 && vif.monitor_cb.wb_exception !== 1'b1) begin
      bit [31:0] word = {vif.monitor_cb.id_pc[31:2], 2'b00};
      iside_q.push_back('{order: vif.monitor_cb.id_order,
                          addr:  vif.monitor_cb.id_fetch_err_plus2 === 1'b1 ? word + 4 : word});
      iside_prune = 1'b1;
    end
  endfunction

  protected function void attach_iside_err(eg2_rvfi_item item);
    while (iside_q.size() > 0 && iside_q[0].order < item.order) void'(iside_q.pop_front());
    if (iside_q.size() > 0 && iside_q[0].order == item.order) begin
      item.iside_err      = 1'b1;
      item.iside_err_addr = iside_q[0].addr;
      void'(iside_q.pop_front());
    end
  endfunction

  // At the end: data-side accesses no RVFI item came after. Up to MaxInFlight belong to an
  // instruction the end of the run cut off; more are accesses no instruction accounts for (an
  // extra store the chip made). Review R9, 2026-10-25.
  function void check_phase(uvm_phase phase);
    int unsigned left = dside_done.size() + dside_pending.size();
    super.check_phase(phase);
    if (vif == null || in_reset || left == 0) return;
    if (left > MaxInFlight) begin
      `uvm_error(`gfn, $sformatf({"%0d data-side accesses of the core after its last retired ",
                                  "instruction (first: %s at %08h): no instruction accounts for them"},
                                 left, dside_done.size() > 0 && dside_done[0].store ? "store" : "access",
                                 dside_done.size() > 0 ? dside_done[0].addr : dside_pending[0].addr))
    end else begin
      `uvm_info(`gfn, $sformatf("%0d data-side accesses in flight at the end of the run", left),
                UVM_MEDIUM)
    end
  endfunction

  // This cycle's data-side bus events: a response completes the oldest request (responses come
  // in request order), then a request accepted this cycle waits for its own (a response never
  // comes in the cycle of its request).
  protected function void sample_dside();
    if (vif.monitor_cb.dside_rvalid === 1'b1) begin
      if (dside_pending.size() == 0) begin
        `uvm_error(`gfn, "data-side response without an outstanding request")
      end else begin
        eg2_dside_access_t a = dside_pending.pop_front();
        a.err    = vif.monitor_cb.dside_err;
        a.rdata  = vif.monitor_cb.dside_rdata;
        a.t_resp = $realtime;  // R6, 2026-10-25
        dside_done.push_back(a);
      end
    end
    if (vif.monitor_cb.dside_req === 1'b1) begin
      eg2_dside_access_t a;
      a.store                      = vif.monitor_cb.dside_we;
      a.addr                       = {vif.monitor_cb.dside_addr[31:2], 2'b00};
      a.be                         = vif.monitor_cb.dside_be;
      a.wdata                      = vif.monitor_cb.dside_wdata;
      a.misaligned_first           = vif.monitor_cb.dside_misaligned_first;
      a.misaligned_second          = vif.monitor_cb.dside_misaligned_second;
      a.misaligned_first_saw_error = vif.monitor_cb.dside_misaligned_first_saw_error;
      a.m_mode                     = vif.monitor_cb.dside_m_mode;
      dside_pending.push_back(a);
    end
  endfunction

  protected function eg2_rvfi_item sample();
    eg2_rvfi_item item = eg2_rvfi_item::type_id::create("item");
    item.irq_only       = vif.monitor_cb.valid !== 1'b1;
    item.order          = vif.monitor_cb.order;
    item.insn           = vif.monitor_cb.insn;
    item.trap           = vif.monitor_cb.trap;
    item.halt           = vif.monitor_cb.halt;
    item.intr           = vif.monitor_cb.intr;
    item.mode           = vif.monitor_cb.mode;
    item.pc             = vif.monitor_cb.pc_rdata;
    item.pc_next        = vif.monitor_cb.pc_wdata;
    item.rd_addr        = vif.monitor_cb.rd_addr;
    item.rd_wdata       = vif.monitor_cb.rd_wdata;
    item.rd_wcap        = vif.monitor_cb.rd_wcap;
    item.mem_addr       = vif.monitor_cb.mem_addr;
    // Ibex sets the masks for every instruction, from the LSU type and data_we_o, whether or not
    // it accessed memory (ibex_core.sv, rvfi_stage_mem_rmask): a jal reports a 4-byte load at its
    // target. They are kept only for loads and stores.
    if (is_data_access(item.insn)) begin
      item.mem_rmask    = vif.monitor_cb.mem_rmask;
      item.mem_wmask    = vif.monitor_cb.mem_wmask;
    end
    item.mem_rdata      = vif.monitor_cb.mem_rdata;
    item.mem_wdata      = vif.monitor_cb.mem_wdata;
    item.mem_is_cap     = vif.monitor_cb.mem_is_cap;
    item.mem_rcap       = vif.monitor_cb.mem_rcap;
    item.mem_wcap       = vif.monitor_cb.mem_wcap;
    item.pre_mip        = vif.monitor_cb.ext_pre_mip;
    item.post_mip       = vif.monitor_cb.ext_post_mip;
    item.nmi            = vif.monitor_cb.ext_nmi;
    item.nmi_int        = vif.monitor_cb.ext_nmi_int;
    item.debug_req      = vif.monitor_cb.ext_debug_req;
    item.debug_mode     = vif.monitor_cb.ext_debug_mode;
    item.rf_wr_suppress = vif.monitor_cb.ext_rf_wr_suppress;
    item.mcycle         = vif.monitor_cb.ext_mcycle;
    foreach (item.mhpmcounters[i]) begin
      item.mhpmcounters[i]  = vif.monitor_cb.ext_mhpmcounters[i];
      item.mhpmcountersh[i] = vif.monitor_cb.ext_mhpmcountersh[i];
    end
    item.ic_scr_key_valid = vif.monitor_cb.ext_ic_scr_key_valid;
    item.mcounteren_writable = vif.monitor_cb.ext_mcounteren_writable;
    // As the core bench: a trapping micro-operation ends the instruction even if not flagged last.
    item.more_ops         = vif.monitor_cb.ext_expanded_insn_valid &&
                            !vif.monitor_cb.ext_expanded_insn_last && !vif.monitor_cb.trap;
    item.nmi_int_mtval    = vif.monitor_cb.nmi_int_mtval;
    item.cheriot          = vif.monitor_cb.cheriot_ena;
    // R13, 2026-10-25.
    item.mcounteren       = vif.monitor_cb.csr_mcounteren;
    item.csr_mcause       = vif.monitor_cb.csr_mcause;
    item.csr_mtval        = vif.monitor_cb.csr_mtval;
    return item;
  endfunction

  // Loads and stores, by encoding: LOAD and STORE (CLC and CSC included); in quadrant 0 every
  // funct3 but c.addi4spn (c.lw, c.sw, c.clc, c.csc and Zcb's c.lbu, c.lhu, c.lh, c.sb, c.sh); in
  // quadrant 2 c.lwsp, c.swsp, c.clcsp, c.cscsp. An encoding the mode does not have traps, and a
  // trap's access is not used. A Zcmp instruction retires as its 32-bit micro-operations, one
  // RVFI item each, which the ISS's one step per item does not follow; EG2's software is built
  // without Zcmp (toolchain/BUILD -march), so none retires.
  static function bit is_data_access(bit [31:0] insn);
    bit [2:0] f3 = insn[15:13];
    case (insn[1:0])
      2'b11:   return insn[6:0] inside {7'b0000011, 7'b0100011};
      2'b00:   return f3 != 3'b000;
      2'b10:   return f3 inside {3'b010, 3'b011, 3'b110, 3'b111};
      default: return 1'b0;
    endcase
  endfunction
endclass

class eg2_rvfi_agent extends uvm_agent;
  `uvm_component_utils(eg2_rvfi_agent)

  eg2_rvfi_monitor monitor;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    monitor = eg2_rvfi_monitor::type_id::create("monitor", this);
    if (!uvm_config_db#(eg2_rvfi_vif)::get(this, "", "eg2_rvfi_vif", monitor.vif)) begin
      `uvm_info(`gfn, "No RVFI in this build (en_ibex_tracer): the RVFI agent is off", UVM_LOW)
    end
  endfunction

  function bit enabled();
    return monitor.vif != null;
  endfunction
endclass
