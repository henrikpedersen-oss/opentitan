// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// What the reference model expects, sent to the scoreboard.

// The response to one access: the error, the data in the bits of rdata_care, and for the core
// data port the tag values allowed (rtag_set bit 0: tag 0 allowed, bit 1: tag 1 allowed).
class cheriot_mem_sys_rsp_exp extends uvm_object;
  string     tag_relation_error;  // relational comparison, independent of predicted state
  string     what;      // the access, for messages
  bit        err;
  bit        err_care = 1'b1;
  bit [31:0] rdata;
  bit [31:0] rdata_care;
  // The tag filter preserves an incoming integrity fault, including when it merges d_error.
  // A zero syndrome means intact; a nonzero one must not be silently repaired or changed.
  bit [tlul_pkg::D2HRspIntgWidth-1:0] rsp_intg_syndrome;
  bit [tlul_pkg::DataIntgWidth-1:0] data_intg_syndrome;
  bit        intg_optional;  // ambiguous external fault ownership: intact or injected syndrome
  bit [1:0]  rtag_set = 2'b01;

  `uvm_object_utils(cheriot_mem_sys_rsp_exp)
  `uvm_object_new
endclass

// An alert the model expects: fatal_fault latched until reset, one alert of an ALERT_TEST write,
// or an alert the documents leave open (allowed, not required).
typedef enum int {AlertExpFatal, AlertExpTest, AlertExpOptional} alert_exp_e;

class cheriot_mem_sys_alert_exp extends uvm_object;
  alert_exp_e kind;

  `uvm_object_utils(cheriot_mem_sys_alert_exp)
  `uvm_object_new
endclass
