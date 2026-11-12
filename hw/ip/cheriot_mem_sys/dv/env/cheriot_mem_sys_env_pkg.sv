// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

package cheriot_mem_sys_env_pkg;
  // dep packages
  import uvm_pkg::*;
  import top_pkg::*;
  import dv_utils_pkg::*;
  import dv_base_agent_pkg::*;
  import dv_lib_pkg::*;
  import tl_agent_pkg::*;
  import cip_base_pkg::*;
  import dv_base_reg_pkg::*;
  import csr_utils_pkg::*;
  import cheriot_mem_sys_regs_ral_pkg::*;
  import cheriot_mem_sys_core_agent_pkg::*;
  import mem_model_pkg::*;

  // macro includes
  `include "uvm_macros.svh"
  `include "dv_macros.svh"

  // parameters
  parameter uint   NUM_ALERTS = 1;
  parameter string LIST_OF_ALERTS[NUM_ALERTS] = {"fatal_fault"};

  // The address map the DUT is built with (tb.sv). The feature tests use a small one (build mode
  // cms_map) because they fill and sweep whole regions; otherwise the DUT's Earl Grey 2 defaults.
`ifdef CHERIOT_MEM_SYS_CMS_MAP
  parameter bit [31:0] MainSramBase = 32'h1000_0000;
  parameter bit [31:0] MainSramTop  = 32'h1000_4000;  // 16 KiB
  parameter bit [31:0] NvmBase      = 32'h3000_0000;
  parameter bit [31:0] NvmTop       = 32'h3000_2000;  // 8 KiB
`else
  parameter bit [31:0] MainSramBase = 32'h1000_0000;
  parameter bit [31:0] MainSramTop  = 32'h1003_0000;  // 192 KiB
  parameter bit [31:0] NvmBase      = 32'h3000_0000;
  parameter bit [31:0] NvmTop       = 32'h3020_0000;  // 2 MiB
`endif
  parameter bit [31:0] MetaBase     = 32'h1100_0000;

  parameter int unsigned SramBytes = MainSramTop - MainSramBase;
  parameter int unsigned NvmBytes  = NvmTop - NvmBase;
  parameter int unsigned SramCaps  = SramBytes / 8;
  parameter int unsigned NvmCaps   = NvmBytes / 8;

  // The meta SRAM (theory_of_operation.md, "Meta SRAM Address Map"): the revocation bitmap, the
  // NVM's tags, the SRAM's tags; one bit per 8-byte granule.
  parameter int unsigned RevbmBytes      = SramBytes / 64;
  parameter bit [31:0]   MetaRevbmBase   = MetaBase;
  parameter bit [31:0]   MetaNvmTagBase  = MetaRevbmBase + RevbmBytes;
  parameter bit [31:0]   MetaSramTagBase = MetaNvmTagBase + NvmBytes / 64;
  parameter bit [31:0]   MetaTop         = MetaSramTagBase + SramBytes / 64;

  // Untagged memory behind the crossbar (a peripheral window); its top answers with d_error.
  parameter bit [31:0] UntaggedBase = 32'h2000_0000;
  parameter bit [31:0] UntaggedTop  = 32'h2000_1000;
  parameter bit [31:0] DataErrBase  = 32'h2000_0f00;

  // TBRE_STATUS fields
  parameter int unsigned StatusBusy     = 0;
  parameter int unsigned StatusStartErr = 8;
  parameter int unsigned StatusSweepErr = 9;

  // Compressed permissions of a capability (CHERIoT ISA)
  parameter bit [5:0] PermsMemRw   = 6'b011111;  // memory, read-write
  parameter bit [5:0] PermsMemRo   = 6'b010110;  // memory, read-only
  parameter bit [5:0] PermsExec    = 6'b001111;  // executable
  parameter bit [5:0] PermsSealing = 6'b000111;  // sealing: never revoked
  parameter bit [5:0] PermsNone    = 6'b000000;

  // types

  // A fault the memory devices inject (cheriot_mem_sys_mem_device_seq::inject).
  typedef enum int {InjErr, InjRspIntg, InjDataIntg} inj_kind_e;
  // Which requests an injection matches.
  typedef enum int {InjAny, InjReads, InjWrites} inj_dir_e;

  // Where a core access goes.
  typedef enum int {RegionSram, RegionNvm, RegionUntagged, RegionError, RegionOther} region_e;

  // functions

  function automatic region_e region_of(bit [31:0] addr);
    if (addr >= MainSramBase && addr < MainSramTop)  return RegionSram;
    if (addr >= NvmBase && addr < NvmTop)            return RegionNvm;
    if (addr >= DataErrBase && addr < UntaggedTop)   return RegionError;
    if (addr >= UntaggedBase && addr < DataErrBase)  return RegionUntagged;
    return RegionOther;
  endfunction

  function automatic bit in_nvm(bit [31:0] addr);
    return addr >= NvmBase && addr < NvmTop;
  endfunction

  function automatic bit [31:0] granule_of(bit [31:0] addr);
    return {addr[31:3], 3'b000};
  endfunction

  // The meta SRAM word that holds the tag of the granule at `addr`.
  function automatic bit [31:0] tag_word_addr(bit [31:0] addr);
    if (in_nvm(addr)) return MetaNvmTagBase + ((addr - NvmBase) >> 8) * 4;
    return MetaSramTagBase + ((addr - MainSramBase) >> 8) * 4;
  endfunction

  // The revocation bit of a capability base: bit b covers the heap granule MainSramBase + 8 * b.
  // A base outside the bitmap has none.
  function automatic bit revbm_index(bit [31:0] base, output int unsigned bit_idx);
    bit_idx = (base - MainSramBase) >> 3;
    return base >= MainSramBase && bit_idx < RevbmBytes * 8;
  endfunction

  // A capability in memory (CHERIoT ISA): word 0 is the address; word 1 is
  // [31] reserved, [30:25] permissions, [24:22] otype, [21:18] exponent (15 means 24),
  // [17:9] top, [8:0] base.
  function automatic int unsigned cap_exp(bit [31:0] w1);
    return (w1[21:18] == 4'hf) ? 24 : int'(w1[21:18]);
  endfunction

  // The base a capability decodes to: its 2^(E+9) block is the address's, or the one below when
  // the address's middle bits are under the base field.
  function automatic bit [31:0] cap_base(bit [31:0] w0, bit [31:0] w1);
    longint unsigned e    = cap_exp(w1);
    longint unsigned b    = w1[8:0];
    longint unsigned top  = 64'(w0) >> (e + 9);
    if (((64'(w0) >> e) & 64'h1ff) < b) top--;
    return 32'((top << (e + 9)) + (b << e));
  endfunction

  // Whether a TL-UL request's command and data integrity are right (as tlul_pkg computes them).
  function automatic bit tl_a_intg_ok(tl_seq_item it);
    tlul_pkg::tl_h2d_t    h2d = '0;
    tlul_pkg::tl_a_user_t u   = it.a_user;
    h2d.a_opcode  = tlul_pkg::tl_a_op_e'(it.a_opcode);
    h2d.a_address = it.a_addr;
    h2d.a_mask    = it.a_mask;
    h2d.a_user    = u;
    return u.cmd_intg == tlul_pkg::get_cmd_intg(h2d) &&
           u.data_intg == tlul_pkg::get_data_intg(it.a_data);
  endfunction

  // Whether a TL-UL response's response and data integrity are right.
  function automatic bit tl_d_intg_ok(tl_seq_item it);
    tlul_pkg::tl_d2h_t    d2h = '0;
    tlul_pkg::tl_d_user_t u   = it.d_user;
    d2h.d_opcode = tlul_pkg::tl_d_op_e'(it.d_opcode);
    d2h.d_size   = it.d_size;
    d2h.d_error  = it.d_error;
    return u.rsp_intg == tlul_pkg::get_rsp_intg(d2h) &&
           u.data_intg == tlul_pkg::get_data_intg(it.d_data);
  endfunction

  function automatic bit [31:0] cap_meta(bit [31:0] base, int unsigned e, bit [5:0] perms,
                                         bit [2:0] otype, bit [8:0] top);
    int unsigned ee = (e >= 24) ? 24 : e;
    return {1'b0, perms, otype, (e >= 24) ? 4'hf : 4'(e), top, 9'(base >> ee)};
  endfunction

  // package sources
  `include "cheriot_mem_sys_env_cfg.sv"
  `include "cheriot_mem_sys_cov_item.sv"
  `include "cheriot_mem_sys_env_cov.sv"
  `include "cheriot_mem_sys_exp_items.sv"
  `include "cheriot_mem_sys_mem_backdoor.sv"
  `include "cheriot_mem_sys_virtual_sequencer.sv"
  `include "cheriot_mem_sys_meta_monitor.sv"
  `include "cheriot_mem_sys_ref_model.sv"
  `include "cheriot_mem_sys_scoreboard.sv"
  `include "cheriot_mem_sys_env.sv"
  `include "cheriot_mem_sys_vseq_list.sv"

endpackage
