// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI imports of the clock manager reference model (eg2_clkmgr_dpi.cc, ip/eg2_clkmgr_core.h).
// A read and the outputs return bounds: lo the bits that must be 1, hi the bits that may be 1.
package eg2_clkmgr_cmodel_pkg;
  import "DPI-C" function chandle eg2_clk_create(int mutate);
  import "DPI-C" function void eg2_clk_release(chandle h);
  import "DPI-C" function void eg2_clk_reset(chandle h);
  import "DPI-C" function void eg2_clk_csr_write(chandle h, int unsigned addr, int unsigned data,
                                                 int quiet);
  import "DPI-C" function void eg2_clk_csr_read(chandle h, int unsigned addr, int quiet,
                                                output int unsigned lo, output int unsigned hi);
  import "DPI-C" function void eg2_clk_inputs(chandle h, int unsigned idle, int unsigned scanmode,
                                              int unsigned lc_debug_en, int unsigned lc_byp_req,
                                              int unsigned io_byp_ack, int unsigned all_byp_ack,
                                              int unsigned step_down, int unsigned calib,
                                              int unsigned en, int unsigned run);
  import "DPI-C" function void eg2_clk_settled(chandle h);
  import "DPI-C" function void eg2_clk_aon_period(chandle h, int unsigned cnt_io,
                                                  int unsigned cnt_main, int unsigned cnt_usb,
                                                  int unsigned steady);
  import "DPI-C" function void eg2_clk_outputs(chandle h, int quiet, output int unsigned lo,
                                               output int unsigned hi);
  import "DPI-C" function int unsigned eg2_clk_sel(chandle h);
  import "DPI-C" function int unsigned eg2_clk_sel_other(chandle h);
  import "DPI-C" function int unsigned eg2_clk_stat(chandle h, int which);

  // The output vector's bits (Eg2ClkmgrCore::kO*).
  typedef enum int {
    ClkOAllBypTrue = 0, ClkOAllBypFalse, ClkOIoBypTrue, ClkOIoBypFalse, ClkOLcAckOn, ClkOLcAckOff,
    ClkOJitterTrue, ClkOJitterFalse, ClkOHiSpeedTrue, ClkOHiSpeedFalse,
    ClkOPwrMain, ClkOPwrIo, ClkOPwrUsb,
    ClkOPeriIoDiv4, ClkOPeriIoDiv2, ClkOPeriIo, ClkOPeriUsb, ClkOPeriSpiHost1,
    ClkOTransAes, ClkOTransHmac, ClkOTransKmac, ClkOTransOtbn,
    ClkODivNormal, ClkODivStepped, ClkONumOutputs
  } clk_out_e;
endpackage
