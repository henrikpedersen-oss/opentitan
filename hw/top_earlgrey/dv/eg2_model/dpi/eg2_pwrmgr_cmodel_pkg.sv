// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI imports of the power manager reference model (eg2_pwrmgr_dpi.cc, ip/eg2_pwrmgr_core.h).
// A read returns bounds: lo the bits that must read 1, hi the bits that may read 1.
package eg2_pwrmgr_cmodel_pkg;
  import "DPI-C" function chandle eg2_pwr_create();
  import "DPI-C" function void eg2_pwr_release(chandle h);
  import "DPI-C" function void eg2_pwr_reset(chandle h);
  import "DPI-C" function void eg2_pwr_csr_write(chandle h, int unsigned addr, int unsigned data,
                                                 int quiet);
  import "DPI-C" function void eg2_pwr_csr_read(chandle h, int unsigned addr, int quiet,
                                                output int unsigned lo, output int unsigned hi);
  import "DPI-C" function void eg2_pwr_intr(chandle h, int quiet, output int unsigned lo,
                                            output int unsigned hi);
  import "DPI-C" function void eg2_pwr_inputs(chandle h, int unsigned wake, int unsigned rst,
                                              int esc, int sleeping, int sw_rst, int main_pok,
                                              int idle, int active, int pd_n_req);
  import "DPI-C" function void eg2_pwr_settled(chandle h);
  import "DPI-C" function int unsigned eg2_pwr_csr_value(chandle h, int unsigned addr);
  import "DPI-C" function int unsigned eg2_pwr_resets(chandle h);
  import "DPI-C" function int unsigned eg2_pwr_entries(chandle h);
  import "DPI-C" function int eg2_pwr_core_low_power(chandle h);
endpackage
