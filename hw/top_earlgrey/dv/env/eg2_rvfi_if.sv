// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The RVFI of rv_core_ibex's main core, as the EG2 RVFI agent samples it. tb.sv instantiates it
// only when the RTL is built with RVFI (build mode en_ibex_tracer) and drives it from the core
// hierarchically; nothing in the RTL changes.

interface eg2_rvfi_if #(
  parameter int unsigned CapW = 35  // $bits(ibex_cheriot_pkg::cap_t)
) (
  input logic clk,
  input logic rst_n
);
  logic            valid;
  logic [63:0]     order;
  logic [31:0]     insn;
  logic            trap;
  logic            halt;
  logic            intr;
  logic [1:0]      mode;
  logic [4:0]      rs1_addr;
  logic [4:0]      rs2_addr;
  logic [31:0]     rs1_rdata;
  logic [31:0]     rs2_rdata;
  logic [4:0]      rd_addr;
  logic [31:0]     rd_wdata;
  logic [31:0]     pc_rdata;
  logic [31:0]     pc_wdata;
  logic [31:0]     mem_addr;
  logic [3:0]      mem_rmask;
  logic [3:0]      mem_wmask;
  logic [31:0]     mem_rdata;
  logic [31:0]     mem_wdata;
  // CHERIoT
  logic [CapW-1:0] rs1_rcap;
  logic [CapW-1:0] rs2_rcap;
  logic [CapW-1:0] rd_wcap;
  logic            mem_is_cap;
  logic [CapW-1:0] mem_rcap;
  logic [CapW-1:0] mem_wcap;
  // Extensions: what a reference model needs to take interrupts where the core took them.
  logic [31:0]     ext_pre_mip;
  logic [31:0]     ext_post_mip;
  logic            ext_nmi;
  logic            ext_nmi_int;
  logic            ext_debug_req;
  logic            ext_debug_mode;
  logic            ext_rf_wr_suppress;
  logic            ext_irq_valid;
  logic [63:0]     ext_mcycle;
  logic [31:0]     ext_mhpmcounters [10];
  logic [31:0]     ext_mhpmcountersh [10];
  logic            ext_ic_scr_key_valid;
  // Zcmp: Ibex retires one item per micro-operation; the last one carries `_last`.
  logic            ext_expanded_insn_valid;
  logic            ext_expanded_insn_last;
  // The mtval Ibex writes when it takes an internal NMI (the address of the response with bad
  // integrity), which an ISS cannot derive: ibex_controller's irq_nm_int_mtval.
  logic [31:0]     nmi_int_mtval;
  // Core probes (ibex_top's ports, not the bus): a data response with an error, held until the
  // instruction it belongs to retires; the CHERIoT enable the core sees; its boot address.
  // data_err is no longer read: mem_error comes from dside_err of the instruction's own
  // transactions (review R14, 2026-10-26).
  logic            data_err;
  logic            cheriot_ena;
  logic [31:0]     boot_addr;
  // The core's data-side bus transactions, at ibex_core's data port (after its PMP gate), with the
  // load/store unit's misaligned-access flags: what Ibex's co-simulation needs for each access, as
  // the Ibex UVM bench gives it (core_ibex_tb_top.sv, data_mem_vif). One RVFI item carries one
  // mem_error bit for both halves of a misaligned access and nothing for a half that PMP stopped;
  // Spike's memory needs each half as the chip's bus saw it (review R2, 2026-10-25).
  logic            dside_req;          // a request accepted (data_req_o & data_gnt_i)
  logic            dside_we;
  logic [3:0]      dside_be;
  logic [31:0]     dside_addr;
  logic [31:0]     dside_wdata;
  logic            dside_misaligned_first;
  logic            dside_misaligned_second;
  logic            dside_misaligned_first_saw_error;
  logic            dside_m_mode;
  logic            dside_rvalid;       // a response (in request order)
  logic            dside_err;
  logic [31:0]     dside_rdata;
  // The core's ID stage, to tie a fetch error to the instruction that takes it (the Ibex UVM bench's
  // core_ibex_instr_monitor_if and run_cosim_imem_errors): an instruction leaving ID (rvfi_id_done)
  // with its RVFI order, PC and fetch-error flags; a writeback exception flushes it. Review R11,
  // 2026-10-25.
  logic            id_done;
  logic [63:0]     id_order;
  logic [31:0]     id_pc;
  logic            id_fetch_err;
  logic            id_fetch_err_plus2;
  logic            wb_exception;
  // CSRs: mcounteren (pushed into Spike, as the Ibex UVM bench does), mcause and mtval (compared
  // with CHERIoT-Sail's after a CHERIoT-mode trap). Review R13, 2026-10-25.
  logic [31:0]     csr_mcounteren;
  logic [31:0]     csr_mcause;
  logic [31:0]     csr_mtval;

  clocking monitor_cb @(posedge clk);
    input rst_n, valid, order, insn, trap, halt, intr, mode, rs1_addr, rs2_addr, rs1_rdata,
          rs2_rdata, rd_addr, rd_wdata, pc_rdata, pc_wdata, mem_addr, mem_rmask, mem_wmask,
          mem_rdata, mem_wdata, rs1_rcap, rs2_rcap, rd_wcap, mem_is_cap, mem_rcap, mem_wcap,
          ext_pre_mip, ext_post_mip, ext_nmi, ext_nmi_int, ext_debug_req, ext_debug_mode,
          ext_rf_wr_suppress, ext_irq_valid, ext_mcycle, ext_mhpmcounters, ext_mhpmcountersh,
          ext_ic_scr_key_valid, ext_expanded_insn_valid, ext_expanded_insn_last, nmi_int_mtval,
          data_err, cheriot_ena, boot_addr,
          dside_req, dside_we, dside_be, dside_addr, dside_wdata, dside_misaligned_first,
          dside_misaligned_second, dside_misaligned_first_saw_error, dside_m_mode, dside_rvalid,
          dside_err, dside_rdata,
          id_done, id_order, id_pc, id_fetch_err, id_fetch_err_plus2, wb_exception,
          csr_mcounteren, csr_mcause, csr_mtval;
  endclocking

endinterface
