// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// One retired instruction (or an interrupt-only notification) of rv_core_ibex's main core, as the
// EG2 RVFI agent's monitor sees it. The fields follow RVFI; the capabilities are the raw 35-bit
// register-format capabilities (ibex_cheriot_pkg::cap_t), bit 32 being the tag.

// One data-side bus transaction of the core (eg2_rvfi_if dside_*), with its response: what Ibex's
// co-simulation takes as a DUT access (riscv_cosim_notify_dside_access). Review R2, 2026-10-25.
typedef struct {
  bit        store;
  bit [31:0] addr;    // word-aligned
  bit [3:0]  be;
  bit [31:0] wdata;
  bit [31:0] rdata;
  bit        err;
  bit        misaligned_first;
  bit        misaligned_second;
  bit        misaligned_first_saw_error;
  bit        m_mode;
  // When the response came: a backdoor write after it is not what this load read (review R6,
  // 2026-10-25).
  realtime   t_resp;
} eg2_dside_access_t;

class eg2_rvfi_item extends uvm_sequence_item;
  // Reset notification: the core came out of reset (no instruction). A reference model restarts.
  bit                  reset;
  // Interrupt-only notification: no instruction retired, an interrupt was taken.
  bit                  irq_only;
  bit [63:0]           order;
  bit [31:0]           insn;
  bit                  trap;
  bit                  halt;
  bit                  intr;
  bit [1:0]            mode;
  bit [31:0]           pc;
  bit [31:0]           pc_next;
  bit [4:0]            rd_addr;
  bit [31:0]           rd_wdata;
  bit [EG2_CAP_W-1:0]  rd_wcap;
  bit [31:0]           mem_addr;
  bit [3:0]            mem_rmask;
  bit [3:0]            mem_wmask;
  bit [31:0]           mem_rdata;
  bit [31:0]           mem_wdata;
  bit                  mem_is_cap;
  bit [EG2_CAP_W-1:0]  mem_rcap;
  bit [EG2_CAP_W-1:0]  mem_wcap;
  // Events for a reference model.
  bit [31:0]           pre_mip;
  bit [31:0]           post_mip;
  bit                  nmi;
  bit                  nmi_int;
  bit                  debug_req;
  bit                  debug_mode;
  bit                  rf_wr_suppress;
  bit [63:0]           mcycle;
  bit [31:0]           mhpmcounters [10];
  bit [31:0]           mhpmcountersh [10];
  bit                  ic_scr_key_valid;
  // A Zcmp micro-operation that is not the instruction's last (the ISS steps on the last).
  bit                  more_ops;
  // The mtval of an internal NMI (eg2_rvfi_if.nmi_int_mtval).
  bit [31:0]           nmi_int_mtval;
  // A data response of this instruction came back with an error.
  bit                  mem_error;
  // A load or store (by encoding): the data-side bus transactions it made, each with its own
  // response, in bus order: every transaction that completed since the previous load or store
  // retired (Ibex has one load or store on the bus at a time and it completes before it retires).
  // A half PMP stopped is absent. mem_error is set from these, not from the RVFI masks: with a
  // writeback stage Ibex zeroes mem_rmask/mem_wmask of an instruction that took a load or store
  // error (ibex_core.sv rvfi_trap_wb) and keeps mem_addr, so a faulting access shows only here.
  // Review R14, 2026-10-26.
  eg2_dside_access_t   mem_acc[$];
  // The data-side bus transactions whose responses came in before this item, in bus order (not
  // only this instruction's: an earlier item that is not a data access may leave them to it). The
  // ISS is told of them before it steps this item, as the Ibex UVM bench tells it of each access as
  // it happens; Spike matches them in order with its own accesses. Review R2, 2026-10-25.
  eg2_dside_access_t   dside[$];
  // The core runs in CHERIoT mode (cheriot_ena at the core).
  bit                  cheriot;
  // The core's boot address (reset items).
  bit [31:0]           boot_addr;
  // When the reset began (reset items): announcements made before it end at this reset (review
  // R12, 2026-10-25).
  realtime             reset_start;
  // The instruction's fetch got an error response (the core's ID stage took it with a fetch error,
  // tied to this RVFI order as the Ibex UVM bench ties it), at word address iside_err_addr. Review
  // R11, 2026-10-25.
  bit                  iside_err;
  bit [31:0]           iside_err_addr;
  // mcounteren as the instruction sees it (a WARL input pushed into Spike before the step, as the
  // Ibex UVM bench pushes it); mcause and mtval when the item retired, for the trap-cause check in
  // CHERIoT mode (review R13, 2026-10-25).
  bit [31:0]           mcounteren;
  bit [31:0]           csr_mcause;
  bit [31:0]           csr_mtval;

  `uvm_object_utils_begin(eg2_rvfi_item)
    `uvm_field_int(irq_only,   UVM_DEFAULT)
    `uvm_field_int(order,      UVM_DEFAULT)
    `uvm_field_int(insn,       UVM_DEFAULT)
    `uvm_field_int(trap,       UVM_DEFAULT)
    `uvm_field_int(intr,       UVM_DEFAULT)
    `uvm_field_int(mode,       UVM_DEFAULT)
    `uvm_field_int(pc,         UVM_DEFAULT)
    `uvm_field_int(pc_next,    UVM_DEFAULT)
    `uvm_field_int(rd_addr,    UVM_DEFAULT)
    `uvm_field_int(rd_wdata,   UVM_DEFAULT)
    `uvm_field_int(rd_wcap,    UVM_DEFAULT)
    `uvm_field_int(mem_addr,   UVM_DEFAULT)
    `uvm_field_int(mem_rmask,  UVM_DEFAULT)
    `uvm_field_int(mem_wmask,  UVM_DEFAULT)
    `uvm_field_int(mem_rdata,  UVM_DEFAULT)
    `uvm_field_int(mem_wdata,  UVM_DEFAULT)
    `uvm_field_int(mem_is_cap, UVM_DEFAULT)
    `uvm_field_int(mem_rcap,   UVM_DEFAULT)
    `uvm_field_int(mem_wcap,   UVM_DEFAULT)
  `uvm_object_utils_end

  `uvm_object_new

  // One line of rvfi.chip.log. Only what the instruction did is printed, so two logs of the same
  // run (chip and model) can be compared with diff.
  virtual function string log_line();
    string s;
    if (reset) return "reset";
    if (irq_only) return $sformatf("%0d irq mip=%08h", order, post_mip);
    s = $sformatf("%0d pc=%08h insn=%08h", order, pc, insn);
    if (trap) s = {s, " trap"};
    if (intr) s = {s, " intr"};
    if (rd_addr != 0) s = {s, $sformatf(" x%0d=%08h", rd_addr, rd_wdata)};
    if (rd_addr != 0 && rd_wcap[32]) s = {s, $sformatf(" cap=%09h", rd_wcap)};
    if (mem_rmask != 0) s = {s, $sformatf(" ld[%08h/%h]=%08h", mem_addr, mem_rmask, mem_rdata)};
    if (mem_wmask != 0) s = {s, $sformatf(" st[%08h/%h]=%08h", mem_addr, mem_wmask, mem_wdata)};
    if (mem_is_cap) s = {s, $sformatf(" mcap r=%09h w=%09h", mem_rcap, mem_wcap)};
    return s;
  endfunction

  // The access this load or store makes, from its encoding: whether it stores, and the byte mask
  // RVFI would show (from mem_addr, unshifted, as ibex_core.sv rvfi_mem_mask_int; a capability
  // access 4'b1111 with mem_is_cap). For a trapping access whose RVFI masks Ibex zeroed (R14):
  // only mem_addr is left. 0 mask: not a load or store. Review R14, 2026-10-26.
  //   LOAD/STORE: funct3[1:0] the size (CLC/CSC funct3 3: a word mask);
  //   quadrant 0: c.lw, c.clc (loads), c.sw, c.csc (stores), Zcb funct3 100: insn[11] store,
  //     insn[10] halfword (c.lbu, c.lhu/c.lh, c.sb, c.sh); quadrant 2: c.lwsp, c.clcsp, c.swsp,
  //     c.cscsp. funct3[2] is the direction in both quadrants.
  static function bit [3:0] insn_access_mask(bit [31:0] insn, output bit store);
    bit [2:0] f3 = insn[15:13];
    store = 1'b0;
    case (insn[1:0])
      2'b11: begin
        if (!(insn[6:0] inside {7'b0000011, 7'b0100011})) return 4'b0000;
        store = insn[5];
        case (insn[13:12])
          2'b00:   return 4'b0001;
          2'b01:   return 4'b0011;
          default: return 4'b1111;
        endcase
      end
      2'b00: begin
        if (f3 == 3'b000) return 4'b0000;
        if (f3 == 3'b100) begin
          store = insn[11];
          return insn[10] ? 4'b0011 : 4'b0001;
        end
        store = f3[2];
        return 4'b1111;
      end
      2'b10: begin
        if (!(f3 inside {3'b010, 3'b011, 3'b110, 3'b111})) return 4'b0000;
        store = f3[2];
        return 4'b1111;
      end
      default: return 4'b0000;
    endcase
  endfunction

endclass
