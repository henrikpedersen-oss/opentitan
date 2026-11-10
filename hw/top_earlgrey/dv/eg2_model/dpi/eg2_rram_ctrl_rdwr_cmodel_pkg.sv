// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI imports of the rram_ctrl controller-path reference model (eg2_rram_ctrl_rdwr_dpi.cc, on
// ip/eg2_rram_ctrl_rdwr_core.h). Judging functions return 0 when the documents allow the
// observation, else 1 (eg2_rdwr_error gives the reason). who / port: 0 software, 1 OTP plug,
// 2 lcmgr plug. sel: the requester select (0 none, 1 SwSel, 2 HwOtpSel, 3 HwLcMgrSel, 4 HwLoopBack).
// err: ERR_CODE bit order ([0] op_err, [1] mp_err, [2] rd_err, [3] wr_err).
package eg2_rram_ctrl_rdwr_cmodel_pkg;
  import "DPI-C" function chandle eg2_rdwr_create();
  import "DPI-C" function void eg2_rdwr_release(chandle h);
  import "DPI-C" function void eg2_rdwr_reset(chandle h);
  import "DPI-C" function string eg2_rdwr_error(chandle h);
  import "DPI-C" function void eg2_rdwr_cycle(chandle h, int phy_init, int ctrl_init, int dis,
                                              int sw_dis);
  import "DPI-C" function int eg2_rdwr_request(chandle h, int who, int op, int part, int num,
                                               int addr, int phase);
  import "DPI-C" function void eg2_rdwr_push(chandle h, int who, longint unsigned d39);
  import "DPI-C" function int eg2_rdwr_rd_req(chandle h, int addr, int ovfl, int sel, int part,
                                              int phase);
  import "DPI-C" function int eg2_rdwr_rd_rsp(chandle h, longint unsigned d39, int rd_err,
                                              int mp_err);
  import "DPI-C" function int eg2_rdwr_deliver(chandle h, int port, longint unsigned d39,
                                               int check);
  import "DPI-C" function int eg2_rdwr_wr_req(chandle h, int addr, int ovfl, longint unsigned d39,
                                              int last, int sel, int part, int phase);
  import "DPI-C" function int eg2_rdwr_wr_rsp(chandle h, int mp_err, int intg_err);
  import "DPI-C" function int eg2_rdwr_done(chandle h, int who, int err, int err_addr);
  import "DPI-C" function string eg2_rdwr_end(chandle h);
  import "DPI-C" function longint unsigned eg2_rdwr_with_intg(int w);
  import "DPI-C" function longint unsigned eg2_rdwr_ones();
  import "DPI-C" function int eg2_rdwr_on(chandle h);
  import "DPI-C" function int eg2_rdwr_who(chandle h);
  import "DPI-C" function int eg2_rdwr_pending(chandle h, int who);
  import "DPI-C" function int eg2_rdwr_expected_left(chandle h);
  import "DPI-C" function int eg2_rdwr_known_erraddr(chandle h);
  import "DPI-C" function int eg2_rdwr_stat(chandle h, int which);
endpackage
