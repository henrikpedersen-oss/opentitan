// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI functions of the rram_ctrl OTP plug reference model (ip/eg2_rram_otp_core.h), for the unit
// bench hw/ip/rram_ctrl/dv/edagen/rram_ctrl_otp and the self-test eg2_rram_otp_dpi_test.cc. A
// 64-bit value is two 32-bit words, least significant first (svBitVecVal of a bit [63:0]).

#include <cstdint>

#include "../ip/eg2_rram_otp_core.h"

using eg2_model::RramOtp;

extern "C" {

void *eg2_rram_otp_create() { return new RramOtp(); }
void eg2_rram_otp_release(void *h) { delete static_cast<RramOtp *>(h); }

// Region offset 0 is the integrity page (RRAM byte 0x1FF400), 512 the first OTP data byte.
void eg2_rram_otp_poke(void *h, unsigned off, unsigned v) {
  static_cast<RramOtp *>(h)->poke(off, uint8_t(v));
}
unsigned eg2_rram_otp_peek(void *h, unsigned off) { return static_cast<RramOtp *>(h)->peek(off); }

unsigned eg2_rram_otp_hamming(const uint32_t *d) {
  return RramOtp::hamming72_64(uint64_t(d[0]) | (uint64_t(d[1]) << 32));
}

// One command: returns the expected error code (RramOtp::Err, -1 not predicted, -2 any error);
// rdata / rmask the expected read data and the bits of it that are predicted; mem_pred 1 when the
// region afterwards is the model's; blk_lo / blk_hi the data blocks the command touched.
int eg2_rram_otp_cmd(void *h, unsigned cmd, unsigned size, unsigned addr, const uint32_t *wdata,
                     int rd_err, int wr_err, int readback_corrupt, int disabled, uint32_t *rdata,
                     uint32_t *rmask, int *mem_pred, unsigned *blk_lo, unsigned *blk_hi) {
  RramOtp::Faults f;
  f.rd_err = rd_err != 0;
  f.wr_err = wr_err != 0;
  f.readback_corrupt = readback_corrupt != 0;
  RramOtp::Result r = static_cast<RramOtp *>(h)->command(
      cmd, size, addr, uint64_t(wdata[0]) | (uint64_t(wdata[1]) << 32), f, disabled != 0);
  rdata[0] = uint32_t(r.rdata);
  rdata[1] = uint32_t(r.rdata >> 32);
  rmask[0] = uint32_t(r.rdata_mask);
  rmask[1] = uint32_t(r.rdata_mask >> 32);
  *mem_pred = r.mem_predicted;
  *blk_lo = r.blk_lo;
  *blk_hi = r.blk_hi;
  return r.err;
}

}  // extern "C"
