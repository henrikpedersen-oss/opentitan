// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// eg2_sysmodel_replay: drives libeg2sysmodel.so (eg2_sysmodel.h) as the chip bench's ISS wrapper
// does, from the data accesses of a run instead of the chip's RVFI, and checks the model's output
// items against a reference, without a simulator.
//
//   eg2_sysmodel_replay --rom <elf> --test <elf> [--otp <vmem>] --trace <spike log>
//                       [--expect <instance>=<file> ...] [--log-dir <dir>] [--max-insns N]
//                       [--drain-us N] [--fault <name>@<instruction>]
//
// The trace is the commit log eg2sim writes with --spike-log for the same images: one line per
// retired instruction ("core 0: 3 <pc> (<insn>) [x<rd> <value>] [mem <addr> [<value>]]"). As the
// bench does per RVFI item: time advances to the instruction's (10 ns per instruction, eg2sim's
// 100 MHz); each load is read from the model, in parts within one 32-bit word, and its value is
// compared with the trace's (the register written, sign- or zero-extended by the instruction)
// where the model predicts it, or given to the model as an event where it does not; each store is
// written to the model; a WFI goes to pwrmgr. The time is eg2sim's as long as the core never sleeps
// (eg2sim adds the time a WFI sleeps; the trace does not show it), so the items come out when
// eg2sim's did: after the last instruction each --expect file (eg2sim's <instance>.model.log) must
// equal the items pulled for that instance, line for line. --drain-us lets the devices run on (eg2sim
// stops at the DV status word with bytes still in the UART FIFOs).
//
// --fault tells the model a fault was injected into the chip before the given instruction
// (eg2_sysmodel_fault, as the bench does when it injects one): lockstep or cheriot_ena_low. If the
// model then has the core halted (eg2_sysmodel_core_halted), the replay stops there: a chip whose
// core retires more instructions after that point would be a mismatch in the bench.
//
// Exit 0: every predicted load matched and every expected file was reproduced; 1 otherwise.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "eg2_sysmodel.h"

namespace {

constexpr uint64_t kPsPerInsn = 10000;  // eg2sim: one instruction per 100 MHz cycle
constexpr uint32_t kWfi = 0x10500073;

struct Access {
  bool load = false, store = false, sign = false, multi = false;
  unsigned size = 0;
};

// The data access an instruction makes (RV32 loads and stores, C, Zcb, Zcmp push/pop).
Access decode(uint32_t insn, bool compressed) {
  Access a;
  if (!compressed) {
    uint32_t op = insn & 0x7f, f3 = (insn >> 12) & 7;
    if (op == 0x03 && f3 != 3 && f3 < 6) {
      a.load = true;
      a.size = (f3 & 3) == 0 ? 1 : (f3 & 3) == 1 ? 2 : 4;
      a.sign = f3 < 4 && a.size < 4;
    } else if (op == 0x23 && f3 < 3) {
      a.store = true;
      a.size = 1u << f3;
    }
    return a;
  }
  uint32_t q = insn & 3, f3 = (insn >> 13) & 7;
  if (q == 0 && f3 == 2) a.load = true, a.size = 4;          // c.lw
  if (q == 0 && f3 == 6) a.store = true, a.size = 4;         // c.sw
  if (q == 2 && f3 == 2) a.load = true, a.size = 4;          // c.lwsp
  if (q == 2 && f3 == 6) a.store = true, a.size = 4;         // c.swsp
  if (q == 0 && f3 == 4) {                                   // Zcb
    uint32_t f = (insn >> 10) & 7;
    if (f == 0) a.load = true, a.size = 1;                   // c.lbu
    if (f == 1) a.load = true, a.size = 2, a.sign = (insn >> 6) & 1;  // c.lhu, c.lh
    if (f == 2) a.store = true, a.size = 1;                  // c.sb
    if (f == 3) a.store = true, a.size = 2;                  // c.sh
  }
  if (q == 2 && f3 == 5 && ((insn >> 8) & 0x1f) >= 0x18) {   // Zcmp cm.push / cm.pop*
    a.multi = true;
    a.size = 4;
    if (((insn >> 8) & 0x1f) == 0x18) a.store = true;
    else a.load = true;
  }
  return a;
}

std::vector<std::string> read_lines(const std::string &path) {
  std::vector<std::string> v;
  std::ifstream f(path);
  for (std::string l; std::getline(f, l);) v.push_back(l);
  return v;
}

}  // namespace

int main(int argc, char **argv) {
  std::string rom, test, otp, trace, log_dir = ".";
  std::vector<std::pair<std::string, std::string>> expects;
  uint64_t max_insns = 0, drain_us = 0, fault_at = 0;
  std::string fault;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i], v = argv[i + 1];
    if (a == "--rom") rom = v;
    else if (a == "--test") test = v;
    else if (a == "--otp") otp = v;
    else if (a == "--trace") trace = v;
    else if (a == "--log-dir") log_dir = v;
    else if (a == "--max-insns") max_insns = std::stoull(v);
    else if (a == "--drain-us") drain_us = std::stoull(v);
    else if (a == "--fault" && v.find('@') != std::string::npos) {
      fault = v.substr(0, v.find('@'));
      fault_at = std::stoull(v.substr(v.find('@') + 1));
    }
    else if (a == "--expect" && v.find('=') != std::string::npos)
      expects.push_back({v.substr(0, v.find('=')), v.substr(v.find('=') + 1)});
    else {
      std::fprintf(stderr, "eg2_sysmodel_replay: unknown option %s\n", a.c_str());
      return 1;
    }
  }
  std::ifstream tf(trace);
  if (rom.empty() || !tf) {
    std::fprintf(stderr, "usage: eg2_sysmodel_replay --rom <elf> --test <elf> [--otp <vmem>] "
                         "--trace <spike log> [--expect <instance>=<file>] [--log-dir <dir>]\n");
    return 1;
  }

  void *h = eg2_sysmodel_create(rom.c_str(), test.c_str(), otp.c_str(), log_dir.c_str());
  if (!h) return 1;
  int cause = eg2_sysmodel_reset(h, 0);
  std::printf("replay: reset, predicted cause %d\n", cause);

  uint64_t insns = 0, loads = 0, stores = 0, predicted = 0, events = 0, matched = 0,
           mismatched = 0, unchecked = 0, errors = 0, wfis = 0, dev_predicted = 0, dev_events = 0;
  std::string line;
  while (std::getline(tf, line)) {
    if (line.compare(0, 12, "core   0: 3 ") != 0) continue;
    std::istringstream ss(line.substr(12));
    std::string pc_s, insn_s;
    ss >> pc_s >> insn_s;
    if (insn_s.size() < 4) continue;
    bool compressed = insn_s.size() == 8;  // "(0xNNNN)"
    uint32_t insn = uint32_t(std::stoul(insn_s.substr(1, insn_s.size() - 2), nullptr, 16));
    ++insns;
    if (!fault.empty() && insns == fault_at) {
      if (eg2_sysmodel_fault(h, fault.c_str(), insns * kPsPerInsn)) {
        std::printf("replay: the model knows no fault %s\n", fault.c_str());
        return 1;
      }
      const char *halted = eg2_sysmodel_core_halted(h);
      if (*halted) {
        std::printf("replay: the model has the core halted from instruction %llu (%s): the trace "
                    "is not replayed further\n", (unsigned long long)insns, halted);
        break;
      }
    }
    eg2_sysmodel_step(h, insns * kPsPerInsn);
    if (!compressed && insn == kWfi) {
      eg2_sysmodel_wfi(h);
      ++wfis;
    }
    // The tokens: x<rd> <value>, c<csr>_<name> <value>, mem <addr> [<value>].
    std::vector<std::string> tok;
    for (std::string t; ss >> t;) tok.push_back(t);
    bool rd_written = false;
    uint32_t rd_value = 0;
    std::vector<std::pair<uint32_t, uint32_t>> mems;  // address, store value
    for (size_t k = 0; k < tok.size(); ++k) {
      if (tok[k] == "mem" && k + 1 < tok.size()) {
        uint32_t addr = uint32_t(std::stoul(tok[++k], nullptr, 16));
        uint32_t val = 0;
        if (k + 1 < tok.size() && tok[k + 1].compare(0, 2, "0x") == 0)
          val = uint32_t(std::stoul(tok[++k], nullptr, 16));
        mems.push_back({addr, val});
      } else if (k + 1 < tok.size()) {
        if (tok[k][0] == 'x' && !rd_written) {
          rd_written = true;
          rd_value = uint32_t(std::stoul(tok[k + 1], nullptr, 16));
        }
        ++k;
      }
    }
    if (mems.empty()) continue;
    Access a = decode(insn, compressed);
    if (!a.load && !a.store) {
      std::fprintf(stderr, "replay: %s: an access from an instruction not decoded, skipped\n",
                   line.c_str());
      ++unchecked;
      continue;
    }
    for (auto &mv : mems) {
      uint32_t addr = mv.first;
      // In parts within one word, as the bench splits RVFI's access.
      unsigned off = addr & 3, n0 = off + a.size > 4 ? 4 - off : a.size;
      if (a.store) {
        ++stores;
        int e = eg2_sysmodel_write(h, addr, n0, mv.second);
        if (n0 < a.size) e |= eg2_sysmodel_write(h, (addr & ~3u) + 4, a.size - n0,
                                                 mv.second >> (8 * n0));
        if (e) ++errors;
        continue;
      }
      ++loads;
      uint32_t d0 = 0, d1 = 0;
      int e0 = 0, e1 = 0;
      int p = eg2_sysmodel_read(h, addr, n0, &d0, &e0);
      if (n0 < a.size) p &= eg2_sysmodel_read(h, (addr & ~3u) + 4, a.size - n0, &d1, &e1);
      if (e0 || e1) ++errors;
      uint32_t mask = a.size == 4 ? 0xffffffffu : (1u << (8 * a.size)) - 1;
      uint32_t model = (d0 | (n0 < a.size ? d1 << (8 * n0) : 0)) & mask;
      if (a.sign && (model >> (8 * a.size - 1)) & 1) model |= ~mask;
      if (a.multi || !rd_written) {
        ++unchecked;  // the value is in no single register of the trace
        continue;
      }
      bool dev = addr >= 0x40000000u;  // top_earlgrey's devices; the memories are below
      if (p) {
        ++predicted;
        dev_predicted += dev;
        if (model == rd_value) {
          ++matched;
        } else if (++mismatched <= 10) {
          std::printf("replay: MISMATCH at insn %llu: load [%08x] model %08x, trace %08x\n  %s\n",
                      (unsigned long long)insns, addr, model, rd_value, line.c_str());
        }
      } else {
        ++events;
        dev_events += dev;
        eg2_sysmodel_load_event(h, addr, n0, rd_value);
        if (n0 < a.size) eg2_sysmodel_load_event(h, (addr & ~3u) + 4, a.size - n0, rd_value >> (8 * n0));
      }
    }
    if (max_insns && insns >= max_insns) break;
  }
  // eg2sim stops at the DV status word, with whatever the UARTs still hold; --drain-us lets the
  // devices go on that long after the last instruction.
  eg2_sysmodel_step(h, insns * kPsPerInsn + drain_us * 1000000ull);

  std::printf("replay: %llu instructions, %llu loads (%llu predicted, %llu of them from devices: "
              "%llu matched, %llu mismatched; %llu events, %llu of them from devices; %llu "
              "unchecked), %llu stores, %llu error responses, %llu WFI\n",
              (unsigned long long)insns, (unsigned long long)loads, (unsigned long long)predicted,
              (unsigned long long)dev_predicted, (unsigned long long)matched,
              (unsigned long long)mismatched, (unsigned long long)events,
              (unsigned long long)dev_events, (unsigned long long)unchecked,
              (unsigned long long)stores, (unsigned long long)errors, (unsigned long long)wfis);
  std::printf("replay: verdict \"%s\", stop \"%s\"\n", eg2_sysmodel_verdict(h),
              eg2_sysmodel_stop_reason(h));
  bool ok = mismatched == 0;
  for (auto &ex : expects) {
    std::vector<std::string> want = read_lines(ex.second);
    std::vector<std::string> got;
    while (eg2_sysmodel_pending(h, ex.first.c_str())) got.push_back(eg2_sysmodel_pull(h, ex.first.c_str()));
    size_t i = 0;
    while (i < want.size() && i < got.size() && want[i] == got[i]) ++i;
    bool same = i == want.size() && i == got.size();
    std::printf("replay: %s: %zu items pulled, %zu expected (%s): %s\n", ex.first.c_str(),
                got.size(), want.size(), ex.second.c_str(),
                same ? "identical" : "DIFFERENT");
    if (!same) {
      std::printf("replay:   first difference at item %zu: model \"%s\", expected \"%s\"\n", i,
                  i < got.size() ? got[i].c_str() : "<none>",
                  i < want.size() ? want[i].c_str() : "<none>");
      ok = false;
    }
  }
  eg2_sysmodel_release(h);
  std::printf("replay: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
