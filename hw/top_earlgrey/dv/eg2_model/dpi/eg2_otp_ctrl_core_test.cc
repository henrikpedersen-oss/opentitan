// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The OTP controller core's self-test (ip/eg2_otp_ctrl_core.h): each documented behaviour step by
// step on an OTP array of its own. EG2_OTP_TEST_BREAK=1 expects a DAI write into a digest-locked
// partition to succeed: the test must then fail.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror dpi/eg2_otp_ctrl_core_test.cc -o /tmp/otp_core_test
//   /tmp/otp_core_test && ! EG2_OTP_TEST_BREAK=1 /tmp/otp_core_test > /dev/null

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "../ip/eg2_otp_ctrl_core.h"

namespace {
using namespace eg2_model;

using C = Eg2OtpCtrlCore;
int fails = 0, checks = 0;
const bool kBreak = std::getenv("EG2_OTP_TEST_BREAK") != nullptr;

void check(bool ok, const std::string &what) {
  ++checks;
  if (!ok) {
    ++fails;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

struct Port : Eg2OtpCtrlPort {
  int done = 0, errors = 0;
  std::string stop_reason;
  void unmodelled(const std::string &s) override { stop_reason = s; }
  std::map<std::string, int> alerts;
  bool seed_rw = true, seed_hw = true, rma = false, byp = false;
  void intr(unsigned bit) override { (bit ? errors : done)++; }
  void fatal_alert(const char *name) override { alerts[name]++; }
  bool lc(Eg2OtpLc s) override {
    switch (s) {
      case Eg2OtpLc::kCreatorSeedSwRwEn: return seed_rw;
      case Eg2OtpLc::kSeedHwRdEn: return seed_hw;
      case Eg2OtpLc::kRmaState: return rma;
      case Eg2OtpLc::kCheckBypEn: return byp;
    }
    return false;
  }
};

struct Dut {
  Port port;
  std::vector<uint8_t> mem;
  C core{&port, &mem};
  Dut() { core.power_up(); }
  uint32_t cmd(uint32_t c, uint32_t addr, uint32_t w0 = 0, uint32_t w1 = 0) {
    core.csr_write(C::kDaiAddress, addr);
    core.csr_write(C::kDaiWdata0, w0);
    core.csr_write(C::kDaiWdata1, w1);
    core.csr_write(C::kDaiCmd, c);
    return core.csr_read(C::kErrCode0 + C::kDaiErr);
  }
  uint32_t rd(uint32_t addr) { return cmd(1, addr); }
  uint32_t wr(uint32_t addr, uint32_t w0, uint32_t w1 = 0) { return cmd(2, addr, w0, w1); }
  uint64_t rdata() {
    return core.csr_read(C::kDaiRdata0) | (uint64_t(core.csr_read(C::kDaiRdata1)) << 32);
  }
  uint32_t status() { return core.csr_read(C::kStatus); }
  uint32_t err(int p) { return core.csr_read(C::kErrCode0 + p); }
};

constexpr uint32_t kDaiIdle = 1u << 8, kDaiError = 1u << 1, kLciError = 1u << 2,
                   kPartError = 1u << 0;
constexpr uint32_t kCreatorSwCfg = 0x040, kCreatorSwCfgDigest = 0x1C8;

void test_init_and_rw() {
  Dut d;
  check(d.mem.size() == kEg2OtpSize, "the array is 2064 bytes after initialisation");
  check(d.status() == kDaiIdle, "STATUS after initialisation: DAI_IDLE only");
  check(d.core.csr_read(C::kDaiRegwen) == 1 && d.core.csr_read(C::kCheckTriggerRegwen) == 1,
        "REGWENs reset to 1");
  for (int p = 0; p < kEg2OtpNumParts; ++p) check(!d.core.partition_locked(p), "blank: unlocked");
  check(d.wr(kCreatorSwCfg + 2, 0x12345678) == C::kNoError, "WR CREATOR_SW_CFG word");
  check(d.mem[kCreatorSwCfg] == 0x78 && d.mem[kCreatorSwCfg + 3] == 0x12,
        "WR aligns to the 32-bit granule, little endian");
  check(d.rd(kCreatorSwCfg) == C::kNoError && d.rdata() == 0x12345678, "RD returns the word");
  check(d.port.done == 2 && d.port.errors == 0, "each command raises otp_operation_done");
  // Blank check (PG "Programming Already Programmed Regions").
  check(d.wr(kCreatorSwCfg, 0x00000001) == C::kWriteBlankError, "WR clearing bits: WriteBlank");
  check((d.status() & kDaiError) && d.port.errors == 1, "WriteBlank: DAI_ERROR and otp_error");
  check(d.rd(kCreatorSwCfg) == C::kNoError && !(d.status() & kDaiError),
        "the next command clears the DAI error");
  check(d.rdata() == 0x12345678, "a request that clears programmed bits is rejected");
  // Access errors.
  check(d.rd(kEg2OtpLcState) == C::kAccessError, "RD LIFE_CYCLE: AccessError");
  check(d.wr(kEg2OtpLcState, 1) == C::kAccessError, "WR LIFE_CYCLE: AccessError");
  check(d.rd(0x900) == C::kAccessError, "RD outside every partition: AccessError");
  check(d.wr(eg2_otp_digest_off(kEg2OtpParts[kEg2OtpHwCfg0]), 1, 1) == C::kAccessError,
        "WR a hardware digest: AccessError");
  check(d.wr(eg2_otp_zer_off(kEg2OtpParts[kEg2OtpSecret1]), 1, 1) == C::kAccessError,
        "WR a zeroization marker: AccessError");
  check(d.cmd(3, kCreatorSwCfg) == C::kAccessError, "two command bits at once: AccessError");
  // Secret partitions: scrambled, 64-bit.
  check(d.wr(kEg2OtpSramDataKeySeed + 4, 0x11223344, 0x55667788) == C::kNoError, "WR SECRET1");
  uint64_t raw = 0;
  for (unsigned i = 0; i < 8; ++i) raw |= uint64_t(d.mem[kEg2OtpSramDataKeySeed + i]) << (8 * i);
  check(raw != 0x5566778811223344ull && C::descramble(raw, kEg2OtpSecret1) == 0x5566778811223344ull,
        "SECRET1 is stored PRESENT-scrambled with its own key");
  check(d.rd(kEg2OtpSramDataKeySeed) == C::kNoError && d.rdata() == 0x5566778811223344ull,
        "RD SECRET1 descrambles");
  // SECRET2 gated by lc_creator_seed_sw_rw_en.
  d.port.seed_rw = false;
  check(d.wr(kEg2OtpRmaToken, 1, 0) == C::kAccessError, "WR RMA_TOKEN, seed_sw_rw off: refused");
  check(d.rd(kEg2OtpCreatorRootKeyShare0) == C::kAccessError, "RD root key, seed off: refused");
  check(d.rd(eg2_otp_digest_off(kEg2OtpParts[kEg2OtpSecret2])) == C::kNoError,
        "RD SECRET2's digest, seed off: allowed");
  uint8_t b[4];
  check(!d.core.window_read(kEg2OtpRmaToken, 4, b), "window read of RMA_TOKEN, seed off: error");
  d.port.seed_rw = true;
  check(d.wr(kEg2OtpRmaToken, 1, 0) == C::kNoError, "WR RMA_TOKEN, seed on: allowed");
  // Window.
  check(d.core.window_read(kCreatorSwCfg, 4, b) && b[0] == 0x78, "window read of a SW partition");
  check(!d.core.window_read(kEg2OtpSize, 4, b), "window read beyond the array: error");
}

// Independent regression for EG2's documented RRAM-backed OTP semantics.
// The second value matches the low-power test's formerly rejected write.
void test_monotonic_reprogramming() {
  Dut d;
  check(d.wr(kCreatorSwCfg, 0x00010000) == C::kNoError, "RSW first write");
  check(d.wr(kCreatorSwCfg, 0x04010000) == C::kNoError,
        "RSW accepts a second write that only sets more bits");
  check(d.rd(kCreatorSwCfg) == C::kNoError && d.rdata() == 0x04010000,
        "RSW readback contains both writes' set bits");
  check(d.wr(kCreatorSwCfg, 0x04010000) == C::kNoError, "RSW same-value write is legal");
  check(d.wr(kCreatorSwCfg, 0x00010000) == C::kWriteBlankError,
        "RSW still rejects an attempt to clear an existing bit");
}

void test_read_lock() {
  Dut d;
  unsigned rl = C::kReadLock0 + 1;  // CREATOR_SW_CFG_READ_LOCK
  d.core.csr_write(rl, 0);
  uint8_t b[4];
  check(d.rd(kCreatorSwCfg) == C::kAccessError, "READ_LOCK cleared: DAI RD refused");
  check(d.rd(kCreatorSwCfgDigest) == C::kAccessError, "READ_LOCK cleared: its digest too");
  check(!d.core.window_read(kCreatorSwCfg, 4, b), "READ_LOCK cleared: window read errors");
  d.core.csr_write(rl, 1);
  check(d.core.csr_read(rl) == 0, "READ_LOCK is rw0c: writing 1 does not unlock");
  d.port.rma = true;
  check(d.rd(kCreatorSwCfg) == C::kNoError && d.core.window_read(kCreatorSwCfg, 4, b),
        "in RMA the read-locked partition reads (lc_rma_state)");
  d.port.rma = false;
  Dut e;
  e.core.csr_write(C::kDaiRegwen, 0);
  e.core.csr_write(C::kReadLock0, 0);
  check(e.core.csr_read(C::kReadLock0) == 1, "READ_LOCK gated by DIRECT_ACCESS_REGWEN");
  int done = e.port.done;
  e.core.csr_write(C::kDaiCmd, 1);
  check(e.port.done == done, "DIRECT_ACCESS_CMD gated by DIRECT_ACCESS_REGWEN");
}

void test_digest_and_lock() {
  Dut d;
  const Eg2OtpPart &s0 = kEg2OtpParts[kEg2OtpSecret0];
  check(d.wr(kEg2OtpTestUnlockToken, 0xaaaa5555, 0x12345678) == C::kNoError, "WR SECRET0 token");
  check(d.wr(kEg2OtpTestExitToken, 1, 2) == C::kNoError, "provision second token before digest");
  check(d.cmd(4, kCreatorSwCfg) == C::kAccessError, "DIGEST on a software-digest partition");
  check(d.cmd(4, s0.off) == C::kNoError, "DIGEST SECRET0");
  check(d.rd(eg2_otp_digest_off(s0)) == C::kNoError && d.rdata() != 0, "digest programmed");
  check(d.cmd(4, s0.off) == C::kAccessError, "a second DIGEST: AccessError");
  check(!d.core.partition_locked(kEg2OtpSecret0), "the lock waits for the next initialisation");
  uint64_t dg = d.rdata();
  d.core.power_up();
  check(d.core.partition_locked(kEg2OtpSecret0), "locked after initialisation");
  check(d.core.csr_read(C::kDigest0 + 2 * kEg2OtpSecret0) == uint32_t(dg) &&
            d.core.csr_read(C::kDigest0 + 2 * kEg2OtpSecret0 + 1) == uint32_t(dg >> 32),
        "SECRET0_DIGEST CSRs from OTP");
  bool wrote = d.wr(kEg2OtpTestExitToken + 8, 4, 0) == C::kNoError;
  check(kBreak ? wrote : !wrote, "WR into a digest-locked partition: AccessError");
  check(d.rd(kEg2OtpTestUnlockToken) == C::kAccessError, "RD a locked secret partition's data");
  check(d.rd(eg2_otp_digest_off(s0)) == C::kNoError && d.rdata() == dg, "its digest reads");
  uint8_t b[8];
  check(!d.core.window_read(kEg2OtpTestUnlockToken, 4, b), "window: locked secret data errors");
  check(d.core.window_read(eg2_otp_digest_off(s0), 8, b), "window: the digest reads");
  check(d.cmd(4, s0.off) == C::kAccessError, "DIGEST on a locked partition: AccessError");
  C::LcData lc = d.core.lc_data();
  check(lc.test_tokens_valid && lc.test_unlock_token[0] == 0x55 && lc.test_unlock_token[4] == 0x78,
        "lc_ctrl gets the descrambled test tokens once SECRET0 is locked");
  // A software digest locks the same way.
  check(d.wr(kCreatorSwCfgDigest, 0xdead, 0xbeef) == C::kNoError, "WR the software digest");
  d.core.power_up();
  check(d.core.partition_locked(1) && d.wr(kCreatorSwCfg + 8, 1) == C::kAccessError,
        "a software digest locks writes after initialisation");
  check(d.rd(kCreatorSwCfg) == C::kNoError, "a locked non-secret partition still reads");

  // The old model adopted a checksum of the already-corrupt image at reset.
  // Changing data after DIGEST must instead fail against the stored digest.
  Dut corrupt;
  check(corrupt.cmd(4, s0.off) == C::kNoError, "digest before corruption");
  check(corrupt.wr(kEg2OtpTestExitToken, 1, 2) == C::kNoError,
        "partition remains writable until reset");
  corrupt.core.power_up();
  check(corrupt.err(kEg2OtpSecret0) == C::kCheckFailError,
        "reset detects data changed after digest calculation");
  check(!corrupt.core.lc_data().test_tokens_valid &&
        corrupt.port.alerts["fatal_check_error"] == 1,
        "corrupt token partition raises fatal alert and withholds tokens");

  Dut known;
  check(known.cmd(4, kEg2OtpParts[kEg2OtpHwCfg1].off) == C::kNoError,
        "digest short HW_CFG1 partition");
  known.rd(eg2_otp_digest_off(kEg2OtpParts[kEg2OtpHwCfg1]));
  check(known.rdata() == 0x0bfbee58d4257c23ull,
        "DAI digest matches independent PRESENT reference for eight zero bytes");
}

void test_outputs() {
  Dut d;
  check(d.wr(kEg2OtpEnSramIfetch, 0x96) == C::kNoError, "WR EN_SRAM_IFETCH");
  check(!d.core.en_sram_ifetch(), "HW_CFG1 is buffered: no change before initialisation");
  check(d.wr(kEg2OtpDeviceId, 0xcafe0001) == C::kNoError, "WR DEVICE_ID word 0");
  d.core.power_up();
  check(d.core.en_sram_ifetch(), "EN_SRAM_IFETCH broadcast after initialisation");
  check(d.core.hw_cfg_byte(kEg2OtpDeviceId) == 0x01, "DEVICE_ID from the HW_CFG0 buffer");
  uint8_t seed[16], s0[32], s1[32];
  check(!d.core.sram_key_seed(seed), "SRAM seed not valid while SECRET1 is unlocked");
  check(d.wr(kEg2OtpSramDataKeySeed, 0x01020304, 0x05060708) == C::kNoError, "WR SRAM seed");
  check(d.cmd(4, kEg2OtpParts[kEg2OtpSecret1].off) == C::kNoError, "DIGEST SECRET1");
  check(d.wr(kEg2OtpCreatorRootKeyShare0, 0x11111111, 0x22222222) == C::kNoError, "WR share 0");
  check(d.cmd(4, kEg2OtpParts[kEg2OtpSecret2].off) == C::kNoError, "DIGEST SECRET2");
  d.core.power_up();
  check(d.core.sram_key_seed(seed) && seed[0] == 0x04 && seed[7] == 0x05,
        "SRAM seed valid and descrambled once SECRET1 is locked");
  d.port.seed_hw = false;
  check(!d.core.creator_root_key(s0, s1), "root key not valid while seed_hw_rd_en is off");
  d.port.seed_hw = true;
  check(d.core.creator_root_key(s0, s1) && s0[0] == 0x11 && s0[4] == 0x22,
        "root key shares valid once SECRET2 is locked and seed_hw_rd_en on");
  check(d.core.lc_data().secrets_valid, "secrets_valid once SECRET2 is locked");
}

void test_zeroize() {
  Dut d;
  const Eg2OtpPart &s1 = kEg2OtpParts[kEg2OtpSecret1];
  check(d.cmd(8, kEg2OtpDeviceId) == C::kAccessError, "ZEROIZE a non-zeroizable partition");
  check(d.cmd(8, kEg2OtpNvmAddrKeySeed + 3) == C::kNoError && d.rdata() == 64,
        "ZEROIZE stores a diagnostic bit count internally");
  check(d.port.stop_reason.find("undocumented: OTP ZEROIZE RDATA") == 0,
        "conflicting ZEROIZE result documentation stops observation");
  check(d.cmd(8, kEg2OtpNvmAddrKeySeed) == C::kNoError && d.rdata() == 64, "ZEROIZE idempotent");
  check(d.cmd(8, eg2_otp_zer_off(s1)) == C::kNoError, "ZEROIZE the marker");
  d.core.power_up();
  check(d.core.zeroized(kEg2OtpSecret1), "marker above the bound: zeroized at initialisation");
  check(!d.core.zeroized(kEg2OtpSecret0), "SECRET0 not zeroized");
  // A zeroized partition is not checked: corrupt its digest in OTP, trigger consistency.
  d.mem[eg2_otp_digest_off(s1)] ^= 1;
  d.core.csr_write(C::kCheckTrigger, 2);
  check(d.err(kEg2OtpSecret1) == C::kNoError, "no consistency check on a zeroized partition");
}

void test_checks() {
  Dut d;
  d.core.csr_write(C::kCheckTrigger, 3);
  check(d.status() == kDaiIdle && d.port.errors == 0, "both checks pass on a consistent array");
  check(d.core.csr_read(C::kCheckTrigger) == 0, "CHECK_TRIGGER reads 0 (r0w1c)");
  // Consistency: LIFE_CYCLE changed behind the buffer.
  d.mem[kEg2OtpLcState] ^= 0x10;
  d.port.byp = true;
  d.core.csr_write(C::kCheckTrigger, 2);
  check(d.err(kEg2OtpLifeCycle) == C::kNoError, "lc_check_byp_en pauses the LIFE_CYCLE check");
  d.port.byp = false;
  d.core.csr_write(C::kCheckTrigger, 2);
  check(d.err(kEg2OtpLifeCycle) == C::kCheckFailError, "LIFE_CYCLE mismatch: CheckFailError");
  check((d.core.csr_read(C::kPartitionStatus) >> kEg2OtpLifeCycle) & 1, "PARTITION_STATUS bit");
  check((d.status() & kPartError) && d.port.alerts["fatal_check_error"] == 1 && d.port.errors == 1,
        "PARTITION_ERROR, otp_error and fatal_check_error");
  check(!d.core.lc_data().valid, "life-cycle data invalid after the failure");
  // Integrity: a locked HW_CFG0 with a buffer fault.
  Dut e;
  check(e.wr(kEg2OtpDeviceId, 0x5a5a5a5a) == C::kNoError && e.cmd(4, kEg2OtpDeviceId) == 0,
        "HW_CFG0 written and digested");
  e.core.power_up();
  e.core.csr_write(C::kCheckTrigger, 1);
  check(e.err(kEg2OtpHwCfg0) == C::kNoError, "integrity passes on an intact buffer");
  e.core.inject_buffer_fault(kEg2OtpHwCfg0, 2, 0x40);
  e.core.csr_write(C::kCheckTriggerRegwen, 0);
  e.core.csr_write(C::kCheckTrigger, 1);
  check(e.err(kEg2OtpHwCfg0) == C::kNoError, "CHECK_TRIGGER_REGWEN 0: no check");
  Dut f;
  check(f.wr(kEg2OtpDeviceId, 0x5a5a5a5a) == C::kNoError && f.cmd(4, kEg2OtpDeviceId) == 0, "");
  f.core.power_up();
  f.core.inject_buffer_fault(kEg2OtpHwCfg0, 2, 0x40);
  f.core.csr_write(C::kCheckTrigger, 1);
  check(f.err(kEg2OtpHwCfg0) == C::kCheckFailError, "buffer fault: integrity CheckFailError");
  check(f.core.hw_cfg_byte(kEg2OtpDeviceId) == 0, "HW_CFG0 outputs back to their defaults");
  check(f.cmd(1, kEg2OtpDeviceId) == C::kAccessError, "a partition in error refuses the DAI");
  // Consistency of a digest partition: a DIGEST written after initialisation.
  Dut g;
  check(g.cmd(4, kEg2OtpParts[kEg2OtpHwCfg1].off) == C::kNoError, "DIGEST HW_CFG1");
  g.core.csr_write(C::kCheckTrigger, 2);
  check(g.err(kEg2OtpHwCfg1) == C::kCheckFailError,
        "OTP digest differs from the buffered one: consistency CheckFailError (PG: no change "
        "after the digest before the reset)");
}

void test_lci_and_escalation() {
  Dut d;
  uint16_t w[44] = {};
  for (int i = 0; i < 44; ++i) w[i] = uint16_t(0x0101 * (i + 1));
  check(d.core.lci_program(w), "LCI programs a blank LIFE_CYCLE");
  C::LcData lc = d.core.lc_data();
  check(lc.valid && lc.count[0] == 0x0101 && lc.state[0] == 0x1919, "the LIFE_CYCLE buffer follows");
  w[3] = 0;
  check(!d.core.lci_program(w), "LCI clearing a bit fails");
  check(d.err(C::kLciErr) == C::kWriteBlankError && (d.status() & kLciError) &&
            d.port.alerts["fatal_check_error"] == 1,
        "LCI WriteBlank: ERR_CODE_12, LCI_ERROR, fatal_check_error");
  w[3] = 0x0404;
  check(!d.core.lci_program(w), "the LCI is in its terminal error state");
  Dut e;
  for (int i = 0; i < 44; ++i) w[i] = 0;
  e.core.inject_lci_error(2);
  check(e.core.lci_program(w) && !e.core.lci_program(w), "injected error hits the 2nd request");
  Dut f;
  f.core.lc_escalate();
  check(f.status() == (kPartError | kDaiError | kLciError), "escalation: all errors, DAI not idle");
  bool all7 = true;
  for (unsigned i = 0; i < 13; ++i) all7 = all7 && f.err(int(i)) == C::kFsmStateError;
  check(all7, "escalation: every ERR_CODE is FsmStateError");
  int done = f.port.done;
  f.core.csr_write(C::kDaiAddress, kCreatorSwCfg);
  f.core.csr_write(C::kDaiCmd, 1);
  check(f.port.done == done, "escalation: the DAI accepts no command");
  check(!f.core.lci_program(w) && !f.core.lc_data().valid, "escalation: LCI and outputs dead");
}

}  // namespace

int main() {
  test_init_and_rw();
  test_monotonic_reprogramming();
  test_read_lock();
  test_digest_and_lock();
  test_outputs();
  test_zeroize();
  test_checks();
  test_lci_and_escalation();
  std::printf("otp_ctrl core self-test: %d checks, %d failed%s\n", checks, fails,
              kBreak ? " (EG2_OTP_TEST_BREAK: must fail)" : "");
  return fails ? 1 : 0;
}
