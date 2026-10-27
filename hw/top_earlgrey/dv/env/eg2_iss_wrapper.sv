// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The reference model's instruction-set part (doc/scoreboard/eg2_reference_model.png in
// eg2_top_verification): each RVFI item steps the ISS by one instruction of the same software
// image: Spike in RISC-V mode (phase 1 of the scoreboard plan), through Ibex's co-simulation as
// the Ibex UVM bench uses it: since 2026-10-25 the verified core's own files,
// ibex_cheriot_verification's ibex/dv/cosim (dvsim's EG2_IBEX_DV, chip_sim_cfg.hjson), not the
// vendored hw/vendor/lowrisc_ibex/dv/cosim, which lacked the core bench's fixes (review R1);
// CHERIoT-Sail in CHERIoT mode (phase 2), through ibex_cheriot_verification's CHERIoT-Sail
// co-simulation, until MPACT-CHERIoT takes that place.
//
// - Spike fetches from its own memory, loaded from the ROM and test ELFs the bench loads into the
//   chip (eg2_cosim_load_elf), plus every word written there behind the core's back: rram_ctrl's
//   controller writes (watched here; the words are the ones the core's stores sent to the write
//   FIFO, R7) and the bench's backdoor writes (mem_write_event, backdoor_write), the ROM's and
//   RRAM's kept and written again when Spike restarts after a reset, as the chip keeps them.
// - Data accesses are the core's data-side bus transactions (the RVFI monitor's dside list), told
//   to Spike as the Ibex UVM bench tells it: each half of a misaligned access with its own error,
//   a half PMP stopped absent. Spike checks every access's address and type, every store's data,
//   every register result, PC and trap, and (since review R6, 2026-10-25) every load's data from
//   the memories it holds (ROM, the SRAMs, RRAM; MemBase below, from top_earlgrey_pkg): those
//   come from the images, the tracked writes above and the core's own stores, which Spike
//   executes. A load's value is an event, written into Spike before the step, only
//     - from a device (any address outside MemBase): the plan's phase 1, until the SystemC model
//       predicts it;
//     - of a memory byte the model has no value for: in the ROM or RRAM outside the images and
//       every tracked write; in an SRAM, a byte neither written by the core nor read since the
//       chip reset or since the core last renewed that SRAM's key or started its INIT (sram_ctrl
//       CTRL), after which the chip's contents are undefined. The first read takes the chip's
//       value (counted, reported at the end); every later one is compared. The retention SRAM is
//       treated so too: its contents over a reset are not modelled.
//   A backdoor write reaches Spike for the loads whose responses came after it (eg2_dside_access_t
//   t_resp), not earlier ones in flight.
// - A data access the chip made that no retired instruction takes up is an error (R9): Spike's
//   queue of them (eg2_cosim_num_pending_dside) may hold an access for at most one more step, as
//   an access completes before its instruction retires; CHERIoT mode checks each access against
//   the RVFI record of a load or store. Accesses left at a reset, a stop or the end are reported.
// - A fetch error is given to the model only for an instruction the chip's ID stage took it on (tied
//   to the RVFI order, as the Ibex UVM bench does, R11) at a word the bench armed
//   (iside_error_event): a corrupted prefetch the chip discards is never the model's. Spike's
//   decoded instructions are flushed whenever memory is written behind its back.
// - Interrupts, NMI and debug requests are taken where RVFI shows them taken.
// - Spike starts from the core's reset state, including its PMP and mseccfg reset values.
// - At the CHERIoT lock (the first instruction RVFI shows in CHERIoT mode) the state passes from
//   Spike to CHERIoT-Sail, never from the chip (REQ_SOC_07): Spike's next PC must be where the chip
//   continues; its integer registers x1-x15 become untagged; PCC, MTCC and MEPCC the executable
//   root at the PC, mtvec and mepc; mstatus.MIE/MPIE, mie, mcause, mtval carry over; Spike's memory
//   (the images and every store made in RISC-V mode) becomes CHERIoT-Sail's.
// - In CHERIoT mode CHERIoT-Sail executes the RVFI instruction word: an integer load's value is an
//   event by the same rule as Spike's (devices, and memory bytes the model has no value for;
//   capability loads read its own memory and tags), interrupts are taken from its own state when
//   RVFI shows one taken, and every result, capability tag and trap is checked; every integer
//   store's address and data too. A bus error the chip took on a fetch the bench armed or on a
//   data access is given to it as an access fault (cheriot_sail_cosim_step_bus_error, R12), and
//   after every trap the chip's mcause and mtval are compared with the model's (R13).
// - A stop (stop_after_fault, stop_until_reset) ends the checks until the next chip reset; the
//   instructions not compared are counted, and a stop no reset ends fails the run (R8).
// - Reviews R15, R18, R19 (2026-10-26): rram_ctrl's programmed words are checked where they are
//   written (a Write's against the words sent, a Rewrite's against the model's own word, a word
//   no documented writer sent and a sent word never programmed are errors); in CHERIoT mode every
//   bus store's bytes are compared with CHERIoT-Sail's, capability loads follow the SRAM rule,
//   CHERIoT-Sail revokes from the chip's bitmap (RevHeapBase) and its tags follow INIT, key
//   renewal, rram_ctrl programs and the revocation engine's clears; the writes behind the core's
//   back reach the model by the oldest access it has not executed, SRAM INIT and key renewal in
//   bus order, and the bench's set_mem and RRAM reloads have hooks (backdoor_undefined,
//   rram_image_reloaded).
//
// Each step's outcome goes to eg2_scoreboard as an eg2_rvfi_exp_item (expected side). Built only
// with EG2_COSIM (build mode en_eg2_cosim); otherwise the wrapper is present but off. With
// EG2_SYSMODEL too (build mode en_eg2_model) each data access also goes to the EG2 SystemC model
// (eg2_sysmodel.sv), which predicts the loads it can; its findings fail the instruction.
//
// Plusargs: +sb_corrupt=insn.rd (corrupts one register result before it is checked, from the 100th
// instruction), +sb_corrupt=insn.cheriot_rd (the same from the 100th CHERIoT-mode instruction, so
// CHERIoT-Sail's check is the one shown to fail); the run must fail. +eg2_spike_log=1 (Spike's
// commit log, spike.model.log).

class eg2_rvfi_exp_item extends uvm_sequence_item;
  bit [63:0]    order;
  bit [31:0]    pc;
  bit           match;
  string        errors;
  eg2_rvfi_item act;

  `uvm_object_utils(eg2_rvfi_exp_item)
  `uvm_object_new
endclass

`ifdef EG2_COSIM
  `include "spike_cosim_dpi.svh"
  `include "cosim_dpi.svh"
  `include "cheriot_sail_cosim_dpi.svh"
  import "DPI-C" function int eg2_cosim_load_elf(chandle cosim_handle, string path);
  import "DPI-C" function int unsigned eg2_cosim_get_gpr(chandle cosim_handle, int idx);
  import "DPI-C" function int unsigned eg2_cosim_get_pc(chandle cosim_handle);
  import "DPI-C" function int unsigned eg2_cosim_get_csr(chandle cosim_handle, int csr_num);
  import "DPI-C" function int eg2_cosim_copy_mem_to_cheriot_sail(chandle cosim_handle,
                                                                 int unsigned base,
                                                                 int unsigned size);
  // Reviews R6, R9, R11 (2026-10-25): the image ranges loaded, Spike's unmatched data accesses,
  // its decoded-instruction cache.
  import "DPI-C" function int eg2_cosim_elf_covers(int unsigned addr);
  import "DPI-C" function void eg2_cosim_elf_forget();
  import "DPI-C" function int eg2_cosim_num_pending_dside(chandle cosim_handle);
  import "DPI-C" function void eg2_cosim_flush_icache(chandle cosim_handle);
  // CHERIoT-Sail takes a bus error the chip took as an access fault (review R12, 2026-10-25;
  // ibex_cheriot_verification's cheriot_sail_cosim_dpi.h: libcheriotsail_rvfi.so must be rebuilt).
  // kind 0 fetch, 1 load, 2 store. Imported here, not in the shared .svh, so the core bench does
  // not need the rebuilt library.
  import "DPI-C" function int cheriot_sail_cosim_step_bus_error(bit [31:0] pc, int kind,
                                                                 bit [31:0] tval);
  // Reviews R15, R18, R19 (2026-10-26): the images' bytes (a Rewrite's expected word, the RRAM
  // image the bench puts back); CHERIoT-Sail's revocation layout and tag clears
  // (cheriot_sail_cosim_revocation.cc, in libcheriotsail_rvfi.so; imported here as the CHERIoT
  // RTOS bench's soc_iss_wrapper.sv imports them, not in the shared .svh).
  import "DPI-C" function int eg2_cosim_elf_byte(int unsigned addr);
  import "DPI-C" function int eg2_cosim_elf_restore(chandle cosim_handle, int unsigned base,
                                                    int unsigned size, int to_sail);
  import "DPI-C" function void cheriot_sail_cosim_set_revocation(int unsigned heap_base,
                                                                  int unsigned heap_size,
                                                                  int unsigned bits_base);
  import "DPI-C" function void cheriot_sail_cosim_clear_tag(int unsigned addr);
`endif

class eg2_iss_wrapper extends uvm_component;
  `uvm_component_utils(eg2_iss_wrapper)

  chip_env_cfg cfg;

  uvm_tlm_analysis_fifo #(eg2_rvfi_item) rvfi_fifo;
  uvm_analysis_port #(eg2_rvfi_exp_item) exp_port;

  // Set by chip_env: the RVFI agent has an interface and the build has the co-simulation.
  bit enabled;

  // The core's configuration (rv_core_ibex's parameters, from tb.sv).
  int unsigned pmp_num_regions, pmp_granularity, mhpm_counter_num, rv32b, rv32zc;
  bit          secure_ibex, icache;
  bit [31:0]   dm_base_addr, dm_addr_mask;

  protected chandle      cosim;
  protected bit          stopped;
  // CHERIoT mode: CHERIoT-Sail has the state (from the handover at the lock).
  protected bit          sail_active;
  protected bit          sail_prev_trap;
  // The previous trap was an NMI or debug entry, which the model has no counterpart for (R13).
  protected bit          sail_prev_skip;
  protected bit [31:0]   boot_addr_q;
  protected bit          corrupt_rd;
  protected bit          corrupt_cheriot_rd;
  protected int unsigned sail_steps;
  protected int unsigned stepped;

  // The wrapper of this run (null without one), for the sequences' backdoor writes (backdoor_write).
  // 2026-10-25.
  static eg2_iss_wrapper inst;

  // The memories the model holds (review R6, 2026-10-25), from the top's memory map: a load from
  // them is compared with the model's own copy; anything else is a device. Non-volatile ones keep
  // their contents over a chip reset; volatile ones (sram_ctrl's) lose them, and when the core
  // renews their key or starts their INIT through MemCtrlRegs' CTRL register.
  typedef enum bit [1:0] {MemNone, MemNv, MemVol} mem_kind_e;
  localparam int unsigned NumMemRegions = 6;
  localparam bit [31:0] MemBase[NumMemRegions] = '{
      top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR};
  localparam bit [31:0] MemSize[NumMemRegions] = '{
      top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_META_RAM_SIZE_BYTES,
      top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES};
  localparam mem_kind_e MemKind[NumMemRegions] = '{MemNv, MemVol, MemVol, MemVol, MemVol, MemNv};
  localparam bit [31:0] MemCtrlRegs[NumMemRegions] = '{
      32'h0,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_REGS_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_REGS_BASE_ADDR,
      top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR,
      32'h0};
  localparam bit [31:0] RramHost = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR;
  localparam bit [31:0] RramCore = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR;

  // Every byte written behind the core's back into the ROM or RRAM (mem_write_event,
  // backdoor_write, rram_ctrl's programmed words) or first read there without a value in the model:
  // the chip keeps them over a reset, so a restarted Spike gets them again. 2026-10-25.
  protected bit [7:0]    nv_mem[bit [31:0]];
  // The SRAM bytes the model has a value for since the last reset or invalidation, per word. R6.
  protected bit [3:0]    vol_known[bit [31:0]];
  // Writes behind the core's back not yet given to the model: each reaches it before the first
  // instruction whose data came after it. R6.
  // Since 2026-10-26 also what else happens to memory behind the core's back, in the same time
  // order: a byte (WrByte; untag: rram_ctrl programmed it, which leaves its capability tag 0,
  // REQ_CMS_16, R18), a range whose contents become undefined (WrUnknown: the bench's set_mem,
  // R19), the images put back over a range (WrImage: the bench reloads the RRAM, R19), a
  // capability tag the revocation engine cleared (WrUntag, R18).
  typedef enum bit [1:0] {WrByte, WrUnknown, WrImage, WrUntag} wr_kind_e;
  typedef struct {
    realtime   t;
    wr_kind_e  kind;
    bit [31:0] addr;
    bit [31:0] size;
    bit [7:0]  data;
    bit        untag;
  } mem_wr_t;
  protected mem_wr_t     pending_wr[$];
  protected int unsigned unknown_reads;

  // The chip's data accesses the model has been told of and has not taken up, each with the step
  // it came with: at most one more step may pass before its instruction retires. R9, 2026-10-25.
  typedef struct {
    eg2_dside_access_t a;
    int unsigned       carrier;
  } told_access_t;
  protected told_access_t spike_q[$];
  protected told_access_t sail_q[$];
  protected int unsigned  spike_steps;
  localparam int unsigned MaxCarry = 2;

  // Fetch errors the bench made (iside_error_event), by word address, until an instruction the
  // chip took a fetch error on there retires (R11), and the unarmed ones already reported.
  protected bit          iside_armed[bit [31:0]];
  protected bit          iside_unarmed_seen[bit [31:0]];

  // Stops (R8): asked for (stop_after_fault arms it for the next data-access fault), in effect
  // until the next chip reset, and what was not compared meanwhile.
  protected string       stop_why;
  protected bit          stop_armed;
  protected bit          stop_open;
  protected realtime     stop_t;
  protected int unsigned dropped, dropped_total;
  // Data-access bus errors on the item that took them (mem_error), and those CHERIoT-Sail was given
  // (cheriot_sail_cosim_step_bus_error): reported, so a run shows they reached the model. R14,
  // 2026-10-26.
  protected int unsigned mem_faults, sail_bus_errors;

  // rram_ctrl's controller writes (R7, R10): the core's write operation as its stores set it up
  // (ADDR, CONTROL with START, Write, data partition; the words it stores to wr_fifo), the words
  // the PHY accepted and not yet programmed, and the programmed ones not yet given to the model.
  protected bit [31:0]   rram_addr_reg;
  protected bit [31:0]   rram_op_base;
  protected int unsigned rram_op_num, rram_op_idx;
  protected bit [31:0]   rram_sw_words[bit [31:0]];
  protected bit [31:0]   rram_waiting[bit [31:0]];
  protected bit          rram_waiting_rma[bit [31:0]];
  typedef struct {
    realtime   t;
    bit [31:0] addr;
    bit [31:0] phy_data;
    bit        rma;  // programmed while lc_ctrl requested RMA (R15, 2026-10-26)
  } rram_prog_t;
  protected rram_prog_t  rram_programmed[$];
  protected int unsigned rram_words_core, rram_words_other;
  // Review R15, 2026-10-26: the controller operation the core (or the bench over SBA) started last,
  // until the next START, a reset or the end (rram_close_op): its kind, whether the core read
  // OP_STATUS with done or err set since, and,
  // for a Rewrite (OP 2), the row's four words still to be written back; who sent it; the words
  // programmed by the documented other writers (a Rewrite, otp_ctrl's OTP region, the RMA wipe)
  // and those no writer accounts for.
  typedef enum bit [1:0] {RramOpNone, RramOpWrite, RramOpRewrite} rram_op_e;
  protected rram_op_e    rram_op;
  protected string       rram_op_by;
  protected bit          rram_done_seen, rram_err_seen;
  protected bit          rram_rewrite_left[bit [31:0]];
  protected int unsigned rram_words_rewrite, rram_words_rewrite_unknown, rram_words_otp,
                         rram_words_rma, rram_words_unexplained;
  // The revocation engine's tag clears given to CHERIoT-Sail's queue (R18, 2026-10-26).
  protected int unsigned tbre_clears;
  // The OTP region: the data partition's top OtpPages (6) pages of 32 16-byte words, which only
  // otp_ctrl writes, through rram_ctrl's OTP plug (rram_ctrl programmers_guide.md "Accessing the
  // OTP Region"; rram_ctrl_pkg OtpPages, TotalOtpBytes 3 KiB). R15, 2026-10-26.
  localparam bit [31:0] RramOtpBase = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR +
                                      top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES -
                                      32'd6 * 32'd512;

  // CHERIoT-Sail's revocation layout, from the documented map (eg2_toplevel_verification_spec.md
  // "Memory subsystem", cheriot_mem_sys theory_of_operation.md "Meta SRAM Address Map"): the 3 KiB
  // bitmap at the base of the meta SRAM (the software window at 0x1100_0000), one bit per 8 heap
  // bytes from 0x1000_0000, so main and sec SRAM, 0x1000_0000-0x1002_FFFF. Tags: one per 8 bytes
  // of main SRAM, sec SRAM and RRAM. Review R18, 2026-10-26 (CHERIoT-Sail never revoked: its
  // default heap is its RAM at 0x8000_0000).
  localparam bit [31:0] RevHeapBase  = 32'h1000_0000;
  localparam bit [31:0] RevHeapSize  = 32'h0003_0000;
  localparam bit [31:0] RevBitsBase  = 32'h1100_0000;
  localparam bit [31:0] RevBitsBytes = 32'h0000_0C00;

  `uvm_component_new

  // Compiled with the co-simulation (EG2_COSIM).
  static function bit compiled();
`ifdef EG2_COSIM
    return 1'b1;
`else
    return 1'b0;
`endif
  endfunction

  function void build_phase(uvm_phase phase);
    string sb_corrupt;
    string cfg_names[] = '{"pmp_num_regions", "pmp_granularity", "mhpm_counter_num", "rv32b",
                           "rv32zc", "secure_ibex", "icache", "dm_base_addr", "dm_addr_mask"};
    super.build_phase(phase);
    inst      = this;
    rvfi_fifo = new("rvfi_fifo", this);
    exp_port  = new("exp_port", this);
    void'($value$plusargs("sb_corrupt=%s", sb_corrupt));
    corrupt_rd         = sb_corrupt == "insn.rd";
    corrupt_cheriot_rd = sb_corrupt == "insn.cheriot_rd";
    foreach (cfg_names[i]) begin
      int unsigned v;
      if (!uvm_config_db#(int unsigned)::get(this, "", cfg_names[i], v) && compiled()) begin
        `uvm_fatal(`gfn, $sformatf("no %s from tb.sv", cfg_names[i]))
      end
      set_cfg(cfg_names[i], v);
    end
  endfunction

  protected function void set_cfg(string name, int unsigned v);
    case (name)
      "pmp_num_regions":  pmp_num_regions  = v;
      "pmp_granularity":  pmp_granularity  = v;
      "mhpm_counter_num": mhpm_counter_num = v;
      "rv32b":            rv32b            = v;
      "rv32zc":           rv32zc           = v;
      "secure_ibex":      secure_ibex      = v;
      "icache":           icache           = v;
      "dm_base_addr":     dm_base_addr     = v;
      "dm_addr_mask":     dm_addr_mask     = v;
      default: `uvm_fatal(`gfn, name)
    endcase
  endfunction

  // Spike's ISA string for this core, by the Ibex UVM bench's rule (core_ibex_base_test).
  function string isa_string();
    string isa = "rv32imc";
    case (rv32b)
      int'(ibex_pkg::RV32BBalanced): isa = {isa, "_Zba_Zbb_Zbs"};
      int'(ibex_pkg::RV32BFull):     isa = {isa, "_Zba_Zbb_Zbc_Zbs_Zbkb_Zbkx"};
      default: ;
    endcase
    case (rv32zc)
      int'(ibex_pkg::RV32ZcaZcb):     isa = {isa, "_Zcb"};
      int'(ibex_pkg::RV32ZcaZcmp):    isa = {isa, "_Zcmp"};
      int'(ibex_pkg::RV32ZcaZcbZcmp): isa = {isa, "_Zcb_Zcmp"};
      default: ;
    endcase
    if (mhpm_counter_num > 0) isa = {isa, "_Zihpm"};
    return isa;
  endfunction

  virtual task run_phase(uvm_phase phase);
    eg2_rvfi_item item;
    if (!enabled) return;
`ifdef EG2_SYSMODEL
    eg2_sysmodel::watch_clocks(cfg.chip_vif);
`endif
    fork
      watch_rram_writes();
      watch_tbre_clears();  // R18, 2026-10-26
    join_none
    forever begin
      rvfi_fifo.get(item);
      handle(item);
    end
  endtask

`ifdef EG2_COSIM
  protected function void handle(eg2_rvfi_item item);
    eg2_rvfi_exp_item exp;
    bit [31:0] rd_wdata;

    if (item.reset) begin
`ifdef EG2_PERIPH_SB
      // Announced faults end at a chip reset (review R12, 2026-10-25).
      if (eg2_inject_registry::active()) eg2_inject_registry::chip_reset(item.reset_start);
`endif
      if (stop_open) begin
        `uvm_info(`gfn, $sformatf("EG2_ISS the chip reset ends the stop (%s): %0d items not compared",
                                  stop_why, dropped), UVM_LOW)
        stop_open = 1'b0;
      end
      report_left("the chip reset");
      sail_active = 1'b0;
      restart(item.boot_addr);
`ifdef EG2_SYSMODEL
      if (cosim != null) eg2_sysmodel::chip_reset(cfg);
`endif
      return;
    end
    if (cosim == null) return;
    if (stopped) begin
      // rram_ctrl's operations are still followed, so their words are accounted for (R15,
      // 2026-10-26: a word programmed by no operation the wrapper saw is an error now).
      track_stores(item);
      // Not compared, and counted (R8, 2026-10-25).
      if (!item.irq_only) begin
        dropped++;
        dropped_total++;
      end
      return;
    end
    // What the core's stores set up behind the model's back (rram_ctrl's controller operations);
    // then the writes behind the core's back the item's data accesses come after. R6, R7,
    // 2026-10-25. SRAM key renewal and INIT are taken in bus order with the loads and stores
    // since R19 (sram_ctrl_store, 2026-10-26).
    track_stores(item);
    apply_pending_writes(item);
    // The store to CHERIOT_LOCK that switches the mode retires after its write has changed
    // cheriot_ena, so its item already reads CHERIoT; it executed in RISC-V mode and is Spike's.
    // The handover is at the next instruction.
    if (item.cheriot && !(!sail_active && is_lock_store(item))) begin
      if (!sail_active && !item.irq_only) handover(item);
      // Every data access, also an interrupt-only item's, is accounted for (R9).
      foreach (item.dside[i]) sail_q.push_back('{a: item.dside[i], carrier: sail_steps});
      if (item.irq_only) return;  // CHERIoT-Sail takes interrupts from its own state
      sail_step(item);
      check_stop_trigger(item);
      return;
    end

    // The bus transactions completed before this item, before anything else: Spike matches its own
    // accesses against them in order, whichever item they came with.
    notify_dside(item);

    if (item.irq_only) begin
      // Only an interrupt notification: no instruction retired. As the core bench
      // (ibex_cosim_scoreboard.sv): the internal NMI's mtval, and set_irq_taken, since the RVFI
      // notification now comes when the core commits to the interrupt (R3, the vendored
      // ibex_core.sv's capture ported 2026-10-25), so the model takes it ahead of a later NMI or
      // debug request even if it was pending, masked, before.
      riscv_cosim_set_nmi(cosim, item.nmi);
      riscv_cosim_set_nmi_int(cosim, item.nmi_int, item.nmi_int_mtval);
      riscv_cosim_set_mip(cosim, item.pre_mip, item.pre_mip);
      riscv_cosim_set_irq_taken(cosim);
      return;
    end

    // In this order, so debug, NMI and interrupts arriving together are taken by priority.
    riscv_cosim_set_debug_req(cosim, item.debug_req);
    riscv_cosim_set_nmi(cosim, item.nmi);
    riscv_cosim_set_nmi_int(cosim, item.nmi_int, item.nmi_int_mtval);
    riscv_cosim_set_mip(cosim, item.pre_mip, item.post_mip);
    riscv_cosim_set_mcycle(cosim, item.mcycle);
    for (int i = 0; i < 10; i++) begin
      riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_MHPMCOUNTER3) + i, item.mhpmcounters[i]);
      riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_MHPMCOUNTER3H) + i, item.mhpmcountersh[i]);
    end
    // Before the step, as the Ibex UVM bench pushes it (ibex_cosim_scoreboard.sv): Spike checks
    // mcounteren when it executes a U-mode counter read. A WARL input Spike cannot be relied on to
    // reproduce, pushed, not compared (the core's formal proof compares it). R13, 2026-10-25.
    riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_MCOUNTEREN), item.mcounteren);
    riscv_cosim_set_ic_scr_key_valid(cosim, item.ic_scr_key_valid);
    // A fetch error the chip took on this instruction at a word the bench armed: Spike's fetch there
    // fails in this step (R11, 2026-10-25).
    if (item.iside_err) begin
      if (iside_armed.exists(item.iside_err_addr)) begin
        void'(riscv_cosim_set_iside_error(cosim, item.iside_err_addr));
        // Spike's decoded-instruction cache serves an instruction it ran before without a fetch,
        // so the armed error did not fire on a re-executed instruction (the trap went missing and
        // the error stayed armed for a later access to the word): dropped, so this step fetches.
        // Review R17, 2026-10-26. (The core's SpikeCosim::set_iside_error flushes too since then;
        // this call covers a checkout without it.)
        eg2_cosim_flush_icache(cosim);
        iside_armed.delete(item.iside_err_addr);
        `uvm_info(`gfn, $sformatf("EG2_ISS fetch error at %08h given to Spike (order %0d, pc %08h)",
                                  item.iside_err_addr, item.order, item.pc), UVM_LOW)
      end else begin
        report_unarmed_iside(item);
      end
    end

    rd_wdata = item.rd_wdata;
    if (corrupt_rd && item.rd_addr != 0 && stepped >= 100) begin
      rd_wdata   = ~rd_wdata;
      corrupt_rd = 1'b0;
      `uvm_info(`gfn, $sformatf("+sb_corrupt=insn.rd: x%0d of order %0d corrupted",
                                item.rd_addr, item.order), UVM_LOW)
`ifdef EG2_PERIPH_SB
      if (eg2_inject_registry::active()) begin
        void'(eg2_inject_registry::announce("sb_corrupt insn.rd", "insn",
            $sformatf("x%0d of order %0d inverted", item.rd_addr, item.order), $realtime, $realtime));
      end
`endif
    end

    exp       = eg2_rvfi_exp_item::type_id::create("exp");
    exp.order = item.order;
    exp.pc    = item.pc;
    exp.act   = item;
    // The core's co-simulation (R1, 2026-10-25) takes `intr` (the deferred-interrupt path:
    // set_mip() no longer takes an interrupt by itself, step() does when RVFI shows the handler
    // entered) and `more_ops` (a Zcmp micro-operation that is not the last only records its
    // register write; the ISS steps the whole instruction on the last one).
    exp.match = riscv_cosim_step(cosim, item.rd_addr, rd_wdata, item.pc, item.intr, item.trap,
                                 item.rf_wr_suppress, item.more_ops);
    if (!exp.match) begin
      for (int i = 0; i < riscv_cosim_get_num_errors(cosim); i++) begin
        exp.errors = {exp.errors, riscv_cosim_get_error(cosim, i), "; "};
      end
      riscv_cosim_clear_errors(cosim);
    end
    if (!item.more_ops) begin
      spike_steps++;
      check_spike_left(exp);
    end
`ifdef EG2_SYSMODEL
    model_check(exp, eg2_sysmodel::access(item));
`endif
    stepped++;
    exp_port.write(exp);
    check_stop_trigger(item);
  endfunction

  // A fresh Spike at the core's reset state, loaded with the run's ROM and test images.
  protected function void restart(bit [31:0] boot_addr);
    bit en_log;
    string images[$];
    if (cosim != null) spike_cosim_release(cosim);
    void'($value$plusargs("eg2_spike_log=%b", en_log));
    if (cfg.rom_ext_assembled) begin
      // ROM_EXT and the owner image are one assembled image here, not two ELFs at their slots.
      `uvm_info(`gfn, "EG2_ISS off in secure-boot runs (assembled image); phase 1 covers test-ROM runs",
                UVM_LOW)
      cosim   = null;
      stopped = 1'b1;
      return;
    end
    boot_addr_q = boot_addr;
    // Ibex starts at boot_addr + 0x80 with mtvec at boot_addr, vectored.
    cosim = spike_cosim_init(isa_string(), boot_addr + 32'h80, boot_addr | 32'h1,
                             en_log ? "spike.model.log" : "", pmp_num_regions, pmp_granularity,
                             mhpm_counter_num, secure_ibex, icache, dm_base_addr,
                             dm_base_addr + dm_addr_mask);
    if (cosim == null) `uvm_fatal(`gfn, "EG2_ISS cannot start Spike")
    set_pmp_reset();
    // Spike's cpuctrlsts never initialises its stored value (riscv-isa-sim 4b97396,
    // riscv/csrs.cc cpuctrlsts_csr_t), so sync_exc_seen and double_fault_seen can read 1
    // before any trap. Ibex resets the register to 0.
    riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_CPUCTRLSTS), 32'h0);

    // Only the slots the test has: a test_in_rom image (eg2_boot_*) is the ROM alone, with no
    // flash-slot image, and loading the empty name failed as "cannot load .elf".
    foreach (cfg.sw_images[t]) begin
      if (t inside {SwTypeRom, SwTypeTestSlotA} && cfg.sw_images[t] != "") begin
        images.push_back({cfg.sw_images[t], ".elf"});
      end
    end
    if (images.size() == 0) `uvm_fatal(`gfn, "EG2_ISS no ROM or flash-slot image to load")
    eg2_cosim_elf_forget();
    foreach (images[i]) begin
      int n = eg2_cosim_load_elf(cosim, images[i]);
      if (n < 0) `uvm_fatal(`gfn, $sformatf("EG2_ISS cannot load %s into Spike", images[i]))
      `uvm_info(`gfn, $sformatf("EG2_ISS %0d bytes of %s loaded", n, images[i]), UVM_MEDIUM)
    end
    // The words rram_ctrl programmed before the reset are in the array (R7, R10); its write
    // operation ends with the reset. The SRAMs' contents, the writes into them not yet given, the
    // accesses and fetch errors of the last boot are gone (R6, R9, R11). 2026-10-25.
    resolve_rram_programmed($realtime);
    // The words the operation sent and the controller never programmed: an error if the core had
    // read OP_STATUS.done, else the reset may have cut the operation (reported). R15, 2026-10-26.
    rram_close_op("the chip reset", 1'b0);
    vol_known.delete();
    pending_wr.delete();
    spike_q.delete();
    sail_q.delete();
    iside_armed.delete();
    // Then the bytes written behind the core's back since the run began (mem_write_event,
    // backdoor_write, rram_ctrl's programs) or first read there, which the chip's ROM and RRAM
    // still hold. 2026-10-25.
    foreach (nv_mem[a]) riscv_cosim_write_mem_byte(cosim, a, nv_mem[a]);
    if (nv_mem.size() != 0) begin
      `uvm_info(`gfn, $sformatf("EG2_ISS %0d ROM/RRAM bytes written since the run began given again",
                                nv_mem.size()), UVM_LOW)
    end
    stopped = 1'b0;
    `uvm_info(`gfn, $sformatf("EG2_ISS Spike started: %s, pc %08h", isa_string(),
                              boot_addr + 32'h80), UVM_LOW)
  endfunction

  // The PMP and mseccfg reset values rv_core_ibex has (ibex_pmp_reset_pkg). mseccfg first:
  // Smepmp lets RLB be set only while no entry is locked, so written after the locked reset
  // entries it would stay 0 in Spike, while the chip resets with RLB = 1 and the ROM rewrites
  // locked entries. Then addresses before configurations (a locked entry refuses address writes).
  protected function void set_pmp_reset();
    bit [31:0] pmpcfg[4];
    riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_MSECCFG),
                        {29'b0, ibex_pmp_reset_pkg::PmpMseccfgRst.rlb,
                         ibex_pmp_reset_pkg::PmpMseccfgRst.mmwp,
                         ibex_pmp_reset_pkg::PmpMseccfgRst.mml});
    for (int i = 0; i < 16; i++) begin
      ibex_pkg::pmp_cfg_t c = ibex_pmp_reset_pkg::PmpCfgRst[i];
      riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_PMPADDR0) + i,
                          ibex_pmp_reset_pkg::PmpAddrRst[i][33:2]);
      pmpcfg[i / 4][8 * (i % 4) +: 8] = {c.lock, 2'b00, 2'(c.mode), c.exec, c.write, c.read};
    end
    foreach (pmpcfg[i]) riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_PMPCFG0) + i, pmpcfg[i]);
  endfunction

  // Hand the core's data-side bus transactions to Spike, as the Ibex UVM bench does
  // (ibex_cosim_scoreboard.sv run_cosim_dmem): each word transaction with its own error and the
  // load/store unit's misaligned flags. This replaced (review R2, 2026-10-25) a split of the RVFI
  // item's access into word halves, which
  //   - put the item's one mem_error bit on both halves, so a misaligned store whose second half
  //     alone got a bus error wrote nothing in Spike (its first byte already "failed") while the
  //     chip wrote the first half, and one whose first half alone failed never had its second half
  //     applied (spike_cosim.cc misaligned_store_second_half needs the second half error-free);
  //   - dropped every access of a trapping instruction without mem_error, so the half of a
  //     misaligned store that passed PMP, which the chip wrote, never reached Spike's memory
  //     (spike_cosim.cc misaligned_pmp_fixup needs it queued).
  // A request PMP stopped never reaches the bus and is absent here, as on the core bench.
  // A load's value goes into Spike's memory before the step only where it is an event (feed_load:
  // a device, or a memory byte the model has no value for); otherwise Spike checks it against its
  // own memory (review R6, 2026-10-25: every load was written in, so Spike compared each load with
  // the chip's own bytes, in every memory). Each access is kept until Spike takes it up (R9).
  protected function void notify_dside(eg2_rvfi_item item);
    foreach (item.dside[i]) begin
      eg2_dside_access_t a = item.dside[i];
      // In bus order: a store defines the SRAM bytes for the loads after it, not before; so does a
      // store to an sram_ctrl's CTRL undefine them (R19, 2026-10-26: track_stores did that for the
      // whole item first, so a store before it in the same item stayed defined).
      if (!a.store && !a.err) feed_load(a.addr, a.be, a.rdata);
      if (a.store && !a.err) sram_ctrl_store(a);
      if (a.store && !a.err) store_defines(a.addr, a.be);
      riscv_cosim_notify_dside_access(cosim, a.store, a.addr, a.store ? a.wdata : a.rdata, a.be,
                                      a.err, a.misaligned_first, a.misaligned_second,
                                      a.misaligned_first_saw_error, a.m_mode);
      spike_q.push_back('{a: a, carrier: spike_steps});
    end
  endfunction

  // Which memory `addr` is in (MemBase), or MemNone (a device). R6, 2026-10-25.
  protected function mem_kind_e mem_kind(bit [31:0] addr, output int unsigned region);
    for (int r = 0; r < NumMemRegions; r++) begin
      if (addr >= MemBase[r] && addr - MemBase[r] < MemSize[r]) begin
        region = r;
        return MemKind[r];
      end
    end
    region = NumMemRegions;
    return MemNone;
  endfunction

  // Whether the model has a value of its own for the memory byte at `addr`: the ROM and RRAM from
  // the images and every byte written there behind the core's back; an SRAM byte written by the
  // core (or first read) since the last reset or invalidation. R6, 2026-10-25.
  protected function bit model_has(bit [31:0] addr);
    int unsigned r;
    bit [31:0]   w = {addr[31:2], 2'b00};
    case (mem_kind(addr, r))
      MemNv:   return nv_mem.exists(addr) || eg2_cosim_elf_covers(addr) != 0;
      MemVol:  return vol_known.exists(w) && vol_known[w][addr[1:0]];
      default: return 1'b0;
    endcase
  endfunction

  // The bytes of a load the chip read (word address, byte enables, data) that are events, into the
  // model of the mode (Spike, or CHERIoT-Sail once it has the state): a device's, every time; a
  // memory byte's, the first time it is read without a value in the model (then it has one).
  // R6, 2026-10-25.
  protected function void feed_load(bit [31:0] waddr, bit [3:0] be, bit [31:0] rdata);
    bit fed_mem;
    for (int b = 0; b < 4; b++) begin
      bit [31:0]   addr = waddr + b;
      int unsigned r;
      mem_kind_e   k;
      if (!be[b]) continue;
      k = mem_kind(addr, r);
      if (k != MemNone && model_has(addr)) continue;
      model_write_byte(addr, rdata[8 * b +: 8]);
      if (k == MemNone) continue;
      unknown_reads++;
      fed_mem = 1'b1;
      if (k == MemNv) nv_mem[addr] = rdata[8 * b +: 8];
      else mark_known(addr, 4'b0001 << addr[1:0]);
    end
    if (fed_mem && !sail_active) eg2_cosim_flush_icache(cosim);
  endfunction

  // A store of the core (the model executes it) gives the model the SRAM bytes it writes. R6.
  protected function void store_defines(bit [31:0] waddr, bit [3:0] be);
    int unsigned r;
    if (mem_kind(waddr, r) == MemVol) mark_known(waddr, be);
  endfunction

  protected function void mark_known(bit [31:0] addr, bit [3:0] be);
    bit [31:0] w = {addr[31:2], 2'b00};
    if (!vol_known.exists(w)) vol_known[w] = '0;
    vol_known[w] |= be;
  endfunction

  // One byte into the memory of the model that has the state now.
  protected function void model_write_byte(bit [31:0] addr, bit [7:0] data);
    if (sail_active) cheriot_sail_cosim_write_mem_byte(addr, data);
    else if (cosim != null) riscv_cosim_write_mem_byte(cosim, addr, data);
  endfunction

  // The core's accesses (without an error) of this item to rram_ctrl's registers, which change the
  // RRAM behind the model's back (R6, R7, 2026-10-25; R15, 2026-10-26): the operation the core
  // starts (rram_reg_store), and OP_STATUS as the core reads it.
  protected function void track_stores(eg2_rvfi_item item);
    foreach (item.dside[i]) begin
      eg2_dside_access_t a = item.dside[i];
      if (a.err) continue;
      if (a.store) begin
        rram_reg_store(a.addr, a.wdata, a.t_resp, "the core");
      end else if (a.addr == RramCore + 32'(rram_ctrl_reg_pkg::RRAM_CTRL_OP_STATUS_OFFSET) &&
                   a.be[0] && a.rdata[0]) begin
        // OP_STATUS.done (bit 0): every word of the operation is programmed (programmers_guide.md
        // "When all words have been programmed, the controller sets OP_STATUS.done"); err (bit 1):
        // it failed (mp_err, a write error), and need not have programmed them all. Remembered,
        // not acted on: software clears the register itself, so a read right after a START may
        // still show the previous operation's done.
        if (rram_op != RramOpNone) begin
          rram_done_seen = 1'b1;
          rram_err_seen |= a.rdata[1];
        end
      end
    end
  endfunction

  // A write to rram_ctrl's ADDR, CONTROL (START with OP Write on the data partition starts a write
  // of NUM + 1 bus words from ADDR, truncated to 16 bytes, and clears the write FIFO; with OP
  // Rewrite, one RRAM word, four bus words, read and written back) or wr_fifo (the operation's next
  // word), by the core or by the bench over SBA (backdoor_write32): the words the controller is to
  // program, from those stores, which the model checks (programmers_guide.md "Issuing a Controller
  // Write", "Issuing a Rewrite Operation"). A START first settles the operation before it (its
  // programmed words, then the words left over, rram_close_op). R7, 2026-10-25; R15, 2026-10-26.
  protected function void rram_reg_store(bit [31:0] addr, bit [31:0] wdata, realtime t, string by);
    if (addr == RramCore + 32'(rram_ctrl_reg_pkg::RRAM_CTRL_ADDR_OFFSET)) begin
      rram_addr_reg = {11'b0, wdata[20:0]};  // ADDR.START, 20:0
    end else if (addr == RramCore + 32'(rram_ctrl_reg_pkg::RRAM_CTRL_CONTROL_OFFSET) && wdata[0]) begin
      resolve_rram_programmed(t);
      rram_close_op("the next START", 1'b1);
      rram_op_idx  = 0;
      rram_op_base = {rram_addr_reg[31:4], 4'h0};
      rram_op_by   = by;
      // OP (5:4) Write = 1, Rewrite = 2; PARTITION (8) data = 0; NUM (25:16).
      rram_op_num = 0;
      if (!wdata[8] && wdata[5:4] == 2'd1) begin
        rram_op     = RramOpWrite;
        rram_op_num = int'(wdata[25:16]) + 1;
      end else if (!wdata[8] && wdata[5:4] == 2'd2) begin
        rram_op = RramOpRewrite;
        for (int w = 0; w < 4; w++) rram_rewrite_left[RramHost + rram_op_base + 4 * w] = 1'b1;
      end
    end else if (addr == RramCore + 32'(rram_ctrl_reg_pkg::RRAM_CTRL_WR_FIFO_OFFSET) &&
                 rram_op == RramOpWrite && rram_op_idx < rram_op_num) begin
      rram_sw_words[RramHost + rram_op_base + 4 * rram_op_idx] = wdata;
      rram_op_idx++;
    end
  endfunction

  // The end of a controller operation (R15, 2026-10-26: the words left were deleted unchecked at
  // the next START or reset, so a controller that dropped words passed): a word the operation sent
  // that the controller never programmed is an error once the operation is over (the next START,
  // which software may only issue once the previous operation completed, programmers_guide.md
  // step 1, or `next_op` 0, a reset or the end of the run, after the core read OP_STATUS.done),
  // unless the core read OP_STATUS.err. A reset or the end before done may have cut it short:
  // reported.
  protected function void rram_close_op(string at, bit next_op);
    int unsigned n = rram_sw_words.size() + rram_rewrite_left.size();
    bit          finished = next_op || rram_done_seen;
    bit          failed = rram_err_seen;
    if (n != 0) begin
      bit [31:0] first;
      if (rram_sw_words.size() != 0) void'(rram_sw_words.first(first));
      else void'(rram_rewrite_left.first(first));
      if (finished && !failed) begin
        `uvm_error(`gfn, $sformatf({"EG2_ISS rram_ctrl never programmed %0d of the words the %s ",
                                    "operation %s sent (the first at %08h), at %s"}, n,
                                   rram_op == RramOpRewrite ? "Rewrite" : "Write", rram_op_by,
                                   first, at))
      end else begin
        `uvm_info(`gfn, $sformatf({"EG2_ISS rram_ctrl did not program %0d words of the %s operation ",
                                   "%s sent (the first at %08h): %s %s"}, n,
                                  rram_op == RramOpRewrite ? "Rewrite" : "Write", rram_op_by,
                                  first, at, failed ? "reported an error" : "came first"), UVM_LOW)
      end
    end
    rram_sw_words.delete();
    rram_rewrite_left.delete();
    rram_done_seen = 1'b0;
    rram_err_seen  = 1'b0;
    rram_op     = RramOpNone;
    rram_op_num = 0;
    rram_op_idx = 0;
  endfunction

  // A store of the core to an sram_ctrl's CTRL (sram_ctrl.hjson: RENEW_SCR_KEY bit 0, INIT bit 1),
  // in bus order with its loads and stores (R19, 2026-10-26):
  //   - key renewal: that SRAM's contents become undefined, so the model has no value for any of
  //     its bytes until written or read;
  //   - INIT: the same, except the meta SRAM, which INIT zeroes (REQ_CMS_14): its bytes (the
  //     revocation bitmap) are 0 in the model then, and so is every capability tag (the tags are
  //     meta SRAM words too);
  //   - in CHERIoT mode the tags of main and sec SRAM go with an INIT or key renewal there (REQ_CMS_16:
  //     "an INIT or key renewal of sram_ctrl_main/sec over a tagged granule leaves tag 0"), so
  //     CHERIoT-Sail's tags of that SRAM are cleared (R18: they survived, and the model's own stale
  //     capabilities loaded tagged).
  protected function void sram_ctrl_store(eg2_dside_access_t a);
    for (int m = 0; m < NumMemRegions; m++) begin
      bit is_meta = MemBase[m] == top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR;
      if (MemCtrlRegs[m] == 0 ||
          a.addr != MemCtrlRegs[m] + 32'(sram_ctrl_reg_pkg::SRAM_CTRL_CTRL_OFFSET) ||
          !a.be[0] || a.wdata[1:0] == 2'b00) begin
        continue;
      end
      if (is_meta && a.wdata[1]) begin
        zero_meta(m);
      end else begin
        invalidate(m, a.wdata[0] ? "key renewal" : "INIT");
        if (sail_active && MemBase[m] < RevHeapBase + RevHeapSize && MemBase[m] >= RevHeapBase) begin
          sail_clear_tags(MemBase[m], MemSize[m]);
        end
      end
    end
  endfunction

  protected function void invalidate(int unsigned region, string why);
    int unsigned n = forget_vol(MemBase[region], MemSize[region]);
    `uvm_info(`gfn, $sformatf("EG2_ISS SRAM at %08h: %s by the core, %0d words the model held are undefined now",
                              MemBase[region], why, n), UVM_MEDIUM)
  endfunction

  // The SRAM words in [base, base + size) the model held a value for are undefined now. R19.
  protected function int unsigned forget_vol(bit [31:0] base, bit [31:0] size);
    int unsigned n;
    foreach (vol_known[w]) begin
      if (w >= base && w - base < size) begin
        vol_known.delete(w);
        n++;
      end
    end
    return n;
  endfunction

  // The meta SRAM after INIT (REQ_CMS_14, "INIT zeroes it"): all of it 0 in the model and known, and
  // in CHERIoT mode every capability tag 0 (main and sec SRAM, RRAM). R18, R19, 2026-10-26.
  protected function void zero_meta(int unsigned region);
    for (bit [31:0] off = 0; off < MemSize[region]; off += 4) begin
      for (int b = 0; b < 4; b++) model_write_byte(MemBase[region] + off + b, 8'h00);
      vol_known[MemBase[region] + off] = 4'hf;
    end
    if (!sail_active) eg2_cosim_flush_icache(cosim);
    if (sail_active) begin
      sail_clear_tags(RevHeapBase, RevHeapSize);
      sail_clear_tags(RramHost, top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES);
    end
    `uvm_info(`gfn, $sformatf("EG2_ISS meta SRAM INIT by the core: bitmap and tags 0 in the model%s",
                              sail_active ? " (CHERIoT-Sail's tags cleared)" : ""), UVM_MEDIUM)
  endfunction

  // CHERIoT-Sail's capability tags over [base, base + size) cleared (one per 8 bytes). R18.
  protected function void sail_clear_tags(bit [31:0] base, bit [31:0] size);
    for (bit [31:0] off = 0; off < size; off += 8) cheriot_sail_cosim_clear_tag(base + off);
  endfunction

  // The writes behind the core's back that came before this item's data: from the bench's
  // backdoor (mem_write_event, backdoor_write) and rram_ctrl's programs. A load whose response came
  // before such a write read the old value, so a write reaches the model only before the first
  // item whose data came after it. R6, R7, 2026-10-25.
  // The time reference is the oldest response among the accesses the model has not executed yet:
  // this item's and those still waiting from earlier items (Spike's queue, spike_q; CHERIoT mode's
  // unclaimed ones, sail_q). R19, 2026-10-26: it was this item's own accesses only, or now without
  // any, so a load whose access came one item early (with the item before it) was compared with a
  // backdoor write made after its response.
  protected function void apply_pending_writes(eg2_rvfi_item item);
    realtime t_ref = $realtime;
    bit      any;
    foreach (item.dside[i]) if (item.dside[i].t_resp < t_ref) t_ref = item.dside[i].t_resp;
    foreach (spike_q[i]) if (!sail_active && spike_q[i].a.t_resp < t_ref) t_ref = spike_q[i].a.t_resp;
    foreach (sail_q[i]) if (sail_active && sail_q[i].a.t_resp < t_ref) t_ref = sail_q[i].a.t_resp;
    resolve_rram_programmed(t_ref);
    while (pending_wr.size() > 0 && pending_wr[0].t < t_ref) begin
      mem_wr_t w = pending_wr.pop_front();
      int unsigned r;
      case (w.kind)
        WrByte: begin
          model_write_byte(w.addr, w.data);
          if (mem_kind(w.addr, r) == MemVol) mark_known(w.addr, 4'b0001 << w.addr[1:0]);
          // rram_ctrl's program leaves the capability's tag 0 (REQ_CMS_16). R18, 2026-10-26.
          if (w.untag && sail_active && w.addr[1:0] == 2'b00) begin
            cheriot_sail_cosim_clear_tag({w.addr[31:3], 3'b000});
          end
        end
        // The bench's set_mem and the like: no value in the model until written or read (R19).
        WrUnknown: void'(forget_vol(w.addr, w.size));
        // The bench put the images back (R19).
        WrImage: begin
          int n = eg2_cosim_elf_restore(cosim, w.addr, w.size, int'(sail_active));
          `uvm_info(`gfn, $sformatf("EG2_ISS the bench reloaded [%08h, +%0h): %0d image bytes given again",
                                    w.addr, w.size, n), UVM_LOW)
        end
        // The revocation engine cleared a tag (R18).
        WrUntag: if (sail_active) cheriot_sail_cosim_clear_tag(w.addr);
        default: ;
      endcase
      any = 1'b1;
    end
    // Code may have changed: Spike's decoded instructions are dropped (R11).
    if (any && !sail_active && cosim != null) eg2_cosim_flush_icache(cosim);
  endfunction

  // rram_ctrl's programmed words before `t`, into nv_mem and the write queue, by who documentedly
  // writes the data partition (R7, 2026-10-25; R15, 2026-10-26):
  //   - a Write operation's word: the one its stores to wr_fifo sent (R7: not the PHY's input, so a
  //     controller write path that corrupts the word cannot corrupt the model too); the PHY's
  //     input is compared with it (R15: the controller's write path checked where it acts);
  //   - a Rewrite's word: the controller writes back the corrected word it read
  //     (programmers_guide.md "Issuing a Rewrite Operation"), which is the model's own value of
  //     it; a PHY word that differs is an error, and the model keeps its value (R15: the PHY's
  //     word was adopted, so a rewrite of uncorrected data with fresh ECC passed). Without a
  //     value in the model the PHY's word is taken, counted;
  //   - otp_ctrl's OTP region and lc_ctrl's RMA wipe (pseudo-random data): the PHY's word, counted;
  //   - anything else is an error (R15: taken silently unless the operation still had words).
  // Every programmed word leaves its capability tag 0 (REQ_CMS_16): untag.
  protected function void resolve_rram_programmed(realtime t);
    while (rram_programmed.size() > 0 && rram_programmed[0].t < t) begin
      rram_prog_t p = rram_programmed.pop_front();
      bit [31:0]  data = p.phy_data;
      if (rram_sw_words.exists(p.addr)) begin
        data = rram_sw_words[p.addr];
        rram_sw_words.delete(p.addr);
        rram_words_core++;
        if (p.phy_data != data) begin
          `uvm_error(`gfn, $sformatf({"EG2_ISS rram_ctrl programmed %08h with %08h, the word the ",
                                      "Write operation %s sent is %08h"}, p.addr, p.phy_data,
                                     rram_op_by, data))
        end
      end else if (rram_rewrite_left.exists(p.addr)) begin
        bit [31:0] mine;
        rram_rewrite_left.delete(p.addr);
        if (model_nv_word(p.addr, mine)) begin
          rram_words_rewrite++;
          if (p.phy_data != mine) begin
            `uvm_error(`gfn, $sformatf({"EG2_ISS rram_ctrl's Rewrite programmed %08h with %08h, ",
                                        "the model's (corrected) word is %08h"}, p.addr,
                                       p.phy_data, mine))
          end
          data = mine;
        end else begin
          rram_words_rewrite_unknown++;
        end
      end else if (p.addr >= RramOtpBase) begin
        rram_words_otp++;
        rram_words_other++;
      end else if (p.rma) begin
        rram_words_rma++;
        rram_words_other++;
      end else begin
        rram_words_unexplained++;
        rram_words_other++;
        `uvm_error(`gfn, $sformatf({"EG2_ISS rram_ctrl programmed %08h (%08h), which no Write or ",
                                    "Rewrite operation of the core or the bench's SBA sent, outside ",
                                    "the OTP region and RMA"}, p.addr, p.phy_data))
      end
      for (int b = 0; b < 4; b++) begin
        nv_mem[p.addr + b] = data[8 * b +: 8];
        push_wr(wr_byte(p.t, p.addr + b, data[8 * b +: 8], 1'b1));
      end
    end
  endfunction

  // The model's own value of the RRAM word at `addr` (R15, 2026-10-26): every byte from the bytes
  // written behind the core's back (nv_mem) or the images. 0 if a byte has neither.
  protected function bit model_nv_word(bit [31:0] addr, output bit [31:0] w);
    w = '0;
    for (int b = 0; b < 4; b++) begin
      int img;
      if (nv_mem.exists(addr + b)) begin
        w[8 * b +: 8] = nv_mem[addr + b];
        continue;
      end
      img = eg2_cosim_elf_byte(addr + b);
      if (img < 0) return 1'b0;
      w[8 * b +: 8] = 8'(img);
    end
    return 1'b1;
  endfunction

  // Into pending_wr in time order (a programmed word is resolved after later backdoor writes may
  // have been queued; the queue is applied from its front).
  protected function void push_wr(mem_wr_t w);
    int i = pending_wr.size();
    while (i > 0 && pending_wr[i - 1].t > w.t) i--;
    pending_wr.insert(i, w);
  endfunction

  // After a Spike step: the chip's accesses Spike took up leave the front of the queue; one still
  // there after the step following the one it came with belongs to no instruction (an extra access
  // the chip made). R9, 2026-10-25.
  protected function void check_spike_left(eg2_rvfi_exp_item exp);
    int n = eg2_cosim_num_pending_dside(cosim);
    while (spike_q.size() > n) void'(spike_q.pop_front());
    if (spike_q.size() > 0 && spike_steps - spike_q[0].carrier >= MaxCarry) begin
      eg2_dside_access_t a = spike_q[0].a;
      exp.match  = 1'b0;
      exp.errors = {exp.errors, $sformatf({"the chip's %s at %08h (be %h) is taken up by no ",
                                           "instruction Spike executed; "},
                                          a.store ? "store" : "load", a.addr, a.be)};
      // Reported once; Spike's own queue still holds it and a later access will mismatch it.
      void'(spike_q.pop_front());
    end
  endfunction

  // At a reset, a stop or the end: the accesses the model was told of and has not taken up, which
  // an instruction still in flight may own. R9, 2026-10-25.
  protected function void report_left(string at);
    int unsigned n = sail_active ? sail_q.size() :
                     (cosim != null ? eg2_cosim_num_pending_dside(cosim) : 0);
    if (n == 0 || (cosim == null && !sail_active)) return;
    `uvm_info(`gfn, $sformatf("EG2_ISS at %s: %0d data accesses of the chip not yet taken up by %s (in flight)",
                              at, n, sail_active ? "CHERIoT-Sail" : "Spike"), UVM_LOW)
  endfunction

  // A store to rv_core_ibex.cfg's CHERIOT_LOCK.
  protected function bit is_lock_store(eg2_rvfi_item item);
    return item.mem_wmask != 0 &&
           item.mem_addr == top_earlgrey_pkg::TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR +
                            32'(rv_core_ibex_reg_pkg::RV_CORE_IBEX_CHERIOT_LOCK_OFFSET);
  endfunction

  // The handover at the CHERIoT lock (REQ_SOC_07): Spike's state into a fresh CHERIoT-Sail.
  protected function void handover(eg2_rvfi_item item);
    bit [31:0] spike_pc = eg2_cosim_get_pc(cosim);
    int        copied;
    if (spike_pc != item.pc) begin
      // The switch point itself is checked: Spike's next instruction is where the chip goes on.
      eg2_rvfi_exp_item exp = eg2_rvfi_exp_item::type_id::create("exp");
      exp.order  = item.order;
      exp.pc     = item.pc;
      exp.act    = item;
      exp.match  = 1'b0;
      exp.errors = $sformatf("handover: Spike's next PC is %08h, the chip continues at %08h",
                             spike_pc, item.pc);
      exp_port.write(exp);
    end
    // The whole address space is memory to CHERIoT-Sail (its RAM segment at 0x80000000, its second
    // segment below): devices' values are events, written before each load.
    cheriot_sail_cosim_set_mhpm_counter_num(mhpm_counter_num);
    cheriot_sail_cosim_set_rom(32'h0000_0000, 32'h8000_0000);
    cheriot_sail_cosim_set_ram_size(32'h7fff_0000);
    cheriot_sail_cosim_init(boot_addr_q);
    // The chip's revocation layout (RevHeapBase): CHERIoT-Sail's load barrier reads the bitmap from
    // its own memory, which the core's stores to the window keep in step, as they do the chip's.
    // Review R18, 2026-10-26.
    cheriot_sail_cosim_set_revocation(RevHeapBase, RevHeapSize, RevBitsBase);
    cheriot_sail_cosim_null_gprs();
    // Every memory region the model holds (MemBase; the meta SRAM since 2026-10-25).
    for (int r = 0; r < NumMemRegions; r++) begin
      int n = eg2_cosim_copy_mem_to_cheriot_sail(cosim, MemBase[r], MemSize[r]);
      if (n < 0) `uvm_fatal(`gfn, $sformatf("EG2_ISS handover: cannot copy %08h", MemBase[r]))
      copied += n;
    end
    // CHERIoT mode starts with no revocation bit set and no tag (REQ_CMS_15: the meta SRAM is
    // initialised before CHERIOT_LOCK; RISC-V mode cannot reach the window, cheriot_mem_sys
    // theory_of_operation.md): the bitmap is 0 in the model and known, so the core's loads of it
    // are compared (CHERIoT-Sail's tags are 0 after its init). Review R18, 2026-10-26.
    for (bit [31:0] off = 0; off < RevBitsBytes; off += 4) begin
      for (int b = 0; b < 4; b++) cheriot_sail_cosim_write_mem_byte(RevBitsBase + off + b, 8'h00);
      vol_known[RevBitsBase + off] = 4'hf;
    end
    for (int r = 1; r < 16; r++) cheriot_sail_cosim_set_gpr_int(5'(r), eg2_cosim_get_gpr(cosim, r));
    cheriot_sail_cosim_handover(spike_pc,
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MTVEC)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MEPC)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MSTATUS)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MIE)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MCAUSE)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MTVAL)));
    sail_active    = 1'b1;
    sail_prev_trap = 1'b0;
    sail_prev_skip = 1'b0;
    // The chip's accesses Spike was told of and did not take up belong to the CHERIoT-mode
    // instructions after the lock (an access completes before its instruction retires): they are
    // accounted for there (R9, 2026-10-25).
    while (spike_q.size() > eg2_cosim_num_pending_dside(cosim)) void'(spike_q.pop_front());
    foreach (spike_q[i]) sail_q.push_back('{a: spike_q[i].a, carrier: sail_steps});
    spike_q.delete();
    `uvm_info(`gfn, $sformatf({"EG2_ISS handover to CHERIoT-Sail at order %0d, pc %08h, after %0d ",
                               "RISC-V-mode instructions (%0d bytes of memory, %0d data accesses ",
                               "carried over)"},
                              item.order, item.pc, stepped, copied, sail_q.size()), UVM_LOW)
  endfunction

  // One CHERIoT-mode instruction on CHERIoT-Sail.
  protected function void sail_step(eg2_rvfi_item item);
    eg2_rvfi_exp_item exp = eg2_rvfi_exp_item::type_id::create("exp");
    bit [31:0] rd_wdata = item.rd_wdata;
    bit        cheri_we = item.rd_addr != 0 && !item.rf_wr_suppress && !item.trap;
    // R9, R12, 2026-10-25.
    eg2_dside_access_t claimed[$];
    bit        bus_err, err_store;
    bit [31:0] err_tval;
    exp.order = item.order;
    exp.pc    = item.pc;
    exp.act   = item;
    exp.match = 1'b1;
    cheriot_sail_cosim_clear_errors();

    // Inputs that depend on timing or the bench, before the step.
    cheriot_sail_cosim_set_mcycle(item.mcycle);
    for (int i = 0; i < mhpm_counter_num && i < 10; i++) begin
      cheriot_sail_cosim_set_mhpmcounter(3 + i, {item.mhpmcountersh[i], item.mhpmcounters[i]});
    end
    cheriot_sail_cosim_set_ic_scr_key_valid(item.ic_scr_key_valid);
    // rv_core_ibex's MCOUNTEREN_WRITABLE, which software may clear: the model's default (writable)
    // holds only until then. As the core bench (2026-10-27, from the mip review).
    cheriot_sail_cosim_set_mcounteren_writable(item.mcounteren_writable);
    // The chip's pending interrupts, an input like the counters: a csrr mip in the model reads
    // them (2026-10-27: chip_sw_eg2_connectivity_cheriot read mip 0x880 on the chip and 0 in the
    // model, which saw pending bits only when the chip took an interrupt).
    cheriot_sail_cosim_set_mip(item.pre_mip);
    // After a trap, on the handler's first instruction (both sides have committed it, the model is
    // not stepped yet): the chip's mcause and mtval against the model's, as the Ibex UVM bench's
    // check_cheriot_trap_cause. Not after an NMI or debug entry, which the model has no counterpart
    // for. R13, 2026-10-25.
    if (sail_prev_trap && !sail_prev_skip && item.intr) begin
      bit [31:0] m = cheriot_sail_cosim_get_mcause();
      bit [31:0] t = cheriot_sail_cosim_get_mtval();
      if (!cheriot_sail_cosim_get_trap()) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, "the chip trapped before this handler, CHERIoT-Sail did not; "};
      end else if (item.csr_mcause != m || item.csr_mtval != t) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, $sformatf({"trap cause: chip mcause %08h mtval %08h, CHERIoT-Sail ",
                                             "mcause %08h mtval %08h; "},
                                            item.csr_mcause, item.csr_mtval, m, t)};
      end
    end
    // The core entered an interrupt handler: the model takes the interrupt from its own state.
    if (item.intr && !sail_prev_trap && !item.nmi && !item.nmi_int && !item.debug_req) begin
      void'(cheriot_sail_cosim_take_interrupt(item.pre_mip));
    end
    // The chip's data accesses this instruction's RVFI record accounts for (R9); a load's bytes go
    // into the model only where they are events (feed_load, R6: before 2026-10-25 every integer
    // load's value was written in, so the model compared each load with the chip's own bytes).
    // Capability loads by the same rule (R18, 2026-10-26: they never were, so after an SRAM INIT or
    // key renewal the model loaded its own stale capability): a capability's bytes the model has no
    // value for are the chip's; its tag is always the model's (0 after INIT or key renewal of main
    // and sec SRAM, REQ_CMS_16; sram_ctrl_store), and the revocation is CHERIoT-Sail's own.
    // An sram_ctrl CTRL store in bus order with the others (R19, sram_ctrl_store).
    claim_accesses(item, claimed);
    foreach (claimed[i]) begin
      if (!claimed[i].store && !claimed[i].err) begin
        feed_load(claimed[i].addr, claimed[i].be, claimed[i].rdata);
      end
      if (claimed[i].store && !claimed[i].err) sram_ctrl_store(claimed[i]);
      if (claimed[i].store && !claimed[i].err) store_defines(claimed[i].addr, claimed[i].be);
      // A trapping misaligned store wrote the half that got no error (the other half's bus error or
      // PMP stopped it), which CHERIoT-Sail's access fault does not: those bytes go into the model
      // as an event, as Spike's misaligned_store_second_half / misaligned_pmp_fixup apply them.
      // Not checked: Spike checks them against its own rs2, CHERIoT-Sail has no register read for
      // that. Reported. R14, 2026-10-26.
      if (item.trap && claimed[i].store && !claimed[i].err) begin
        for (int b = 0; b < 4; b++) begin
          if (claimed[i].be[b]) model_write_byte(claimed[i].addr + b, claimed[i].wdata[8 * b +: 8]);
        end
        `uvm_info(`gfn, $sformatf({"EG2_ISS the trapping store at order %0d wrote [%08h/%h]=%08h ",
                                   "(the half without a fault): given to CHERIoT-Sail"},
                                  item.order, claimed[i].addr, claimed[i].be, claimed[i].wdata),
                  UVM_LOW)
      end
      if (claimed[i].err && !bus_err) begin
        bus_err   = 1'b1;
        err_store = claimed[i].store;
        // Ibex's mtval for a data bus error: the access's address, the second word's for an error
        // on the second half of a misaligned access (ibex_controller.sv lsu_addr_last).
        err_tval  = claimed[i].misaligned_second ? claimed[i].addr : item.mem_addr;
      end
    end
    if ((corrupt_rd && stepped >= 100 || corrupt_cheriot_rd && sail_steps >= 100) &&
        item.rd_addr != 0 && !item.trap) begin
      rd_wdata           = ~rd_wdata;
      corrupt_rd         = 1'b0;
      corrupt_cheriot_rd = 1'b0;
      `uvm_info(`gfn, $sformatf("+sb_corrupt: x%0d of order %0d (CHERIoT mode) corrupted",
                                item.rd_addr, item.order), UVM_LOW)
`ifdef EG2_PERIPH_SB
      if (eg2_inject_registry::active()) begin
        void'(eg2_inject_registry::announce("sb_corrupt insn.cheriot_rd", "insn",
            $sformatf("x%0d of order %0d (CHERIoT mode) inverted", item.rd_addr, item.order),
            $realtime, $realtime));
      end
`endif
    end
    // A bus error the chip took, on a fetch the bench armed or on a data access: the model takes the
    // access fault instead of executing the instruction (R12, 2026-10-25; it ran on before, and
    // diverged from the chip at the fault).
    if (item.iside_err && !iside_armed.exists(item.iside_err_addr)) report_unarmed_iside(item);
    if (item.iside_err && iside_armed.exists(item.iside_err_addr)) begin
      bit [31:0] tval = item.iside_err_addr == {item.pc[31:2], 2'b00} ? item.pc : item.pc + 2;
      iside_armed.delete(item.iside_err_addr);
      exp.match = cheriot_sail_cosim_step_bus_error(item.pc, 0, tval) == 0 && exp.match;
      if (!item.trap) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, "the chip took a fetch error and did not trap; "};
      end
      `uvm_info(`gfn, $sformatf("EG2_ISS fetch error at %08h given to CHERIoT-Sail (order %0d)",
                                item.iside_err_addr, item.order), UVM_LOW)
    end else if (bus_err) begin
      sail_bus_errors++;  // R14, 2026-10-26
      exp.match = cheriot_sail_cosim_step_bus_error(item.pc, err_store ? 2 : 1, err_tval) == 0 &&
                  exp.match;
      if (!item.trap) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, "the chip's data access got a bus error and it did not trap; "};
      end
    end else begin
      exp.match = cheriot_sail_cosim_step(item.insn, item.pc, cheri_we, item.rd_addr,
                                          item.rd_wcap[32], rd_wdata, item.trap) == 0 && exp.match;
    end
    for (int i = 0; i < cheriot_sail_cosim_get_num_errors(); i++) begin
      exp.errors = {exp.errors, cheriot_sail_cosim_get_error(i), "; "};
    end
    // Every capability result in full, address and metadata, as ibex_sail_rvfi_cmp compares it for
    // TestRIG: cheriot_sail_cosim_step compares only a capability write's register and tag.
    if (cheri_we && cheriot_sail_cosim_get_cd_addr() != 0 &&
        cheriot_sail_cosim_get_cd_addr() == item.rd_addr &&
        {item.rd_wcap[31:0], rd_wdata} != cheriot_sail_cosim_get_cd_wdata()) begin
      exp.match  = 1'b0;
      exp.errors = {exp.errors, $sformatf("cap rd data (c%0d): chip 0x%08x_%08x, CHERIoT-Sail 0x%016x; ",
                                          item.rd_addr, item.rd_wcap[31:0], rd_wdata,
                                          cheriot_sail_cosim_get_cd_wdata())};
    end
    // Every integer store's address and data, from the model's RVFI packet.
    if (item.mem_wmask != 0 && !item.mem_is_cap && !item.trap && !bus_err) begin
      bit [31:0] mask = '0;
      for (int b = 0; b < 4; b++) if (item.mem_wmask[b]) mask[8 * b +: 8] = 8'hff;
      if (!cheriot_sail_cosim_get_mem_present() ||
          cheriot_sail_cosim_get_mem_addr() != item.mem_addr ||
          (cheriot_sail_cosim_get_mem_wdata() & mask) != (item.mem_wdata & mask)) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, $sformatf("store: chip [%08h]=%08h, CHERIoT-Sail [%08h]=%08h; ",
                                            item.mem_addr, item.mem_wdata & mask,
                                            cheriot_sail_cosim_get_mem_addr(),
                                            32'(cheriot_sail_cosim_get_mem_wdata()) & mask)};
      end
    end
    // What the chip put on the bus (R18, 2026-10-26: a claimed store matched by kind and word only,
    // and the check above reads RVFI, which takes the data before the load/store unit aligns it):
    // every byte of the claimed bus stores against the bytes CHERIoT-Sail stored.
    if (!item.trap && !bus_err) check_bus_stores(exp, item, claimed);
`ifdef EG2_SYSMODEL
    model_check(exp, eg2_sysmodel::access(item));
`endif
    sail_prev_trap = item.trap;
    sail_prev_skip = item.nmi || item.nmi_int || item.debug_req;
    sail_steps++;
    // An access still unclaimed a step after the one it came with: no instruction accounts for it
    // (R9, 2026-10-25). CHERIoT-Sail has no queue of the chip's accesses: this check stands in.
    while (sail_q.size() > 0 && sail_steps - sail_q[0].carrier >= MaxCarry) begin
      eg2_dside_access_t a = sail_q.pop_front().a;
      exp.match  = 1'b0;
      exp.errors = {exp.errors, $sformatf({"the chip's %s at %08h (be %h) is accounted for by no ",
                                           "instruction's RVFI record; "},
                                          a.store ? "store" : "load", a.addr, a.be)};
    end
    stepped++;
    exp_port.write(exp);
  endfunction

  // The bytes the chip's bus stores of this instruction wrote (each claimed store's byte enables and
  // data) against those CHERIoT-Sail's step stored (its RVFI memory record: mem_addr, a mask of up
  // to 8 bytes from it, the data, byte k at mem_addr + k; a capability is its 8 memory bytes): a
  // byte only one of them wrote, or a byte they wrote differently, fails the instruction. R18,
  // 2026-10-26.
  protected function void check_bus_stores(eg2_rvfi_exp_item exp, eg2_rvfi_item item,
                                           const ref eg2_dside_access_t claimed[$]);
    bit [7:0]  model_b[bit [31:0]];
    bit [7:0]  chip_b[bit [31:0]];
    bit [31:0] ma = cheriot_sail_cosim_get_mem_addr();
    bit [7:0]  mm = cheriot_sail_cosim_get_mem_present() ? 8'(cheriot_sail_cosim_get_mem_wmask()) :
                                                           8'h00;
    bit [63:0] md = cheriot_sail_cosim_get_mem_wdata();
    string     diffs;
    int        n;
    for (int k = 0; k < 8; k++) if (mm[k]) model_b[ma + k] = md[8 * k +: 8];
    foreach (claimed[i]) begin
      if (!claimed[i].store || claimed[i].err) continue;
      for (int b = 0; b < 4; b++) begin
        if (claimed[i].be[b]) chip_b[claimed[i].addr + b] = claimed[i].wdata[8 * b +: 8];
      end
    end
    foreach (chip_b[a]) begin
      if (model_b.exists(a) && model_b[a] == chip_b[a]) continue;
      if (n < 4) begin
        diffs = {diffs, $sformatf(" [%08h] chip %02h, model %s", a, chip_b[a],
                                  model_b.exists(a) ? $sformatf("%02h", model_b[a]) : "none")};
      end
      n++;
    end
    foreach (model_b[a]) begin
      if (chip_b.exists(a)) continue;
      if (n < 4) diffs = {diffs, $sformatf(" [%08h] chip none, model %02h", a, model_b[a])};
      n++;
    end
    if (n == 0) return;
    exp.match  = 1'b0;
    exp.errors = {exp.errors, $sformatf("bus store bytes differ from CHERIoT-Sail's (%0d):%s; ", n,
                                        diffs)};
  endfunction

  // The chip's data accesses (sail_q) this item's RVFI load or store accounts for: those to the
  // words it covers (4 bytes or less from mem_addr, 8 for a capability), of its kind, oldest first.
  // R9, 2026-10-25.
  // A trapping load or store has RVFI masks of 0 when its fault was a load/store error (a bus error
  // or PMP: Ibex with the writeback stage zeroes them, ibex_core.sv rvfi_trap_wb) but keeps
  // mem_addr: its kind and size come from the encoding then, so its bus error is claimed and given
  // to CHERIoT-Sail and the half of a misaligned access that went out is accounted for. Review
  // R14, 2026-10-26 (it claimed nothing: no bus error was given, the model executed the access and
  // R9 failed the unclaimed access).
  protected function void claim_accesses(eg2_rvfi_item item, ref eg2_dside_access_t claimed[$]);
    bit [3:0]    mask = item.mem_rmask | item.mem_wmask;
    int unsigned size;
    bit [31:0]   lo, hi;
    bit          store = item.mem_wmask != 0;
    if (mask == 0 && item.trap) mask = eg2_rvfi_item::insn_access_mask(item.insn, store);
    if (mask == 0) return;
    size = item.mem_is_cap ? 8 : (mask[3] ? 4 : mask[2] ? 3 : mask[1] ? 2 : 1);
    lo   = {item.mem_addr[31:2], 2'b00};
    hi   = {(item.mem_addr + size - 1) >> 2, 2'b00};
    for (int i = 0; i < sail_q.size(); i++) begin
      if (sail_q[i].a.store == store && sail_q[i].a.addr >= lo && sail_q[i].a.addr <= hi) begin
        claimed.push_back(sail_q[i].a);
        sail_q.delete(i);
        i--;
        if (claimed.size() > (hi - lo) / 4) break;
      end
    end
  endfunction

`ifdef EG2_SYSMODEL
  // The SystemC model's findings on the instruction's data access (eg2_sysmodel.sv) fail it too.
  protected function void model_check(eg2_rvfi_exp_item exp, string errors);
    if (errors == "") return;
    exp.match  = 1'b0;
    exp.errors = {exp.errors, errors};
  endfunction
`endif

  // At the end (R8, R9, R11; 2026-10-25): a stop no chip reset ended leaves the rest of the run
  // unchecked, which fails it; the accesses still in flight and the fetch errors the bench armed
  // that no instruction took are reported.
  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    if (stop_open) begin
      `uvm_error(`gfn, $sformatf({"EG2_ISS stopped at %0t (%s) and no chip reset came: %0d ",
                                  "instructions after it were not compared"},
                                 stop_t, stop_why, dropped))
    end
    if (stop_armed) begin
      `uvm_info(`gfn, $sformatf("EG2_ISS a stop was armed (%s) and no data-access fault came",
                                stop_why), UVM_LOW)
    end
    if (!stopped) report_left("the end of the run");
    // The RRAM operation still open: its words the controller did not program are an error once the
    // core read OP_STATUS.done, else reported, as the end may have cut it short (R15, 2026-10-26).
    if (enabled && cosim != null) begin
      resolve_rram_programmed($realtime);
      rram_close_op("the end of the run", 1'b0);
    end
    foreach (iside_armed[a]) begin
      `uvm_info(`gfn, $sformatf("EG2_ISS the bench's fetch error at %08h was taken by no instruction",
                                a), UVM_LOW)
    end
  endfunction

  function void report_phase(uvm_phase phase);
    super.report_phase(phase);
    if (!enabled) return;
    `uvm_info(`gfn, $sformatf({"EG2_ISS %0d instructions compared, %0d not compared while stopped; ",
                               "%0d memory bytes read without a value in the model (the chip's ",
                               "taken); rram_ctrl programmed %0d words a Write operation sent, ",
                               "%0d by Rewrite (%0d compared with the model's word, %0d without ",
                               "one, taken from the PHY), %0d others from the PHY's input (%0d ",
                               "OTP region, %0d RMA wipe, %0d by no documented writer); %0d ",
                               "data-access bus errors on the instruction that took them, %0d ",
                               "of them given to CHERIoT-Sail; %0d revocation-engine tag clears"},
                              stepped, dropped_total, unknown_reads, rram_words_core,
                              rram_words_rewrite + rram_words_rewrite_unknown, rram_words_rewrite,
                              rram_words_rewrite_unknown, rram_words_other, rram_words_otp,
                              rram_words_rma, rram_words_unexplained, mem_faults, sail_bus_errors,
                              tbre_clears), UVM_LOW)
  endfunction

  function void final_phase(uvm_phase phase);
    super.final_phase(phase);
    if (cosim != null) spike_cosim_release(cosim);
    cosim = null;
    if (sail_active) cheriot_sail_cosim_cleanup();
`ifdef EG2_SYSMODEL
    eg2_sysmodel::close();
`endif
  endfunction
`else
  protected function void handle(eg2_rvfi_item item);
  endfunction

  protected function void report_left(string at);
  endfunction

  protected function void rram_reg_store(bit [31:0] addr, bit [31:0] wdata, realtime t, string by);
  endfunction
`endif

  function int unsigned num_stepped();
    return stepped;
  endfunction

  // A write into memory the model holds that no RVFI store shows: a bench backdoor write (the ROM,
  // a software symbol, an SRAM word) or a device's (rram_ctrl's controller writes,
  // watch_rram_writes). Spike fetches and (since R6) loads from its own memory, so it takes the
  // same bytes, from the first instruction whose data came after the write (apply_pending_writes;
  // chip_sw_eg2_illegal_sweep_vseq places code that way). CHERIoT-Sail takes them too.
  // ROM and RRAM bytes are also kept and written again whenever Spike restarts (restart): the chip
  // keeps them over a reset, a fresh Spike has only the ELF images (chip_sw_rram_uncorr_fetch's
  // second boot runs the words its first boot wrote through the controller; 2026-10-25). So a
  // write before Spike has started still reaches Spike after the next reset. An SRAM write before
  // the reset is lost with the SRAM's contents.
  function void mem_write_event(bit [31:0] addr, bit [31:0] data);
    for (int b = 0; b < 4; b++) mem_write_byte(addr + b, data[8 * b +: 8]);
  endfunction

  protected function void mem_write_byte(bit [31:0] addr, bit [7:0] data);
    bit is_mem = 1'b0;
    for (int r = 0; r < NumMemRegions; r++) begin
      if (addr >= MemBase[r] && addr - MemBase[r] < MemSize[r]) begin
        is_mem = 1'b1;
        if (MemKind[r] == MemNv) nv_mem[addr] = data;
      end
    end
    // A device's register is not memory: its loads are events anyway.
    // Now is never earlier than a queued write: the queue stays in time order.
    if (is_mem) pending_wr.push_back(wr_byte($realtime, addr, data));
  endfunction

  // One byte written behind the core's back, for pending_wr. 2026-10-26.
  protected function mem_wr_t wr_byte(realtime t, bit [31:0] addr, bit [7:0] data,
                                      bit untag = 1'b0);
    return '{t: t, kind: WrByte, addr: addr, size: 1, data: data, untag: untag};
  endfunction

  // A sequence's backdoor write of `data` at `addr` (chip_sw_base_vseq sw_symbol_backdoor_access,
  // the SRAM backdoor writes): given to the model of this run, if any. R6, 2026-10-25.
  static function void backdoor_write(bit [31:0] addr, bit [7:0] data[]);
    if (inst == null) return;
    foreach (data[i]) inst.mem_write_byte(addr + i, data[i]);
  endfunction

  static function void backdoor_write32(bit [31:0] addr, bit [31:0] data);
    if (inst == null) return;
    inst.mem_write_event(addr, data);
    // A write to rram_ctrl's registers over SBA (the EG2 vseqs' sba_wr32 helpers call this for
    // every write that succeeded) sets up a controller operation as the core's stores do, so the
    // words it programs are the bench's, not unexplained (R15, 2026-10-26).
    if (addr >= RramCore && addr - RramCore < top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_SIZE_BYTES) begin
      inst.rram_reg_store(addr, data, $realtime, "the bench's SBA");
    end
  endfunction

  // The bench made memory in [base, base + size) undefined behind the core's back
  // (mem_bkdr_util set_mem, which writes all-ones words without valid scrambling and integrity:
  // chip_sw_sram_ctrl_scrambled_access_vseq, chip_sw_eg2_sram_scrambled_access_cheriot_vseq
  // cpu_init): the SRAM bytes the model held have no value from then on, as after INIT. Review
  // R19, 2026-10-26 (no hook: the model kept the bytes it had).
  static function void backdoor_undefined(bit [31:0] base, bit [31:0] size);
    if (inst == null) return;
    inst.pending_wr.push_back('{t: $realtime, kind: WrUnknown, addr: base, size: size, data: 0,
                                untag: 0});
  endfunction

  // The bench loaded the RRAM's data partition from the test image again after an RMA wiped it
  // (chip_sw_eg2_sec_rma_key_change_vseq, chip_sw_lc_walkthrough_vseq). Every RRAM byte the model
  // kept from writes behind the core's back is gone with the wipe: the chip holds the image again
  // where it has one, so the model takes the image's bytes back from now on, and has no value
  // elsewhere (the wipe's pseudo-random data: a first read takes the chip's). Review R19,
  // 2026-10-26: the kept bytes were written into Spike again at the next restart, over the image.
  static function void rram_image_reloaded();
    int unsigned n;
    if (inst == null) return;
    foreach (inst.nv_mem[a]) begin
      if (a >= RramHost && a - RramHost < top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES) begin
        inst.nv_mem.delete(a);
        n++;
      end
    end
    inst.pending_wr.push_back('{t: $realtime, kind: WrImage, addr: RramHost,
                                size: top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES,
                                data: 0, untag: 0});
    // Through inst: `uvm_info calls uvm_report_enabled, which a static function cannot reach.
    inst.uvm_report_info("eg2_iss_wrapper",
                         $sformatf({"EG2_ISS the bench reloaded the RRAM image: %0d RRAM bytes ",
                                    "kept from writes behind the core's back forgotten"}, n),
                         UVM_LOW, `__FILE__, `__LINE__);
  endfunction

  // rram_ctrl's controller writes into the RRAM's data partition (chip_if rram_wr_*): the addresses
  // the PHY's write path accepted, programmed once the array reports the write done (busy_o with
  // done_i), not when accepted: until then the chip still reads the old words. Without this Spike
  // executed stale RRAM in every test whose software writes code through the controller and runs it
  // (chip_sw_rram_write_then_fetch, chip_sw_rram_host_exec, chip_sw_rram_uncorr_fetch; review
  // 2026-10-25). The words are the ones the core's stores sent (resolve_rram_programmed, R7);
  // the PHY's input only for a word no core store sent. Words accepted and never programmed (a
  // reset, or rram_ctrl disabling itself, which it does only between writes) are dropped (R10:
  // they were flushed into the model at the next busy fall, after a reset or a disable, and given
  // again after every later reset).
  protected task watch_rram_writes();
    forever begin
      @(posedge cfg.chip_vif.rram_wr_clk iff (cfg.chip_vif.rram_wr_word === 1'b1 ||
                                              cfg.chip_vif.rram_wr_done === 1'b1 ||
                                              (rram_waiting.size() != 0 &&
                                               (cfg.chip_vif.rram_wr_rst_n !== 1'b1 ||
                                                cfg.chip_vif.rram_wr_disabled === 1'b1))));
      if (cfg.chip_vif.rram_wr_rst_n !== 1'b1 ||
          (cfg.chip_vif.rram_wr_disabled === 1'b1 && cfg.chip_vif.rram_wr_busy !== 1'b1)) begin
        `uvm_info(`gfn, $sformatf("EG2_ISS rram_ctrl %s: %0d words its PHY accepted are never programmed",
                                  cfg.chip_vif.rram_wr_rst_n !== 1'b1 ? "reset" : "disabled",
                                  rram_waiting.size()), UVM_LOW)
        rram_waiting.delete();
        rram_waiting_rma.delete();
        continue;
      end
      if (cfg.chip_vif.rram_wr_word === 1'b1) begin
        rram_waiting[RramHost + cfg.chip_vif.rram_wr_addr] = cfg.chip_vif.rram_wr_data;
        // R15, 2026-10-26: accepted during lc_ctrl's RMA request, the lcmgr's wipe.
        rram_waiting_rma[RramHost + cfg.chip_vif.rram_wr_addr] = cfg.chip_vif.rram_rma_req === 1'b1;
      end
      if (cfg.chip_vif.rram_wr_done === 1'b1) begin
        foreach (rram_waiting[a]) begin
          rram_programmed.push_back('{t: $realtime, addr: a, phy_data: rram_waiting[a],
                                      rma: rram_waiting_rma[a]});
        end
        rram_waiting.delete();
        rram_waiting_rma.delete();
      end
    end
  endtask

  // The revocation engine's tag clears (chip_if cms_tbre_*): a clear the engine presented takes
  // effect when cheriot_mem_sys's RMW filter accepts it, unless a core write to the capability
  // marked it stale meanwhile (then the RMW only reads). Each one clears CHERIoT-Sail's tag of that
  // capability, in time order with the other writes behind the core's back (WrUntag), as the CHERIoT
  // RTOS bench does (soc_iss_wrapper sail_tag_clears): another bus master's write, an event. Review
  // R18, 2026-10-26 (no clear reached the model: a capability the engine untagged and whose
  // revocation bit software cleared afterwards loaded tagged in the model).
  protected task watch_tbre_clears();
    bit        pending;
    bit [31:0] addr;
    forever begin
      @(posedge cfg.chip_vif.cms_clk iff (cfg.chip_vif.cms_tbre_clear_req === 1'b1 ||
                                         cfg.chip_vif.cms_tbre_clear_rmw === 1'b1));
      if (cfg.chip_vif.cms_tbre_clear_req === 1'b1) begin
        pending = 1'b1;
        addr    = {cfg.chip_vif.cms_tbre_clear_addr[31:3], 3'b000};
      end
      if (cfg.chip_vif.cms_tbre_clear_rmw === 1'b1 && pending) begin
        pending = 1'b0;
        if (cfg.chip_vif.cms_tbre_clear_stale !== 1'b1) begin
          tbre_clears++;
          pending_wr.push_back('{t: $realtime, kind: WrUntag, addr: addr, size: 8, data: 0,
                                 untag: 1});
        end
      end
    end
  endtask

  // The bench makes a fetch of the word at `addr` fail (an integrity fault it injects on the
  // instruction path, or an uncorrectable ECC error in the RRAM row). The model is told of it only
  // for the instruction the chip took a fetch error on there (eg2_rvfi_monitor ties it to the RVFI
  // order, as the Ibex UVM bench does; R11, 2026-10-25): Spike's fetch fails (set_iside_error), or
  // CHERIoT-Sail takes an instruction access fault (cheriot_sail_cosim_step_bus_error, R12). A
  // corrupted prefetch the chip discards is never given to the model. Returns 1 when a model runs
  // and will take it, so the fault need not be announced to the instruction check.
  function bit iside_error_event(bit [31:0] addr);
    iside_armed[{addr[31:2], 2'b00}] = 1'b1;
`ifdef EG2_COSIM
    return enabled && cosim != null;
`else
    return 1'b0;
`endif
  endfunction

  protected function void report_unarmed_iside(eg2_rvfi_item item);
    if (iside_unarmed_seen.exists(item.iside_err_addr)) return;
    iside_unarmed_seen[item.iside_err_addr] = 1'b1;
    `uvm_info(`gfn, $sformatf({"EG2_ISS the chip took a fetch error at %08h (order %0d, pc %08h) ",
                               "the bench did not arm: the model is not told"},
                              item.iside_err_addr, item.order, item.pc), UVM_LOW)
  endfunction

  // The bench makes every later fetch fail until the next chip reset, which no per-fetch event
  // can tell the model: chip_sw_rram_uncorr_fetch's local escalation disables rram_ctrl ("host and
  // controller requests are blocked") while the software runs from RRAM, so the core traps on
  // every fetch until the vseq resets the chip. The instruction check stops and starts again with
  // Spike at the reset (restart). Every item meanwhile is counted, and a stop no reset ends fails
  // the run (check_phase). R8, 2026-10-25.
  //   stop_after_fault: armed now, in effect after the next instruction whose data access faults
  //     (that instruction and its fault are still compared);
  //   stop_until_reset: in effect now.
  function void stop_after_fault(string why);
    stop_armed = 1'b1;
    stop_why   = why;
    `uvm_info(`gfn, $sformatf("EG2_ISS will stop after the next data-access fault: %s", why), UVM_LOW)
  endfunction

  function void stop_until_reset(string why);
    stop_why = why;
    do_stop();
  endfunction

  protected function void check_stop_trigger(eg2_rvfi_item item);
    if (item.trap && item.mem_error) mem_faults++;  // R14, 2026-10-26
    if (stop_armed && item.trap && item.mem_error) do_stop();
  endfunction

  protected function void do_stop();
    stop_armed = 1'b0;
    if (stopped || !enabled) return;
    report_left("the stop");
    `uvm_info(`gfn, $sformatf("EG2_ISS stopped until the next reset: %s (after %0d instructions)",
                              stop_why, stepped), UVM_LOW)
    stopped   = 1'b1;
    stop_open = 1'b1;
    stop_t    = $realtime;
    dropped   = 0;
  endfunction
endclass
