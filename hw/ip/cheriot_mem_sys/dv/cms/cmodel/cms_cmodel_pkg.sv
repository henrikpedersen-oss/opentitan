// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// cms_cmodel_pkg: the CHERIoT memory subsystem's reference model (cms_core.h) for SystemVerilog
// benches, through cms_dpi.cc. A scoreboard creates one model (cms_new), feeds it every transaction
// its monitors observe, and reports a non-zero return with cms_last_messages().

package cms_cmodel_pkg;

  // TL-UL A opcodes as the model takes them.
  localparam int unsigned CmsPutFull = 0, CmsPutPartial = 1, CmsGet = 4;
  // Register offsets (registers.md).
  localparam int unsigned CmsIntrState = 'h00, CmsIntrEnable = 'h04, CmsIntrTest = 'h08,
                          CmsAlertTest = 'h0c, CmsTbreRegwen = 'h10, CmsTbreBase = 'h14,
                          CmsTbreNum = 'h18, CmsTbreStart = 'h1c, CmsTbreStatus = 'h20,
                          CmsTbreEpoch = 'h24;

  import "DPI-C" function chandle cms_new(int map);  // 0 Earl Grey, 1 the cms bench's small map
  import "DPI-C" function void cms_free(chandle h);
  import "DPI-C" function void cms_reset(chandle h);
  import "DPI-C" function void cms_set_mode(chandle h, int ena);
  import "DPI-C" function void cms_dut_reset(chandle h);   // registers back, memories kept
  import "DPI-C" function void cms_open_tag(chandle h, int unsigned addr);

  import "DPI-C" function void cms_backdoor_data(chandle h, int unsigned addr, int unsigned word);
  import "DPI-C" function void cms_backdoor_tag(chandle h, int unsigned addr, int tag);
  import "DPI-C" function void cms_backdoor_revbm(chandle h, int unsigned word_idx, int unsigned v);

  // One core data transaction: request, what the host and the meta path answered, the response.
  import "DPI-C" function int cms_core(chandle h, int op, int unsigned addr, int size, int mask,
                                       int unsigned wdata, int tag, int ena, int host_err,
                                       int meta_err, int err, int unsigned rdata, int rtag);
  // One bitmap window transaction: core_port 1 for corerevbm_tl, 0 for revbm_tl_d.
  import "DPI-C" function int cms_bitmap(chandle h, int core_port, int op, int unsigned off,
                                         int size, int mask, int unsigned wdata, int ena, int err,
                                         int unsigned rdata);
  import "DPI-C" function int cms_csr(chandle h, int write, int unsigned off, int unsigned wdata,
                                      int unsigned rdata);

  import "DPI-C" function void cms_sweep_read_error(chandle h, int unsigned addr);
  import "DPI-C" function void cms_sweep_fault(chandle h, int on_bitmap_lookup, int unsigned addr);
  import "DPI-C" function void cms_sweep_end(chandle h);
  // Faults a test injects into a running sweep, as it declares them to its scoreboard.
  import "DPI-C" function void cms_sweep_keep(chandle h, int unsigned addr);
  import "DPI-C" function void cms_sweep_revoke(chandle h, int unsigned addr);
  import "DPI-C" function void cms_sweep_dont_care(chandle h, int unsigned addr);
  import "DPI-C" function void cms_sweep_error(chandle h, int alert);
  import "DPI-C" function int cms_sweep_active(chandle h);

  import "DPI-C" function int cms_intr_expected(chandle h);
  import "DPI-C" function int cms_alerts_expected(chandle h);
  import "DPI-C" function int cms_tag(chandle h, int unsigned addr);
  import "DPI-C" function int cms_check_meta_word(chandle h, int unsigned off, int unsigned actual);
  import "DPI-C" function int unsigned cms_meta_top(chandle h);
  import "DPI-C" function string cms_last_messages();

endpackage
