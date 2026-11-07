// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI imports of the debug module reference model (eg2_dm_dpi.cc, ip/eg2_dm_core.h).
package eg2_dm_cmodel_pkg;
  import "DPI-C" function chandle eg2_dm_create();
  import "DPI-C" function void eg2_dm_release(chandle h);
  import "DPI-C" function void eg2_dm_reset(chandle h);
  import "DPI-C" function void eg2_dm_hart_reset(chandle h);
  import "DPI-C" function void eg2_dm_set_enables(chandle h, int dmi, int full);
  import "DPI-C" function void eg2_dm_set_unavailable(chandle h, int u);
  import "DPI-C" function void eg2_dm_set_cheriot(chandle h, int c);
  import "DPI-C" function int eg2_dm_dmi_read(chandle h, int unsigned addr,
                                              output int unsigned data, output int unsigned mask);
  import "DPI-C" function int eg2_dm_dmi_write(chandle h, int unsigned addr, int unsigned data);
  import "DPI-C" function int eg2_dm_debug_req(chandle h);
  import "DPI-C" function int eg2_dm_ndmreset(chandle h);
  import "DPI-C" function int eg2_dm_dmactive(chandle h);
  import "DPI-C" function int eg2_dm_halted(chandle h);
  import "DPI-C" function int eg2_dm_busy(chandle h);
  import "DPI-C" function int eg2_dm_sbbusy(chandle h);
  import "DPI-C" function void eg2_dm_mem_write(chandle h, int unsigned off, int unsigned data,
                                                int unsigned be);
  import "DPI-C" function int unsigned eg2_dm_mem_read(chandle h, int unsigned off,
                                                       output int unsigned mask);
  import "DPI-C" function int eg2_dm_sba_take(chandle h, output int write, output int unsigned addr,
                                              output int unsigned size, output int unsigned wdata);
  import "DPI-C" function void eg2_dm_sba_complete(chandle h, int unsigned rdata, int error);
endpackage
