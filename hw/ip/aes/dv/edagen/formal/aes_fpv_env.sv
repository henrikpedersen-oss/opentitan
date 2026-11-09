// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// aes's FPV environment beyond the generated protocol checks (edagen aes.yaml formal.files).
//
// The EDN endpoint protocol (hw/ip/edn/doc/interfaces.md: an endpoint raises edn_req and holds it;
// EDN answers with a one-cycle edn_ack carrying edn_bus, which stays valid until the next ack). aes
// synchronizes EDN's answer with prim_sync_reqack_data (DataReg 0: the bus is read in aes's clock
// domain after the acknowledgement crosses), so it relies on both rules. With edn_i unconstrained,
// FPV found the ack without a request and the bus changing after the ack (prim_sync_reqack
// SyncReqAckAckNeedsReq, SyncReqAckHoldReq, prim_sync_reqack_data SyncReqAckDataHoldDst2SrcA/B
// counterexamples, 2026-11-07); EDN itself is verified by its own block bench.
module aes_fpv_env (
  input logic               clk_edn_i,
  input logic               rst_edn_ni,
  input edn_pkg::edn_req_t  edn_o,
  input edn_pkg::edn_rsp_t  edn_i
);
  // EDN acknowledges only an outstanding request.
  EdnAckNeedsReq_M: assume property (@(posedge clk_edn_i) disable iff (!rst_edn_ni)
      edn_i.edn_ack |-> edn_o.edn_req);
  // The entropy bus changes only with an acknowledgement (EDN's endpoint holds it in between).
  EdnBusHeld_M: assume property (@(posedge clk_edn_i) disable iff (!rst_edn_ni)
      !edn_i.edn_ack |-> $stable(edn_i.edn_bus));
  // The assumptions must leave the entropy exchange reachable (not vacuous).
  EdnAck_C: cover property (@(posedge clk_edn_i) disable iff (!rst_edn_ni)
      edn_o.edn_req && edn_i.edn_ack);
endmodule

bind aes aes_fpv_env u_fpv_env (.clk_edn_i, .rst_edn_ni, .edn_o, .edn_i);
