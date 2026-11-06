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
//       value (counted, reported at the end); every later one is compared. The retention SRAM
//       keeps what the model knew of it over a chip reset unless the reset was a power-on reset or
//       a global escalation came before it (dv_todo S11, 3 Nov; retention_sram.h "retain
//       information ... across a device reset", EG2 REQ_SRAM_02, sram_ctrl interfaces.md "Global
//       and Local Escalation": the keys are discarded). Before, it was forgotten at every reset,
//       so a chip that lost its contents passed. sram_ctrl_ret's local escalation is not modelled.
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
// - Interrupts and NMI are taken where RVFI shows them taken. At debug-mode entry the model stops
//   until the next chip reset, as the Ibex UVM bench stops a model without debug mode: neither
//   model can follow the core into rv_dm's debug ROM (debug_stopped, 2026-11-02).
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
// the system model on (eg2_sysmodel.sv; unless +eg2_sysmodel=0) each data access also goes to it
// as well, and it predicts the loads it can; its findings fail the instruction.
//
// Plusargs: +sb_corrupt=insn.rd (corrupts one register result before it is checked, from the 100th
// instruction), +sb_corrupt=insn.cheriot_rd (the same from the 100th CHERIoT-mode instruction, so
// CHERIoT-Sail's check is the one shown to fail), +sb_corrupt=insn.mcause (the chip's mcause of the
// first trap handler from the 100th instruction on, plain mode, so Spike's trap-cause check is the
// one shown to fail); the run must fail. +eg2_iss_ignore_debug_session=1
// (the sequences' debug sessions are not taken: a debug test must fail at its first debug-mode
// entry). +eg2_spike_log=1 (Spike's commit log, spike.model.log).

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
  // dv_todo S7: a data bus error the model reaches itself (the errored bytes armed for the step,
  // its own capability checks first, mtval as Ibex documents it); S5: each instruction word from
  // the model's own memory. Also only here, for the same reason.
  import "DPI-C" function int cheriot_sail_cosim_step_data_error(bit [31:0] insn, bit [31:0] pc,
                                                                  bit [31:0] err_addr,
                                                                  int err_bytes, bit [31:0] tval);
  import "DPI-C" function void cheriot_sail_cosim_set_fetch_check(int on);
  // dv_todo S10: whether the model revokes the capability at an address (the load barrier's rule).
  import "DPI-C" function int cheriot_sail_cosim_tbre_revokes(int unsigned addr);
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
  protected bit          corrupt_mcause;
  // Spike's trap-cause check (dv_todo S12): the previous Spike step was a synchronous trap, and
  // whether it came with an NMI or debug request (no counterpart in the check).
  protected bit          spike_prev_trap;
  // The internal-NMI rule (dv_todo S11, Ibex load_store_unit.rst "Bus Integrity Checking",
  // exception_interrupts.rst "Internal Interrupts"): the words whose data response had bad
  // integrity bits and whose NMI the chip has not taken yet, the instructions retired since the
  // first of them, whether the previous item carried the chip's NMI (one NMI shows on more than one
  // item), and whether an NMI handler runs (no other NMI until its mret).
  protected bit [31:0]   intg_words[$];
  protected int unsigned intg_wait;
  protected bit          intg_prev_nmi;
  protected bit          intg_nmi_active;
  // mcounteren as the model knows it (dv_todo S11, 3 Nov): Ibex cs_registers.rst "Machine
  // Counter-Enable Register" (reset 0; CY, IR and the HPM bits up to MHPMCounterNum + 2 writable,
  // TM 0; writes only while mcounteren_writable_i is on) and rv_core_ibex MCOUNTEREN_WRITABLE
  // (MuBi4, reset True; locked by MCOUNTEREN_WRITABLE_REGWEN, rw0c) from the core's stores.
  // Before, the chip's value was pushed into Spike, so a chip that ignored a write, or took one
  // while locked, passed.
  protected bit [31:0]   mcen_model;
  protected bit [31:0]   mcen_seen;  // the value the current instruction sees (before its write)
  protected bit [3:0]    mcen_writable;
  protected bit          mcen_regwen;
  protected bit          mcen_reported;
  protected bit          spike_prev_skip;
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
  // The retention SRAM's bytes the model knows (from the core's stores, first reads and the bench's
  // backdoor writes), given to the new Spike after a reset that keeps them.
  protected bit [7:0]    ret_mem[bit [31:0]];
  protected bit          ret_escalated;
  localparam bit [31:0]  RetBase = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR;
  localparam bit [31:0]  RetSize = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES;
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
  // Memory bytes the bench uses as a mailbox (backdoor_mailbox): read like a device, the chip's
  // value every time, because a backdoor write can cross a chip read already in flight.
  protected bit mailbox[bit [31:0]];

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

  // Debug mode (2026-11-02). Neither model can follow the core into debug mode on this chip:
  //  - Spike enters debug mode at DEBUG_ROM_ENTRY, 0x8000_0000, and takes debug-mode exceptions at
  //    DEBUG_ROM_TVEC, 0x8000_0008: constants of the Spike build (riscv/debug_rom_defines.h), where
  //    the Ibex UVM bench's riscv-dv programs put their debug code (its compile_tb.py:
  //    DEBUG_MODE_HALT_ADDR 8000_0000). The core here goes to its DmHaltAddr (Ibex
  //    doc/03_reference/debug.rst), rv_dm's debug ROM at the debug memory's base + 0x800
  //    (top_earlgrey_pkg RV_DM_MEM, 0x0001_0000; pulp_riscv_dbg doc/debug-system.md "Debug
  //    Memory"). Spike fetched nothing at 0x8000_0000, trapped to 0x8000_0008 and failed every
  //    debug test at the first debug-ROM instruction ("Synchronous trap was expected at ISS PC:
  //    80000008 but the DUT didn't report one at PC 10800"). Besides, the debug ROM's flags, the
  //    abstract commands and the program buffer are rv_dm's, which the model does not have.
  //  - CHERIoT-Sail has no debug mode at all (ibex_platform_csrs.c: "only in debug mode, which the
  //    model does not have"); it ran on in the interrupted program ("cheriot-sail model PC
  //    0x3000082c differs from DUT PC 0x00010800").
  // So, as the Ibex UVM bench does for a model without debug mode (ibex_cosim_scoreboard.sv
  // riscv_sail_unmodelled(): a debug request, an instruction in the debug module's window, dret;
  // with the real debug module its core_ibex_dm_test runs with the co-simulation off), the model
  // stops at debug-mode entry and starts again with Spike at the next chip reset (restart). No
  // model state is taken from the chip. The stop is reported and its instructions counted; it is
  // not a failure, because the test's sequence checks the debug session itself (dcsr, dpc, the
  // registers at entry and after resume: chip_eg2_rv_dm_corners_base_vseq). It is a failure when
  // no sequence of the run started a debug session (debug_session), so that an unexpected debug
  // entry is never hidden.
  localparam bit [31:0] DmMemBase = top_earlgrey_pkg::TOP_EARLGREY_RV_DM_MEM_BASE_ADDR;
  localparam bit [31:0] DmMemSize = top_earlgrey_pkg::TOP_EARLGREY_RV_DM_MEM_SIZE_BYTES;
  localparam bit [31:0] InsnDret  = 32'h7b20_0073;
  protected bit          debug_expected;
  protected string       debug_by;
  protected bit          debug_stopped;
  protected int unsigned debug_stops, debug_dropped;
  // Data-access bus errors on the item that took them (mem_error), and those CHERIoT-Sail was given
  // (cheriot_sail_cosim_step_bus_error): reported, so a run shows they reached the model. R14,
  // 2026-10-26.
  protected int unsigned mem_faults, sail_bus_errors;

  // rram_ctrl's controller writes (R7, R10): the core's write operation as its stores set it up
  // (ADDR, CONTROL with START, Write, data partition; the words it stores to wr_fifo), the words
  // the PHY accepted and not yet programmed, and the programmed ones not yet given to the model.
  protected bit [31:0]   rram_addr_reg;
  // rram_ctrl's EXEC as the core wrote it (registers.md EXEC: "A value of 0xa26a38f7 allows RRAM to
  // be used for code execution. Any other value prevents code execution."; reset value 0), cleared
  // at a chip reset: a fetch error in the RRAM host window while it holds anything else is the
  // documented one, given to the model like an armed one (2026-11-03, chip_sw_rram_exec_en: the
  // chip trapped on the fetch, the model ran the instruction).
  protected bit [31:0]   rram_exec;
  localparam bit [31:0]  RramExecMagic = 32'ha26a_38f7;
  // rram_ctrl's disable (theory_of_operation.md "RRAM Escalation & Disable": software (DIS.SW_DIS,
  // rw1s, any value but MuBi4False) or the global escalation (lc_escalate_en) disables RRAM until a
  // full reset; "all host and controller requests are blocked"): DIS.SW_DIS as the core and the
  // bench wrote it, and whether the controller's escalation input has been seen on. Cleared at a
  // chip reset. The controller's local escalation (its own faults) is not modelled.
  protected bit [3:0]    rram_sw_dis;
  protected bit          rram_escalated;
  // Main SRAM's fetch rule (sram_ctrl theory_of_operation.md "Code Execution from SRAM"): with OTP
  // HW_CFG1.EN_SRAM_IFETCH MuBi8True the EXEC CSR decides (MuBi4True allows), otherwise the life
  // cycle's HW_DEBUG_EN (on in TEST_UNLOCKED, DEV and RMA: doc/security/specs/device_life_cycle
  // "Manufacturing States"). The two OTP values are read from the OTP image at each chip reset, as
  // otp_ctrl reads its partitions then; EXEC and EXEC_REGWEN (rw0c) from the core's stores.
  // sram_ctrl "Global and Local Escalation": once escalated, every request is blocked.
  protected bit          sram_ifetch_otp;
  protected bit          sram_hw_debug_en;
  protected bit [3:0]    sram_exec;
  protected bit          sram_exec_regwen;
  protected bit          sram_escalated;
  // rv_core_ibex's mode switch as the model knows it from the core's and the bench's stores
  // (theory_of_operation.md "Execution Mode Switch": a MuBi4True write to CHERIOT_LOCK samples
  // CHERIOT_ENA, MuBi4True CHERIoT, MuBi4False ePMP, both decoded strictly; any other LOCK value or
  // an invalid ENA enters the terminal error state, which halts instruction fetch; afterwards
  // writes have no effect; reset unlocked in ePMP mode). dv_todo S9: the handover followed the
  // chip's cheriot_ena alone. The documents give no latency for the switch or the halt:
  // SwitchMaxItems and ErrorMaxItems are this check's choice.
  typedef enum int {SwUnlocked, SwLockedDis, SwLockedEna, SwError} sw_model_e;
  protected sw_model_e   sw_model;
  protected bit [3:0]    sw_ena;
  protected int unsigned sw_items_after;  // RISC-V-mode items since SwLockedEna or SwError
  protected bit          sw_reported;
  localparam int unsigned SwitchMaxItems = 64;
  // CHERIoT mode: retired instructions with an interrupt the model would take (S11).
  protected int unsigned sail_irq_wait;
  localparam int unsigned IrqMaxItems = 16;
  localparam int unsigned ErrorMaxItems = 16;
  localparam bit [31:0]  SwCfg = top_earlgrey_pkg::TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR;
  // The revocation engine as the model knows it (dv_todo S10; cheriot_mem_sys
  // theory_of_operation.md "Revocation Engine"): TBRE_BASE_ADDR and TBRE_NUM_CAPS from the stores, a
  // START taken only with NUM non-zero, BASE in a tagged region (main SRAM, the NVM) and CHERIoT
  // mode (the model's switch), the sweep clamped to the region's top; the registers locked while it
  // runs. Every tag clear the chip's engine makes must be in the sweep and of a capability the
  // model revokes (the load barrier's rule, cheriot_sail_cosim_tbre_revokes); when software sees
  // the sweep end (an even TBRE_EPOCH), no capability of the sweep the model revokes may still be
  // tagged. Capabilities the core wrote meanwhile are left out ("a capability written from then
  // on until its clear is presented is not cleared"). Before 3 Nov the model followed the chip's
  // clears.
  protected bit [31:0]   tbre_base, tbre_num, tbre_lo, tbre_hi;
  protected bit          tbre_active, tbre_end_pending;
  protected bit          tbre_written[bit [31:0]];
  protected int unsigned tbre_bad_clears, tbre_missed;
  localparam bit [31:0]  CmsRegs = top_earlgrey_pkg::TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR;
  localparam bit [31:0]  SramMainRam = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
  localparam bit [31:0]  SramMainRegs = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR;
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
    corrupt_mcause     = sb_corrupt == "insn.mcause";
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
    eg2_sysmodel::get().watch_clocks(cfg.chip_vif);
`endif
    fetch_rules_reset();
    fork
      watch_rram_writes();
      watch_tbre_clears();  // R18, 2026-10-26
      watch_revbm();        // the bitmap at the core's lookups, 2026-11-06
      // The global escalation reaches every sram_ctrl (lc_escalate_en): the retention SRAM's keys
      // are discarded, whether or not the core retires another instruction before the reset.
      forever begin
        @(posedge cfg.chip_vif.sram_main_escalated);
        ret_escalated = 1'b1;
      end
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
    string     trap_err;

    if (item.reset) begin
      // Announced faults end at a chip reset (review R12, 2026-10-25), in every build (bench faults,
      // 2026-10-28).
      if (eg2_inject_registry::in_use()) eg2_inject_registry::chip_reset(item.reset_start);
      if (stop_open) begin
        `uvm_info(`gfn, $sformatf("EG2_ISS the chip reset ends the stop (%s): %0d items not compared",
                                  stop_why, dropped), UVM_LOW)
        stop_open = 1'b0;
      end
      if (debug_stopped) begin
        `uvm_info(`gfn, $sformatf({"EG2_ISS the chip reset ends the stop at debug-mode entry: %0d ",
                                   "items not compared"}, dropped), UVM_LOW)
        debug_stopped = 1'b0;
      end
      report_left("the chip reset");
      rram_exec   = '0;
      // The retention SRAM's contents are gone after a power-on reset or a global escalation.
      if (item.por || ret_escalated || cfg.chip_vif.sram_main_escalated) begin
        if (ret_mem.size() != 0) begin
          `uvm_info(`gfn, $sformatf("EG2_ISS retention SRAM: %0d bytes forgotten (%s)", ret_mem.size(),
                                    item.por ? "power-on reset" : "global escalation"), UVM_LOW)
        end
        ret_mem.delete();
      end
      ret_escalated = 1'b0;
      fetch_rules_reset();
      sail_active = 1'b0;
      restart(item.boot_addr);
`ifdef EG2_SYSMODEL
      if (cosim != null) eg2_sysmodel::get().chip_reset(cfg);
`endif
      return;
    end
    if (cosim == null) return;
    // Debug-mode entry: the model stops (see debug_stopped). 2026-11-02.
    if (!stopped && !item.irq_only) begin
      string why = debug_entry(item);
      if (why != "") stop_at_debug_entry(item, why);
    end
    if (stopped) begin
      // rram_ctrl's operations are still followed, so their words are accounted for (R15,
      // 2026-10-26: a word programmed by no operation the wrapper saw is an error now).
      track_stores(item);
      // Not compared, and counted (R8, 2026-10-25).
      if (!item.irq_only) begin
        dropped++;
        dropped_total++;
        if (debug_stopped) debug_dropped++;
      end
      return;
    end
    // What the core's stores set up behind the model's back (rram_ctrl's controller operations);
    // then the writes behind the core's back the item's data accesses come after. R6, R7,
    // 2026-10-25. SRAM key renewal and INIT are taken in bus order with the loads and stores
    // since R19 (sram_ctrl_store, 2026-10-26).
    track_stores(item);
    apply_pending_writes(item);
    nmi_int_rule(item);
    if (!item.irq_only) mcen_step(item);
    if (tbre_end_pending) tbre_check_complete("software read an even TBRE_EPOCH");
    // The store to CHERIOT_LOCK that switches the mode retires after its write has changed
    // cheriot_ena, so its item already reads CHERIoT; it executed in RISC-V mode and is Spike's.
    // The handover is at the next instruction.
    if (!item.irq_only) check_switch(item, item.cheriot && !sail_active && !is_lock_store(item));
    if (item.cheriot && !(!sail_active && is_lock_store(item))) begin
      if (!sail_active && !item.irq_only) handover(item);
      // Every data access, also an interrupt-only item's, is accounted for (R9).
      foreach (item.dside[i]) sail_q.push_back('{a: item.dside[i], carrier: sail_steps});
      if (item.irq_only) return;  // CHERIoT-Sail takes interrupts from its own state
      bm_apply_lookups();
      sail_step(item);
      check_stop_trigger(item);
      return;
    end

    // The bus transactions completed before this item, before anything else: Spike matches its own
    // accesses against them in order, whichever item they came with.
    notify_dside(item);

    if (item.irq_only) begin
      // The next handler is the interrupt's, not the previous trap's: no trap-cause check there.
      spike_prev_trap = 1'b0;
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
    // Before the step: Spike checks mcounteren when it executes a U-mode counter read. The model's
    // own value (mcen_step, dv_todo S11, 3 Nov), no longer the chip's; the chip's is compared there.
    riscv_cosim_set_csr(cosim, int'(ibex_pkg::CSR_MCOUNTEREN), mcen_seen);
    riscv_cosim_set_ic_scr_key_valid(cosim, item.ic_scr_key_valid);
    // A fetch error the chip took on this instruction at a word the bench armed: Spike's fetch there
    // fails in this step (R11, 2026-10-25).
    fetch_rules(item);
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

    // After a synchronous trap, on the handler's first instruction (Spike not stepped yet): the
    // chip's mcause and mtval against Spike's. Spike's check_sync_trap compares only the PC and that
    // nothing was written (dv_todo S12, 3 Nov): a chip trapping for the wrong cause, or with the
    // wrong mtval, passed unless the handler read them. As CHERIoT-Sail's check (R13); not after an
    // NMI or debug entry, which have no counterpart here.
    trap_err = "";
    if (spike_prev_trap && !spike_prev_skip && item.intr && !item.nmi && !item.nmi_int &&
        !item.debug_req) begin
      bit [31:0] m = eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MCAUSE));
      bit [31:0] t = eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MTVAL));
      bit [31:0] chip_m = item.csr_mcause;
      if (corrupt_mcause && stepped >= 100) begin
        chip_m         = chip_m ^ 32'h1;
        corrupt_mcause = 1'b0;
        `uvm_info(`gfn, $sformatf("+sb_corrupt=insn.mcause: mcause of order %0d corrupted",
                                  item.order), UVM_LOW)
      end
      if (chip_m != m || item.csr_mtval != t) begin
        trap_err = $sformatf("trap cause: chip mcause %08h mtval %08h, Spike mcause %08h mtval %08h; ",
                             chip_m, item.csr_mtval, m, t);
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
    if (trap_err != "") begin
      exp.match  = 1'b0;
      exp.errors = {exp.errors, trap_err};
    end
    if (!item.more_ops) begin
      spike_prev_trap = item.trap;
      spike_prev_skip = item.nmi || item.nmi_int || item.debug_req;
      spike_steps++;
      check_spike_left(exp);
    end
`ifdef EG2_SYSMODEL
    model_check(exp, eg2_sysmodel::get().access(item));
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
    // The retention SRAM's bytes that survive this reset (decided in handle()).
    foreach (ret_mem[a]) begin
      riscv_cosim_write_mem_byte(cosim, a, ret_mem[a]);
      mark_known(a, 4'b0001 << a[1:0]);
    end
    if (ret_mem.size() != 0) begin
      `uvm_info(`gfn, $sformatf("EG2_ISS retention SRAM: %0d bytes kept over the chip reset",
                                ret_mem.size()), UVM_LOW)
    end
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
      if (a.store && !a.err) store_defines(a.addr, a.be, a.wdata);
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
      if (mailbox.exists(addr)) begin
        model_write_byte(addr, rdata[8 * b +: 8]);
        continue;
      end
      k = mem_kind(addr, r);
      if (k != MemNone && model_has(addr)) continue;
      model_write_byte(addr, rdata[8 * b +: 8]);
      if (k == MemNone) continue;
      unknown_reads++;
      fed_mem = 1'b1;
      if (k == MemNv) nv_mem[addr] = rdata[8 * b +: 8];
      else begin
        mark_known(addr, 4'b0001 << addr[1:0]);
        ret_record(addr, rdata[8 * b +: 8]);
      end
    end
    if (fed_mem && !sail_active) eg2_cosim_flush_icache(cosim);
  endfunction

  // A store of the core (the model executes it) gives the model the SRAM bytes it writes. R6.
  protected function void store_defines(bit [31:0] waddr, bit [3:0] be, bit [31:0] wdata);
    int unsigned r;
    if (mem_kind(waddr, r) == MemVol) mark_known(waddr, be);
    for (int b = 0; b < 4; b++) if (be[b]) ret_record(waddr + b, wdata[8 * b +: 8]);
  endfunction

  protected function void ret_record(bit [31:0] addr, bit [7:0] data);
    if (addr >= RetBase && addr - RetBase < RetSize) ret_mem[addr] = data;
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
        sram_reg_store(a.addr, a.wdata);
        switch_reg_store(a.addr, a.wdata);
        mcen_reg_store(a.addr, a.wdata);
        tbre_reg_store(a.addr, a.wdata);
        if (tbre_active && a.addr >= tbre_lo && a.addr < tbre_hi) begin
          tbre_written[{a.addr[31:3], 3'b000}] = 1'b1;
        end
      end else if (tbre_active &&
                   a.addr == CmsRegs + 32'(cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_EPOCH_OFFSET) &&
                   a.be[0] && !a.rdata[0]) begin
        tbre_end_pending = 1'b1;  // checked after this item's writes behind the core (handle)
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
    end else if (addr == RramCore + 32'(rram_ctrl_reg_pkg::RRAM_CTRL_EXEC_OFFSET)) begin
      rram_exec = wdata;
    end else if (addr == RramCore + 32'(rram_ctrl_reg_pkg::RRAM_CTRL_DIS_OFFSET)) begin
      rram_sw_dis |= wdata[3:0];  // DIS.SW_DIS, rw1s
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
    foreach (ret_mem[a]) if (a >= base && a - base < size) ret_mem.delete(a);
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
  // The memories with capability tags (REQ_CMS_10): main SRAM, sec SRAM, RRAM.
  protected function bit holds_tags(bit [31:0] addr);
    return (addr >= top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR &&
            addr - top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR <
            top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES) ||
           (addr >= top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR &&
            addr - top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR <
            top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES) ||
           (addr >= top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR &&
            addr - top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR <
            top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES);
  endfunction

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
          ret_record(w.addr, w.data);
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
    // Each instruction word from the model's own memory, the chip's compared with it (dv_todo S5;
    // Codex review, 2 Nov: the word came from RVFI, so a chip that fetched the wrong one passed).
    // +eg2_sail_fetch_check=0 turns it off for triage only.
    begin
      int fetch_check = 1;
      void'($value$plusargs("eg2_sail_fetch_check=%d", fetch_check));
      cheriot_sail_cosim_set_fetch_check(fetch_check);
    end
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
    // The record of the window's writes starts from that 0 (watch_revbm).
    bm_hist.delete();
    bm_lookups.delete();
    for (int r = 1; r < 16; r++) cheriot_sail_cosim_set_gpr_int(5'(r), eg2_cosim_get_gpr(cosim, r));
    cheriot_sail_cosim_handover(spike_pc,
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MTVEC)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MEPC)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MSTATUS)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MIE)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MCAUSE)),
                                eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MTVAL)));
    // Spike's cpuctrlsts too: the boot code enables the icache in RISC-V mode, and a fresh
    // CHERIoT-Sail read 0x100 where the chip read 0x101 (fetch_backpressure, rand_isa_cheriot,
    // testrig_seq, 2026-10-28).
    cheriot_sail_cosim_set_cpuctrlsts(eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_CPUCTRLSTS)));
    // And Spike's minstret: CHERIoT-Sail counted from 0 and read 0x3c1d2 where the chip read
    // 0x45b73 (rand_isa_cheriot, 2026-10-28).
    cheriot_sail_cosim_set_minstret({32'(eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MINSTRETH))),
                                     32'(eg2_cosim_get_csr(cosim, int'(ibex_pkg::CSR_MINSTRET)))});
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
    bit [31:0] err_tval, err_addr;
    int        sail_rc;
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
    cheriot_sail_cosim_set_mcounteren_writable(mcen_writable == prim_mubi_pkg::MuBi4True);
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
    // An interrupt the model would take (MIE, mie and mip set: Ibex exception_interrupts.rst) that
    // the chip keeps not taking (dv_todo S11): before 3 Nov the model took interrupts only when
    // the chip did, so a chip that dropped or held back an enabled one passed. The documents give
    // no latency; IrqMaxItems retired instructions is this check's choice.
    if (item.intr || item.debug_req) begin
      sail_irq_wait = 0;
    end else if (cheriot_sail_cosim_irq_would_take(item.pre_mip) != 0) begin
      if (++sail_irq_wait == IrqMaxItems + 1) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, $sformatf({"the chip retired %0d instructions with an enabled, ",
                                             "pending interrupt (mip %08h) it does not take; "},
                                            IrqMaxItems + 1, item.pre_mip)};
      end
    end else begin
      sail_irq_wait = 0;
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
      if (claimed[i].store && !claimed[i].err) store_defines(claimed[i].addr, claimed[i].be, claimed[i].wdata);
      // A trapping misaligned store wrote the half that got no error (the other half's bus error or
      // PMP stopped it), which CHERIoT-Sail's access fault does not: those bytes go into the model
      // as an event, as Spike's misaligned_store_second_half / misaligned_pmp_fixup apply them.
      // Not checked: Spike checks them against its own rs2, CHERIoT-Sail has no register read for
      // that. Reported. R14, 2026-10-26.
      // Only that case (dv_todo S6): Ibex load_store_unit.rst documents the split ("If an error
      // response is received for the first transaction, the second transaction will still be
      // issued"), so a half is written only when the instruction's other access got the bus
      // error. Any other trapping store that reached memory (a CHERI bounds, permission, tag or
      // seal fault, or an aligned store) is an error, and its bytes stay out of the model: before
      // 3 Nov they were copied into CHERIoT-Sail whatever the trap.
      if (item.trap && claimed[i].store && !claimed[i].err) begin
        bit other_half_err = 1'b0;
        foreach (claimed[j]) other_half_err |= j != i && claimed[j].err;
        if (other_half_err) begin
          for (int b = 0; b < 4; b++) begin
            if (claimed[i].be[b]) model_write_byte(claimed[i].addr + b, claimed[i].wdata[8 * b +: 8]);
          end
          `uvm_info(`gfn, $sformatf({"EG2_ISS the trapping store at order %0d wrote [%08h/%h]=%08h ",
                                     "(the half without a fault): given to CHERIoT-Sail"},
                                    item.order, claimed[i].addr, claimed[i].be, claimed[i].wdata),
                    UVM_LOW)
        end else begin
          exp.match  = 1'b0;
          exp.errors = {exp.errors, $sformatf({"the chip wrote [%08h/%h]=%08h on a store that trapped ",
                                               "without a bus error on another access: a trapping ",
                                               "store writes nothing; "},
                                              claimed[i].addr, claimed[i].be, claimed[i].wdata)};
        end
      end
      if (claimed[i].err && !bus_err) begin
        bus_err   = 1'b1;
        err_store = claimed[i].store;
        // Ibex's mtval for a data bus error: the access's address, the second word's for an error
        // on the second half of a misaligned access (ibex_controller.sv lsu_addr_last).
        err_tval  = claimed[i].misaligned_second ? claimed[i].addr : item.mem_addr;
        err_addr  = claimed[i].addr;  // the errored transaction (S7: armed for the model's step)
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
    fetch_rules(item);
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
      // The model executes the instruction with the errored transaction's word answering an
      // error (dv_todo S7): it takes the access fault only where its own address and capability
      // checks let the access through. Before 3 Nov it took the chip's fault without executing,
      // so a chip that skipped a CHERI check, or erred at another address, passed. The bus error
      // itself is the chip scoreboard's to predict (dv_todo S8). err_store stays for the report.
      sail_rc   = cheriot_sail_cosim_step_data_error(item.insn, item.pc, {err_addr[31:2], 2'b00},
                                                     4, err_tval);
      exp.match = sail_rc == 0 && exp.match;
      if (sail_rc == 0 && err_store != (cheriot_sail_cosim_get_mcause() == 32'd7)) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, $sformatf("bus error on a %s, CHERIoT-Sail's access fault is a %s; ",
                                            err_store ? "store" : "load",
                                            cheriot_sail_cosim_get_mcause() == 32'd7 ? "store" : "load")};
      end
      if (!item.trap) begin
        exp.match  = 1'b0;
        exp.errors = {exp.errors, "the chip's data access got a bus error and it did not trap; "};
      end
    end else begin
      exp.match = cheriot_sail_cosim_step(item.insn, item.pc, cheri_we, item.rd_addr,
                                          item.rd_wcap[32], rd_wdata, item.trap) == 0 && exp.match;
      // The destination register too (dv_todo S12): the step compares an integer result's value
      // only, so a chip that wrote the right value to the wrong register passed. As TestRIG's
      // scoreboard does (ibex_dii_scoreboard.sv, cheriot_sail_cosim_get_rd_addr).
      begin
        int unsigned sail_rd = cheriot_sail_cosim_get_rd_addr();
        if (!item.trap && !item.rf_wr_suppress && sail_rd != 0 && sail_rd != item.rd_addr) begin
          exp.match  = 1'b0;
          exp.errors = {exp.errors, $sformatf("destination register: CHERIoT-Sail x%0d, chip x%0d; ",
                                              sail_rd, item.rd_addr)};
        end
      end
    end
    for (int i = 0; i < cheriot_sail_cosim_get_num_errors(); i++) begin
      exp.errors = {exp.errors, cheriot_sail_cosim_get_error(i), "; "};
    end
    // Only main SRAM, sec SRAM and RRAM hold tags (REQ_CMS_10, REQ_SRAM_02): a capability the
    // model stored anywhere else loads back untagged (revocation_sleep via retention SRAM,
    // 2026-10-28).
    begin
      bit [31:0] sa = cheriot_sail_cosim_get_mem_addr();
      if (sail_active && cheriot_sail_cosim_get_mem_present() &&
          8'(cheriot_sail_cosim_get_mem_wmask()) == 8'hff && !holds_tags(sa)) begin
        cheriot_sail_cosim_clear_tag({sa[31:3], 3'b000});
      end
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
    model_check(exp, eg2_sysmodel::get().access(item));
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

  // Why this item is in debug mode, or "": the core took a debug request (RVFI's debug_req, set
  // on the first debug-mode instruction), it executes in rv_dm's debug memory (the debug ROM,
  // abstract commands, program buffer: a halt by ebreak or single step shows no request), or a
  // dret. As the Ibex UVM bench's riscv_sail_unmodelled(). 2026-11-02.
  protected function string debug_entry(eg2_rvfi_item item);
    if (item.debug_req) return "a debug request taken";
    if (item.pc >= DmMemBase && item.pc - DmMemBase < DmMemSize) begin
      return $sformatf("an instruction in rv_dm's debug memory at %08h", item.pc);
    end
    if (item.insn == InsnDret) return "dret";
    return "";
  endfunction

  protected function void stop_at_debug_entry(eg2_rvfi_item item, string why);
    string model = sail_active ? "CHERIoT-Sail has no debug mode" :
                   "Spike's debug entry is fixed at 0x80000000, not rv_dm's debug ROM";
    debug_stops++;
    if (!debug_expected) begin
      `uvm_error(`gfn, $sformatf({"EG2_ISS the core entered debug mode at order %0d, pc %08h (%s), ",
                                  "and no sequence of this run started a debug session"},
                                 item.order, item.pc, why))
    end
    report_left("debug-mode entry");
    `uvm_info(`gfn, $sformatf({"EG2_ISS stopped until the next reset at debug-mode entry: order %0d, ",
                               "pc %08h, %s (%s; debug session of %s), after %0d instructions"},
                              item.order, item.pc, why, model,
                              debug_expected ? debug_by : "no sequence", stepped), UVM_LOW)
    stopped       = 1'b1;
    debug_stopped = 1'b1;
    dropped       = 0;
  endfunction

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
    if (debug_stops != 0) begin
      `uvm_info(`gfn, $sformatf({"EG2_ISS %0d debug-mode entries: the model stopped at each until ",
                                 "the next chip reset, %0d instructions not compared after them"},
                                debug_stops, debug_dropped), UVM_LOW)
    end
  endfunction

  function void final_phase(uvm_phase phase);
    super.final_phase(phase);
    if (cosim != null) spike_cosim_release(cosim);
    cosim = null;
    if (sail_active) cheriot_sail_cosim_cleanup();
`ifdef EG2_SYSMODEL
    eg2_sysmodel::get().close();
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
    // An SBA byte of the revocation bitmap that watch_revbm already gave the model at its bus time.
    if (in_revbm(addr) && bm_sba_bytes.exists(addr) && bm_sba_bytes[addr] > 0) begin
      bm_sba_bytes[addr]--;
      return;
    end
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

  // The bench's mailbox in memory (`kEg2BenchAck` in RRAM): the chip's loads of [addr, addr + len)
  // are not predicted but taken as read, like a device's. The bench writes it through the backdoor
  // while the software polls it, and a chip read in flight returns the old value after the model
  // already has the new one (fetch_backpressure, tbre_corners, 2026-10-28).
  static function void backdoor_mailbox(bit [31:0] addr, int unsigned len);
    if (inst == null) return;
    for (int unsigned i = 0; i < len; i++) inst.mailbox[addr + i] = 1'b1;
  endfunction

  // A sequence of this run halts the core through rv_dm (`by` names it): a debug-mode entry stops
  // the model until the next chip reset (see debug_stopped) instead of failing the run.
  // 2026-11-02.
  // Must-fail knob +eg2_iss_ignore_debug_session=1: the call is ignored, so the run must fail at the
  // first debug-mode entry ("no sequence of this run started a debug session").
  static function void debug_session(string by);
    if (inst == null) return;
    if ($test$plusargs("eg2_iss_ignore_debug_session")) return;
    if (!inst.debug_expected) begin
      inst.uvm_report_info("eg2_iss_wrapper",
                           $sformatf({"EG2_ISS %s starts a debug session: the model stops at ",
                                      "debug-mode entry until the next chip reset"}, by),
                           UVM_LOW, `__FILE__, `__LINE__);
    end
    inst.debug_expected = 1'b1;
    inst.debug_by       = by;
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
    // main sram_ctrl's EXEC and rv_core_ibex's mode switch, written over SBA, for the fetch rule
    // and the model's switch.
    inst.sram_reg_store(addr, data);
    inst.switch_reg_store(addr, data);
    inst.tbre_reg_store(addr, data);
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
    bit [31:0] del_keys[$];
    if (inst == null) return;
    foreach (inst.nv_mem[a])
      if (a >= RramHost && a - RramHost < top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES)
        del_keys.push_back(a);
    n = del_keys.size();
    foreach (del_keys[i]) inst.nv_mem.delete(del_keys[i]);
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
        if (cfg.chip_vif.cms_tbre_clear_stale !== 1'b1 && tbre_clear_ok(addr)) begin
          tbre_clears++;
          pending_wr.push_back('{t: $realtime, kind: WrUntag, addr: addr, size: 8, data: 0,
                                 untag: 1});
        end
      end
    end
  endtask

  // ---------------------------------------------------------------- The bitmap at the core's lookups
  //
  // The core's revocation filter reads the bitmap word of a loaded capability's base after the
  // load's data (corerevbm), so the word it decides with is the one in memory at that lookup, not
  // at the load's data. Another host's write of the word (the bench's SBA, chip_sw_cheriot_
  // revocation_race) can land just before or after. Three things follow (2026-11-06):
  //   - the window's writes are recorded at its device port (every host, bus time: bm_hist), the
  //     memory's input, never the filter's;
  //   - a lookup's word must be the one the record held at the lookup, or one written while it was
  //     outstanding: anything else is an error (the filter read a value no writer produced);
  //   - an SBA write of the window reaches the model at its bus time (not when its JTAG access
  //     returns, which the vseq helpers' backdoor_write then reports a second time: skipped), and,
  //     before a step, the window's writes up to the last completed lookup are applied: the model
  //     decides the load with the word the lookup returned, as the filter did.
  typedef struct { realtime t; bit [31:0] val; } bm_val_t;
  typedef struct { realtime t_a; realtime t_d; int unsigned word; bit [31:0] data; } bm_lookup_t;
  protected bm_val_t     bm_hist[int unsigned][$];
  protected bm_lookup_t  bm_lookups[$];
  protected int unsigned bm_sba_bytes[bit [31:0]];  // SBA bytes reported at bus time, per address
  protected realtime     bm_guard;                  // two clock periods: a write's way into memory
  int unsigned bm_lookups_checked, bm_lookups_newer, bm_lookup_errors;

  protected function bit in_revbm(bit [31:0] addr);
    return addr >= RevBitsBase && addr - RevBitsBase < RevBitsBytes;
  endfunction

  // The word's value in the record at `t` (0 before any write since the mode switch: handover).
  protected function bit [31:0] bm_value_at(int unsigned w, realtime t);
    bit [31:0] v = '0;
    if (!bm_hist.exists(w)) return v;
    foreach (bm_hist[w][i]) begin
      if (bm_hist[w][i].t > t) break;
      v = bm_hist[w][i].val;
    end
    return v;
  endfunction

  protected task watch_revbm();
    int unsigned a_word[$];
    realtime     a_time[$];
    realtime     t_prev;
    forever begin
      tlul_pkg::tl_h2d_t h;
      tlul_pkg::tl_d2h_t d;
      @(posedge cfg.chip_vif.cms_clk);
      if (t_prev > 0) bm_guard = 2 * ($realtime - t_prev);
      t_prev = $realtime;
      // Every host's writes, at the window's device port.
      h = cfg.chip_vif.cms_revbm_dev_h2d;
      d = cfg.chip_vif.cms_revbm_dev_d2h;
      if (sail_active && h.a_valid && d.a_ready && h.a_opcode != tlul_pkg::Get) begin
        int unsigned w  = (h.a_address & 32'hfff) >> 2;
        bit [31:0]   nv = bm_value_at(w, $realtime);
        for (int b = 0; b < 4; b++) if (h.a_mask[b]) nv[8 * b +: 8] = h.a_data[8 * b +: 8];
        bm_hist[w].push_back('{t: $realtime, val: nv});
      end
      // The core's lookups: the word asked for, then the word returned.
      h = cfg.chip_vif.cms_core_revbm_h2d;
      d = cfg.chip_vif.cms_core_revbm_d2h;
      if (sail_active && h.a_valid && d.a_ready && h.a_opcode == tlul_pkg::Get) begin
        a_word.push_back((h.a_address & 32'hfff) >> 2);
        a_time.push_back($realtime);
      end
      if (d.d_valid && h.d_ready && a_word.size() > 0) begin
        int unsigned w   = a_word.pop_front();
        realtime     t_a = a_time.pop_front();
        if (sail_active && !d.d_error) begin
          bm_lookups.push_back('{t_a: t_a, t_d: $realtime, word: w, data: d.d_data});
        end
      end
      // The debug module's SBA writes of the window, for the model at their bus time.
      h = cfg.chip_vif.cms_sba_h2d;
      d = cfg.chip_vif.cms_sba_d2h;
      if (sail_active && h.a_valid && d.a_ready && h.a_opcode != tlul_pkg::Get &&
          in_revbm({h.a_address[31:2], 2'b00})) begin
        for (int b = 0; b < 4; b++) begin
          if (!h.a_mask[b]) continue;
          begin
            bit [31:0] a = {h.a_address[31:2], 2'b00} + b;
            pending_wr.push_back(wr_byte($realtime, a, h.a_data[8 * b +: 8]));
            if (bm_sba_bytes.exists(a)) bm_sba_bytes[a]++;
            else bm_sba_bytes[a] = 1;
          end
        end
      end
    end
  endtask

  // Before a CHERIoT-mode step: every completed lookup is checked against the record, and the
  // window's writes the last one saw are applied to the model (those up to its lookup, and a newer
  // one the lookup returned).
  protected function void bm_apply_lookups();
    realtime t_eff = -1;
    while (bm_lookups.size() > 0 && bm_lookups[0].t_d <= $realtime) begin
      bm_lookup_t l   = bm_lookups.pop_front();
      realtime    t_lo = l.t_a - bm_guard;
      bit [31:0]  v0  = bm_value_at(l.word, t_lo);
      bit         ok  = (l.data == v0);
      realtime    t_ok = l.t_a;
      bm_lookups_checked++;
      if (!ok && bm_hist.exists(l.word)) begin
        foreach (bm_hist[l.word][i]) begin
          if (bm_hist[l.word][i].t <= t_lo || bm_hist[l.word][i].t > l.t_d) continue;
          if (bm_hist[l.word][i].val == l.data) begin
            ok   = 1'b1;
            t_ok = bm_hist[l.word][i].t;
          end
        end
        if (ok) bm_lookups_newer++;
      end
      if (!ok) begin
        bm_lookup_errors++;
        model_error($sformatf({"EG2_ISS the core's revocation filter read bitmap word %0d (0x%08h) ",
                               "as 0x%08h, a value no writer produced around its lookup (the ",
                               "word held 0x%08h)"}, l.word, RevBitsBase + 4 * l.word, l.data, v0));
      end
      t_eff = t_ok;
    end
    if (t_eff < 0) return;
    // The window's pending writes up to t_eff, out of the queue's time order (only these).
`ifdef EG2_COSIM
    for (int i = 0; i < pending_wr.size(); i++) begin
      mem_wr_t w = pending_wr[i];
      if (w.t > t_eff) break;
      if (w.kind != WrByte || !in_revbm(w.addr)) continue;
      model_write_byte(w.addr, w.data);
      mark_known(w.addr, 4'b0001 << w.addr[1:0]);
      pending_wr.delete(i);
      i--;
    end
`endif
  endfunction

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

  // The model's RRAM word at `addr` now (the images, and the words rram_ctrl programmed as the
  // core's Write operations sent them), for the chip scoreboard's prediction of a capability store
  // to the NVM (cheriot_mem_sys "Write-to-Read-and-Compare Filter": it succeeds only if the NVM
  // already holds its data). 0 if the model has no value for it, or there is no co-simulation.
  function bit nv_word_now(bit [31:0] addr, output bit [31:0] w);
    w = '0;
`ifdef EG2_COSIM
    if (!enabled || cosim == null) return 1'b0;
    resolve_rram_programmed($realtime);
    return model_nv_word(addr, w);
`else
    return 1'b0;
`endif
  endfunction

  // rram_ctrl's fetch rule, decided from the model's own copy of EXEC (rram_exec; registers.md
  // EXEC: "Any other value prevents code execution"), not from what the chip did (Henrik
  // 2026-11-03: the model is self-contained; dv_todo S1). For an instruction at `pc` in the RRAM
  // host window while EXEC does not hold the value that allows execution:
  //   - the chip took a fetch error there: the documented one; armed, so Spike's fetch fails or
  //     CHERIoT-Sail takes the access fault, and both sides trap;
  //   - the chip executed it: an error, as execution from RRAM is documented as blocked.
  // A fetch error the chip takes while EXEC allows execution stays unarmed: the model executes,
  // and the instruction check reports the difference.
  // The same for RRAM disabled (DIS.SW_DIS or the global escalation) and, below, for main SRAM.
  protected function void rram_fetch_rule(eg2_rvfi_item item);
    string why;
    rram_escalated |= cfg.chip_vif.rram_ctrl_escalated;
    if (item.irq_only) return;
    if (item.pc < RramHost || item.pc - RramHost >= top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES) return;
    if (rram_sw_dis != prim_mubi_pkg::MuBi4False) why = $sformatf("DIS.SW_DIS is %0h (RRAM disabled)", rram_sw_dis);
    else if (rram_escalated) why = "the global escalation disabled RRAM";
    else if (rram_exec != RramExecMagic) why = $sformatf("EXEC is %08h, not the value that allows execution", rram_exec);
    else return;
    fetch_blocked(item, "RRAM", {"rram_ctrl ", why});
  endfunction

  // Main SRAM's fetch rule (sram_ifetch_otp, sram_hw_debug_en, sram_exec; the table in sram_ctrl
  // "Code Execution from SRAM"), and its global escalation.
  protected function void sram_fetch_rule(eg2_rvfi_item item);
    string why;
    sram_escalated |= cfg.chip_vif.sram_main_escalated;
    if (item.irq_only) return;
    if (item.pc < SramMainRam ||
        item.pc - SramMainRam >= top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES) return;
    if (sram_escalated) why = "the global escalation blocks every request";
    else if (sram_ifetch_otp && sram_exec != prim_mubi_pkg::MuBi4True)
      why = $sformatf("OTP EN_SRAM_IFETCH is MuBi8True and EXEC is %0h, not MuBi4True", sram_exec);
    else if (!sram_ifetch_otp && !sram_hw_debug_en)
      why = "OTP EN_SRAM_IFETCH is not MuBi8True and the life cycle's HW_DEBUG_EN is off";
    else return;
    fetch_blocked(item, "main SRAM", {"sram_ctrl ", why});
  endfunction

  protected function void fetch_rules(eg2_rvfi_item item);
    rram_fetch_rule(item);
    sram_fetch_rule(item);
  endfunction

  // A check of the model's own rules (fetch, mode switch, revocation engine) the chip failed: an
  // error, or EXPECTED while a bench fault announced on "insn" is open (eg2_inject_registry), as
  // an instruction mismatch is.
  protected function void model_error(string msg);
    string src = eg2_inject_registry::in_use() ? eg2_inject_registry::consult("insn") : "";
    if (src != "") `uvm_info(`gfn, {msg, " EXPECTED (injected by ", src, ")"}, UVM_LOW)
    else `uvm_error(`gfn, msg)
  endfunction

  // An instruction at item.pc in a region whose fetch rule blocks it, for `why`: a fetch error the
  // chip took there is the documented one, armed so that both models trap; an executed instruction
  // is an error.
  protected function void fetch_blocked(eg2_rvfi_item item, string region, string why);
    if (item.iside_err) begin
      if (iside_armed.exists(item.iside_err_addr)) return;
      `uvm_info(`gfn, $sformatf("EG2_ISS fetch error at %08h expected: %s", item.iside_err_addr, why),
                UVM_LOW)
      iside_armed[item.iside_err_addr] = 1'b1;
    end else begin
      model_error($sformatf({"EG2_ISS the chip executed the instruction at %08h (order %0d) from %s ",
                                  "while %s: execution there is documented as blocked"},
                                 item.pc, item.order, region, why));
    end
  endfunction

  // The fetch rules' state after a chip reset: rram_ctrl's and main sram_ctrl's registers at their
  // reset values, the escalations ended, and the two OTP values main SRAM's rule reads at boot.
  // The internal NMI, decided by the model (dv_todo S11, 3 Nov). Before, Spike was given the chip's
  // internal NMI and its mtval, so a chip that raised one without cause, at the wrong address, or
  // never, passed. Ibex documents: integrity bits are checked on every data response (loads and
  // stores, the inverted 39/32 Hsiao code of prim_secded_inv_39_32_enc); a mismatch raises an
  // internal NMI, mcause 0xFFFFFFE0, mtval "the faulting address", taken at most one instruction
  // after the faulting load; a load with bad check bits does not write its destination register.
  // The model checks the response itself and expects the NMI only then; mtval at word granularity
  // (for a split access the documents do not say which word's address; the chip's exact value is
  // passed to Spike). The instruction that takes it is the chip's, within IntgMaxItems.
  localparam int unsigned IntgMaxItems = 2;
  protected function void nmi_int_rule(eg2_rvfi_item item);
    bit found;
    foreach (item.dside[i]) begin
      bit [38:0] enc = prim_secded_pkg::prim_secded_inv_39_32_enc(item.dside[i].rdata);
      if (enc[38:32] != item.dside[i].rintg) begin
        if (intg_words.size() == 0) intg_wait = 0;
        intg_words.push_back(item.dside[i].addr);
        `uvm_info(`gfn, $sformatf("EG2_ISS data response at %08h with bad integrity bits (%02h, expected %02h): internal NMI expected",
                                  item.dside[i].addr, item.dside[i].rintg, enc[38:32]), UVM_LOW)
      end
    end
    if (item.nmi_int && !intg_prev_nmi) begin
      foreach (intg_words[i]) if (intg_words[i][31:2] == item.nmi_int_mtval[31:2]) found = 1'b1;
      if (intg_words.size() == 0) begin
        model_error($sformatf({"EG2_ISS the chip took an internal NMI (mtval %08h, order %0d) with no ",
                               "data response of bad integrity before it"}, item.nmi_int_mtval, item.order));
      end else if (!found) begin
        model_error($sformatf({"EG2_ISS the chip's internal NMI has mtval %08h (order %0d); the ",
                               "responses of bad integrity were at %p"}, item.nmi_int_mtval, item.order,
                              intg_words));
      end
      intg_words.delete();
      intg_nmi_active = 1'b1;
    end
    intg_prev_nmi = item.nmi_int;
    if (item.nmi) intg_nmi_active = 1'b1;
    if (item.irq_only) return;
    if (item.rf_wr_suppress && intg_words.size() == 0 && !item.nmi_int) begin
      model_error($sformatf({"EG2_ISS the chip suppressed the register write of order %0d (pc %08h) ",
                             "with no data response of bad integrity"}, item.order, item.pc));
    end
    if (item.insn == 32'h30200073) intg_nmi_active = 1'b0;  // mret ends the NMI handler
    if (intg_words.size() != 0 && !intg_nmi_active && !item.debug_mode) begin
      if (++intg_wait > IntgMaxItems) begin
        model_error($sformatf({"EG2_ISS data response of bad integrity at %08h, but the chip took no ",
                               "internal NMI within %0d instructions (order %0d)"}, intg_words[0],
                              IntgMaxItems, item.order));
        intg_words.delete();
      end
    end
  endfunction

  protected function void fetch_rules_reset();
    lc_ctrl_state_pkg::lc_state_e lc;
    spike_prev_trap  = 1'b0;
    intg_words.delete();
    intg_wait        = 0;
    intg_prev_nmi    = 1'b0;
    intg_nmi_active  = 1'b0;
    mcen_model       = '0;
    mcen_seen        = '0;
    mcen_writable    = prim_mubi_pkg::MuBi4True;
    mcen_regwen      = 1'b1;
    mcen_reported    = 1'b0;
    rram_sw_dis      = prim_mubi_pkg::MuBi4False;
    rram_escalated   = 1'b0;
    sw_model         = SwUnlocked;
    sw_ena           = prim_mubi_pkg::MuBi4False;  // CHERIOT_ENA's reset value, 0x9
    sw_items_after   = 0;
    sw_reported      = 1'b0;
    sail_irq_wait    = 0;
    tbre_base        = '0;
    tbre_num         = '0;
    tbre_active      = 1'b0;
    tbre_end_pending = 1'b0;
    tbre_written.delete();
    sram_exec        = prim_mubi_pkg::MuBi4False;
    sram_exec_regwen = 1'b1;
    sram_escalated   = 1'b0;
    sram_ifetch_otp  = cfg.mem_bkdr_util_h[Otp].read8(otp_ctrl_reg_pkg::EnSramIfetchOffset) ==
                       prim_mubi_pkg::MuBi8True;
    lc = otp_read_lc_partition_state(cfg.mem_bkdr_util_h[Otp]);
    sram_hw_debug_en = lc inside {LcStTestUnlocked0, LcStTestUnlocked1, LcStTestUnlocked2,
                                  LcStTestUnlocked3, LcStTestUnlocked4, LcStTestUnlocked5,
                                  LcStTestUnlocked6, LcStTestUnlocked7, LcStDev, LcStRma};
    `uvm_info(`gfn, $sformatf("EG2_ISS fetch rules at reset: OTP EN_SRAM_IFETCH %0s, %s (HW_DEBUG_EN %0s)",
                              sram_ifetch_otp ? "MuBi8True" : "not MuBi8True", lc.name(),
                              sram_hw_debug_en ? "on" : "off"), UVM_MEDIUM)
  endfunction

  // A store to rv_core_ibex's CHERIOT_ENA or CHERIOT_LOCK, by the core or the bench (sw_model).
  protected function void mcen_reg_store(bit [31:0] addr, bit [31:0] wdata);
    if (addr == SwCfg + 32'(rv_core_ibex_reg_pkg::RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REGWEN_OFFSET)) begin
      if (!wdata[0]) mcen_regwen = 1'b0;
    end else if (addr == SwCfg + 32'(rv_core_ibex_reg_pkg::RV_CORE_IBEX_MCOUNTEREN_WRITABLE_OFFSET) &&
                 mcen_regwen) begin
      mcen_writable = wdata[3:0];
    end
  endfunction

  // The chip's mcounteren as this instruction sees it against the model's, then the instruction's
  // own write (CSRRW/S/C[I] to 0x306 that did not trap), as Ibex accepts it.
  protected function void mcen_step(eg2_rvfi_item item);
    bit [31:0] mask = 32'h5 | (((32'h1 << mhpm_counter_num) - 1) << 3);
    bit [31:0] src;
    bit [2:0]  f3 = item.insn[14:12];
    mcen_seen = mcen_model;
    if (item.mcounteren != mcen_model && !mcen_reported) begin
      model_error($sformatf({"EG2_ISS mcounteren: the chip has %08h at order %0d (pc %08h), the model ",
                             "%08h (MCOUNTEREN_WRITABLE %0h)"}, item.mcounteren, item.order, item.pc,
                            mcen_model, mcen_writable));
      mcen_reported = 1'b1;
    end
    if (item.trap || item.insn[6:0] != 7'h73 || f3 == 3'b000 || f3 == 3'b100 ||
        item.insn[31:20] != 12'h306) begin
      return;
    end
    src = f3[2] ? 32'(item.insn[19:15]) : item.rs1_rdata;
    // CSRRS/CSRRC with x0 or a zero immediate do not write (the privileged ISA).
    if (f3[1] && item.insn[19:15] == 5'd0) return;
    if (mcen_writable != prim_mubi_pkg::MuBi4True) return;
    case (f3[1:0])
      2'b01:   mcen_model = src & mask;
      2'b10:   mcen_model = (mcen_model | src) & mask;
      default: mcen_model = mcen_model & ~src & mask;
    endcase
  endfunction

  protected function void switch_reg_store(bit [31:0] addr, bit [31:0] wdata);
    if (sw_model != SwUnlocked) return;  // "Further writes to either register have no effect"
    if (addr == SwCfg + 32'(rv_core_ibex_reg_pkg::RV_CORE_IBEX_CHERIOT_ENA_OFFSET)) begin
      sw_ena = wdata[3:0];
    end else if (addr == SwCfg + 32'(rv_core_ibex_reg_pkg::RV_CORE_IBEX_CHERIOT_LOCK_OFFSET)) begin
      if (wdata[3:0] != prim_mubi_pkg::MuBi4True) sw_model = SwError;
      else if (sw_ena == prim_mubi_pkg::MuBi4True) sw_model = SwLockedEna;
      else if (sw_ena == prim_mubi_pkg::MuBi4False) sw_model = SwLockedDis;
      else sw_model = SwError;
      sw_items_after = 0;
      `uvm_info(`gfn, $sformatf("EG2_ISS mode switch: CHERIOT_LOCK %0h with CHERIOT_ENA %0h: %s",
                                wdata[3:0], sw_ena, sw_model.name()), UVM_LOW)
    end
  endfunction

  // The chip's mode against the model's switch, for an item that retired (not interrupt-only):
  // CHERIoT mode only once the switch is locked enabled; ePMP mode no longer than SwitchMaxItems
  // instructions after that; nothing retiring ErrorMaxItems instructions after the error state.
  protected function void check_switch(eg2_rvfi_item item, bit entering_cheriot);
    if (sw_reported) return;
    if (entering_cheriot && sw_model != SwLockedEna) begin
      model_error($sformatf({"EG2_ISS the chip entered CHERIoT mode at order %0d (pc %08h) ",
                                  "while the switch is %s (CHERIOT_ENA %0h): only a MuBi4True ",
                                  "CHERIOT_LOCK with CHERIOT_ENA MuBi4True enables it"},
                                 item.order, item.pc, sw_model.name(), sw_ena));
      sw_reported = 1'b1;
    end else if (!item.cheriot && (sw_model == SwLockedEna || sw_model == SwError)) begin
      // Only items the chip runs in ePMP mode count (3 Nov: every item after the handover was
      // counted, so each CHERIoT run failed 65 instructions after its lock).
      sw_items_after++;
      if (sw_model == SwLockedEna && sw_items_after > SwitchMaxItems) begin
        model_error($sformatf({"EG2_ISS the chip still runs in ePMP mode %0d instructions after ",
                                    "the lock that enabled CHERIoT mode (order %0d, pc %08h)"},
                                   sw_items_after, item.order, item.pc));
        sw_reported = 1'b1;
      end else if (sw_model == SwError && sw_items_after > ErrorMaxItems) begin
        model_error($sformatf({"EG2_ISS the chip retired %0d instructions after the mode switch's ",
                                    "error state, which halts instruction fetch (order %0d, pc %08h)"},
                                   sw_items_after, item.order, item.pc));
        sw_reported = 1'b1;
      end
    end
  endfunction

  // A store to cheriot_mem_sys's revocation engine registers, by the core or the bench (tbre_*).
  protected function void tbre_reg_store(bit [31:0] addr, bit [31:0] wdata);
    bit [31:0] top;
    if (addr == CmsRegs + 32'(cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_BASE_ADDR_OFFSET)) begin
      if (!tbre_active) tbre_base = {wdata[31:3], 3'b000};  // locked while active (TBRE_REGWEN)
    end else if (addr == CmsRegs + 32'(cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_NUM_CAPS_OFFSET)) begin
      if (!tbre_active) tbre_num = wdata;
    end else if (addr == CmsRegs + 32'(cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_START_OFFSET) &&
                 wdata[0]) begin
      // A start while a sweep the model knows of has not been seen to end: the earlier sweep is
      // checked now (its end not read by software), then this start is judged.
      if (tbre_active) tbre_check_complete("the next start");
      // The tagged regions a sweep may start in: main SRAM, secure SRAM and RRAM (RFC CHERIoT Memory
      // Subsystem, "Unified Meta SRAM Address Map", Figure 2: secure SRAM has its own tags and
      // revocation bitmap; REQ_CMS_05, REQ_CMS_09). cheriot_mem_sys's programmer's guide names only
      // main SRAM and the NVM: a documentation finding (rtl_tickets.md, 2026-11-05).
      // For a sweep, main and secure SRAM are one region when secure SRAM follows main SRAM in the
      // address map: top_earlgrey.hjson sets cheriot_mem_sys's MainSramTopAddr to the end of secure
      // SRAM, and the guide says a sweep "reaching past the top of its region ends there"
      // (programmers_guide.md step 3). A sweep started in main SRAM runs on into secure SRAM and
      // ends at the top of secure SRAM (decision 2026-11-06; rtl_tickets.md asks the guide to say so).
      if (tbre_base >= SramMainRam &&
          tbre_base - SramMainRam < top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES) begin
        top = SramMainRam + top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES;
        if (top == top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR) begin
          top = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR +
                top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES;
        end
      end else if (tbre_base >= top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR &&
                   tbre_base - top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR <
                   top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES) begin
        top = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR +
              top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES;
      end else if (tbre_base >= RramHost &&
                   tbre_base - RramHost < top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES) begin
        top = RramHost + top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES;
      end else begin
        top = '0;
      end
      if (tbre_num == 0 || top == '0 || sw_model != SwLockedEna) return;  // ignored (start_err)
      tbre_lo = tbre_base;
      tbre_hi = tbre_base + 32'd8 * ((tbre_num < (top - tbre_base) / 8) ? tbre_num :
                                                                          (top - tbre_base) / 8);
      tbre_active = 1'b1;
      tbre_written.delete();
      `uvm_info(`gfn, $sformatf("EG2_ISS revocation engine: sweep [%08h, %08h)", tbre_lo, tbre_hi),
                UVM_LOW)
    end
  endfunction

  // A tag clear the chip's engine made (watch_tbre_clears): applied to the model only if the model
  // agrees with it.
  protected function int model_revokes(bit [31:0] addr);
`ifdef EG2_COSIM
    return cheriot_sail_cosim_tbre_revokes(addr);
`else
    return 0;
`endif
  endfunction

  protected function bit tbre_clear_ok(bit [31:0] addr);
    string why;
    if (!sail_active) return 1'b1;  // CHERIoT-Sail not running (or no model): nothing to decide from
    if (!tbre_active || addr < tbre_lo || addr >= tbre_hi) why = "outside any sweep the model knows of";
    else if (!tbre_written.exists(addr) && model_revokes(addr) != 1)
      why = "a capability the model does not revoke";
    if (why == "") return 1'b1;
    if (tbre_bad_clears++ < 4) begin
      model_error($sformatf("EG2_ISS the revocation engine cleared the tag at %08h: %s", addr,
                                 why));
    end
    return 1'b0;
  endfunction

  // The sweep has ended (software read an even TBRE_EPOCH, or started the next one): no capability
  // in it that the model revokes may still be tagged.
  protected function void tbre_check_complete(string seen_by);
    int unsigned n = 0;
    if (!sail_active) begin
      tbre_active      = 1'b0;
      tbre_end_pending = 1'b0;
      return;
    end
    for (bit [31:0] a = tbre_lo; a < tbre_hi; a += 8) begin
      if (!tbre_written.exists(a) && model_revokes(a) == 1) begin
        if (n++ < 4) begin
          model_error($sformatf({"EG2_ISS the revocation engine's sweep [%08h, %08h) ended (%s) ",
                                      "with the revoked capability at %08h still tagged"},
                                     tbre_lo, tbre_hi, seen_by, a));
        end
      end
    end
    tbre_missed += n;
    tbre_active = 1'b0;
    tbre_end_pending = 1'b0;
  endfunction

  // A store to main sram_ctrl's EXEC_REGWEN (rw0c) or EXEC (rw, gated by EXEC_REGWEN).
  protected function void sram_reg_store(bit [31:0] addr, bit [31:0] wdata);
    if (addr == SramMainRegs + 32'(sram_ctrl_reg_pkg::SRAM_CTRL_EXEC_REGWEN_OFFSET)) begin
      if (!wdata[0]) sram_exec_regwen = 1'b0;
    end else if (addr == SramMainRegs + 32'(sram_ctrl_reg_pkg::SRAM_CTRL_EXEC_OFFSET) &&
                 sram_exec_regwen) begin
      sram_exec = wdata[3:0];
    end
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
