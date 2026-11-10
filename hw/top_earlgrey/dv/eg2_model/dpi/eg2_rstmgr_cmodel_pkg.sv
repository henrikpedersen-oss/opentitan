// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI imports of the reset manager reference model (eg2_rstmgr_dpi.cc, ip/eg2_rstmgr_core.h).
// A read returns bounds: lo the bits that must read 1, hi the bits that may read 1.
package eg2_rstmgr_cmodel_pkg;
  import "DPI-C" function chandle eg2_rst_create();
  import "DPI-C" function void eg2_rst_release(chandle h);
  import "DPI-C" function void eg2_rst_reset(chandle h);
  import "DPI-C" function void eg2_rst_csr_write(chandle h, int unsigned addr, int unsigned data);
  import "DPI-C" function void eg2_rst_csr_read(chandle h, int unsigned addr, int quiet,
                                                output int unsigned lo, output int unsigned hi);
  import "DPI-C" function void eg2_rst_inputs(chandle h, int por_ast, int por, int local_rst,
                                              int unsigned lc,
                                              int unsigned sys, int unsigned rstreqs,
                                              int unsigned cause, int scan);
  import "DPI-C" function void eg2_rst_set_dump(chandle h, int alert, int unsigned n,
                                                int unsigned idx, int unsigned w);
  import "DPI-C" function int eg2_rst_dump_word_ok(chandle h, int alert, int unsigned idx,
                                                   int unsigned v, int quiet);
  import "DPI-C" function void eg2_rst_outputs(chandle h, output int unsigned lc_lo,
                                               output int unsigned lc_hi,
                                               output int unsigned sys_lo,
                                               output int unsigned sys_hi,
                                               output int unsigned sw_lo, output int unsigned sw_hi,
                                               output int unsigned req_lo,
                                               output int unsigned req_hi);
  import "DPI-C" function int unsigned eg2_rst_csr_value(chandle h, int unsigned addr);
  import "DPI-C" function int unsigned eg2_rst_resets(chandle h);
  import "DPI-C" function int unsigned eg2_rst_pors(chandle h);
  import "DPI-C" function int eg2_rst_recording(chandle h);
  import "DPI-C" function void eg2_rst_ctrl_unknown(chandle h);
endpackage
