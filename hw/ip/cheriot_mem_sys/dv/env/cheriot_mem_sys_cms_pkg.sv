// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The cms testbench's package (../cms/tb/cms_pkg.sv: the address map, the register offsets, the
// capability encoders and the transaction type), for the tests ported from it: with the Earl Grey 2
// map, or with the cms testbench's small map in build mode cms_map (+define+CHERIOT_MEM_SYS_CMS_MAP).
`ifndef CHERIOT_MEM_SYS_CMS_MAP
`define CMS_EARLGREY_MAP
`endif
`include "cms_pkg.sv"
