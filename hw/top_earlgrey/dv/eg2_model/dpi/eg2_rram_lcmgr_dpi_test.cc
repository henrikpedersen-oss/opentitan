// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the life-cycle manager plug's reference model (ip/eg2_rram_lcmgr_core.h through
// eg2_rram_lcmgr_dpi.cc): each documented behaviour as a check. Built with
// -DEG2_LCMGR_SELFTEST_FAIL one expectation is inverted and the run must fail.
// Build and run by hand (not in the shared eg2_model Makefile yet):
//   g++ -std=c++17 -O1 -I<eg2_model> dpi/eg2_rram_lcmgr_dpi.cc dpi/eg2_rram_lcmgr_dpi_test.cc
//       -o eg2_rram_lcmgr_test && ./eg2_rram_lcmgr_test

#include <cstdio>
#include <string>

extern "C" {
void *eg2_lcmgr_create();
void eg2_lcmgr_release(void *h);
void eg2_lcmgr_reset(void *h);
const char *eg2_lcmgr_last_error(void *h);
void eg2_lcmgr_init(void *h);
void eg2_lcmgr_lc_seed(void *h, int v);
void eg2_lcmgr_rma_req(void *h, int v);
void eg2_lcmgr_disable(void *h, int v);
int eg2_lcmgr_key_req(void *h, int kind);
void eg2_lcmgr_key_ack(void *h, int kind, int k0, int k1, int k2, int k3, int r0, int r1, int r2,
                       int r3);
int eg2_lcmgr_rma_started(void *h);
int eg2_lcmgr_request(void *h, int op, int part, int byte_addr, int num_words, int phase);
void eg2_lcmgr_seed_word(void *h, int idx, int data, int intg_ok);
void eg2_lcmgr_seed_done(void *h, int err);
void eg2_lcmgr_rma_write(void *h, int part, int word_addr);
void eg2_lcmgr_rma_read(void *h, int part, int word_addr, int same);
int eg2_lcmgr_rma_ack_seen(void *h);
int eg2_lcmgr_keys_acked(void *h);
int eg2_lcmgr_key_word(void *h, int which, int i);
int eg2_lcmgr_seed_valid(void *h, int s);
int eg2_lcmgr_seed_word_of(void *h, int s, int i);
int eg2_lcmgr_seed_err_lo(void *h);
int eg2_lcmgr_seed_err_hi(void *h);
int eg2_lcmgr_intg_err(void *h);
int eg2_lcmgr_init_done_allowed(void *h);
}

static int n_checks, n_fail;
static void check(const char *what, long got, long exp) {
  n_checks++;
  if (got != exp) {
    n_fail++;
    std::printf("FAIL %s: got %ld expected %ld\n", what, got, exp);
  }
}

static const int kOn = 0x5, kOff = 0xa, kFalse = 0x9, kTrue = 0x6;
static const int kPage = 512;

// Keys, then optionally both seeds read twice with the given faults.
static void keys(void *h, int seed_en) {
  eg2_lcmgr_lc_seed(h, seed_en);
  eg2_lcmgr_init(h);
  check("addr key request allowed", eg2_lcmgr_key_req(h, 0), 0);
  check("data key request before the addr ack refused", eg2_lcmgr_key_req(h, 1), 1);
  eg2_lcmgr_key_ack(h, 0, 1, 2, 3, 4, 5, 6, 7, 8);
  check("data key request allowed", eg2_lcmgr_key_req(h, 1), 0);
  eg2_lcmgr_key_ack(h, 1, 11, 12, 13, 14, 15, 16, 17, 18);
  check("keys acked", eg2_lcmgr_keys_acked(h), 1);
  check("addr key word 3", eg2_lcmgr_key_word(h, 0, 3), 4);
  check("rand data key word 0", eg2_lcmgr_key_word(h, 3, 0), 15);
}

static void seed_pass(void *h, int page, int base, int err, int flip_at, int bad_intg_at) {
  check("seed read allowed", eg2_lcmgr_request(h, 0, 1, page * kPage, 8, 0), 0);
  for (int i = 0; i < 8; i++)
    eg2_lcmgr_seed_word(h, i, base + i + (i == flip_at ? 0x100 : 0), i != bad_intg_at);
  eg2_lcmgr_seed_done(h, err);
}

int main() {
  void *h = eg2_lcmgr_create();

  // 1. Provisioned, clean seeds: both valid, values from the first pass, init_done, no seed_err.
  keys(h, kOn);
  check("init_done not before the seeds", eg2_lcmgr_init_done_allowed(h), 0);
  check("write in PhaseSeed refused", eg2_lcmgr_request(h, 1, 1, 5 * kPage, 8, 0), 1);
  check("data partition in PhaseSeed refused", eg2_lcmgr_request(h, 0, 0, 5 * kPage, 8, 0), 1);
  check("isolated page in PhaseSeed refused", eg2_lcmgr_request(h, 0, 1, 7 * kPage, 8, 0), 1);
  check("owner before creator refused", eg2_lcmgr_request(h, 0, 1, 6 * kPage, 8, 0), 1);
  check("seed read with PhaseRma refused", eg2_lcmgr_request(h, 0, 1, 5 * kPage, 8, 1), 1);
  seed_pass(h, 5, 0x1000, 0, -1, -1);
  seed_pass(h, 5, 0x1000, 0, -1, -1);
  check("creator page a third time refused", eg2_lcmgr_request(h, 0, 1, 5 * kPage, 8, 0), 1);
  seed_pass(h, 6, 0x2000, 0, -1, -1);
  seed_pass(h, 6, 0x2000, 0, -1, -1);
  check("creator valid", eg2_lcmgr_seed_valid(h, 0), 1);
  check("owner valid", eg2_lcmgr_seed_valid(h, 1), 1);
  check("creator word 7", eg2_lcmgr_seed_word_of(h, 0, 7), 0x1007);
  check("seed_err must not", eg2_lcmgr_seed_err_hi(h), 0);
  check("init_done allowed", eg2_lcmgr_init_done_allowed(h), 1);
  check("no request in StWait", eg2_lcmgr_request(h, 0, 1, 5 * kPage, 8, 0), 1);
  // Disable clears the seeds.
  eg2_lcmgr_disable(h, kTrue);
  check("creator invalid after disable", eg2_lcmgr_seed_valid(h, 0), 0);
  check("RMA after disable refused", (eg2_lcmgr_rma_req(h, kOn), eg2_lcmgr_rma_started(h)), 1);

  // 2. Discrepancy on the second creator pass, read error on the owner: both invalid, seed_err.
  eg2_lcmgr_reset(h);
  keys(h, kOn);
  seed_pass(h, 5, 0x1000, 0, -1, -1);
  seed_pass(h, 5, 0x1000, 0, 3, -1);
  seed_pass(h, 6, 0x2000, 2, -1, -1);
  seed_pass(h, 6, 0x2000, 0, -1, -1);
  check("creator invalid (discrepancy)", eg2_lcmgr_seed_valid(h, 0), 0);
  check("owner invalid (read error)", eg2_lcmgr_seed_valid(h, 1), 0);
  check("seed_err must", eg2_lcmgr_seed_err_lo(h), 1);

  // 3. Integrity error: seed invalid, intg_err, seed_err not predicted.
  eg2_lcmgr_reset(h);
  keys(h, kOn);
  seed_pass(h, 5, 0x1000, 0, -1, 2);
  seed_pass(h, 5, 0x1000, 0, -1, -1);
  seed_pass(h, 6, 0x2000, 0, -1, -1);
  seed_pass(h, 6, 0x2000, 0, -1, -1);
  check("creator invalid (integrity)", eg2_lcmgr_seed_valid(h, 0), 0);
  check("owner valid", eg2_lcmgr_seed_valid(h, 1), 1);
  check("intg_err", eg2_lcmgr_intg_err(h), 1);
  check("seed_err not required", eg2_lcmgr_seed_err_lo(h), 0);
  check("seed_err allowed", eg2_lcmgr_seed_err_hi(h), 1);

  // 4. Not provisioned and glitched enables: no seed reads, straight to StWait.
  for (int v : {kOff, 0x0, 0xf, 0x7}) {
    eg2_lcmgr_reset(h);
    keys(h, v);
    check("not provisioned: init_done allowed", eg2_lcmgr_init_done_allowed(h), 1);
    check("not provisioned: seed read refused", eg2_lcmgr_request(h, 0, 1, 5 * kPage, 8, 0), 1);
  }

  // 5. RMA: only On starts it; wipe rules; ack only after every word, no ack after an error.
  eg2_lcmgr_reset(h);
  eg2_lcmgr_rma_req(h, 0xf);
  check("glitched rma_req does not start RMA", eg2_lcmgr_rma_started(h), 1);
  eg2_lcmgr_rma_req(h, kOn);
  check("RMA from StIdle", eg2_lcmgr_rma_started(h), 0);
  check("OTP page refused", eg2_lcmgr_request(h, 1, 0, 4090 * kPage, 128, 1), 1);
  check("info page 4 refused", eg2_lcmgr_request(h, 1, 1, 4 * kPage, 128, 1), 1);
  check("isolated page allowed", eg2_lcmgr_request(h, 1, 1, 7 * kPage, 128, 1), 0);
  check("wipe with PhaseSeed refused", eg2_lcmgr_request(h, 1, 0, 0, 128, 0), 1);
  check("early ack refused", eg2_lcmgr_rma_ack_seen(h), 1);
  for (int p : {5, 6, 7})
    for (int w = 0; w < 128; w++) {
      eg2_lcmgr_rma_write(h, 1, p * 128 + w);
      eg2_lcmgr_rma_read(h, 1, p * 128 + w, 1);
    }
  for (int w = 0; w < 4090 * 128; w++) {
    eg2_lcmgr_rma_write(h, 0, w);
    eg2_lcmgr_rma_read(h, 0, w, 1);
  }
#ifdef EG2_LCMGR_SELFTEST_FAIL
  check("ack after the complete wipe (inverted: must fail)", eg2_lcmgr_rma_ack_seen(h), 1);
#else
  check("ack after the complete wipe", eg2_lcmgr_rma_ack_seen(h), 0);
#endif
  check("second RMA refused", eg2_lcmgr_rma_started(h), 1);

  eg2_lcmgr_reset(h);
  eg2_lcmgr_rma_req(h, kOn);
  eg2_lcmgr_rma_started(h);
  eg2_lcmgr_rma_write(h, 0, 0);
  eg2_lcmgr_rma_read(h, 0, 0, 0);  // read back differently
  check("no ack after a verify mismatch", eg2_lcmgr_rma_ack_seen(h), 1);

  // 6. RMA requested during the seed reads: only after StWait.
  eg2_lcmgr_reset(h);
  keys(h, kOn);
  eg2_lcmgr_rma_req(h, kOn);
  check("RMA during the seed reads refused", eg2_lcmgr_rma_started(h), 1);

  std::printf("eg2_rram_lcmgr_test: %d checks, %d failed\n", n_checks, n_fail);
  eg2_lcmgr_release(h);
  return n_fail ? 1 : 0;
}
