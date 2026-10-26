// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// eg2sim --board rtos_soc: the EG2 SystemC model's second board, Ibex's CHERIoT RTOS test SoC
// (rtos_soc.h), run on its own.
//
//   eg2sim --board rtos_soc --boot <sim_boot_stub> --test <firmware ELF> [--log-dir <dir>]
//          [--max-cycles N] [--uart-idle-cycles N] [--max-insns N] [--spike-log <file>]
//          [--default-rsp-error] [--mutate barrier_none|tbre_no_inval]
//
// The images load as the SoC's run script loads them (dv/scripts/cheriot_rtos_*_run.sh,
// util/elf_to_vmem.py): every loadable segment at its physical address, the boot stub and the
// firmware's SRAM segments into the SRAM, its code RAM segments into the code RAM. The core starts at
// the boot vector, 0x0010_0080, in CHERIoT mode. UART0's bytes go to <log-dir>/uart0.log (raw, as
// uartdpi writes them) and its lines to stdout; the run ends as the testbench ends it (rtos_soc.h
// RtosRunMonitor). Judge it as a simulation is judged, with stdout as the run log:
//
//   eg2sim --board rtos_soc ... --log-dir <dir> | tee <dir>/run.log
//   dv/scripts/cheriot_rtos_verdict.sh <dir>/uart0.log <dir>/run.log 1
//
// --mutate breaks one check, as the SoC's +cheriot_rtos_mutate does (README "Mutation check"), to
// show that a suite notices: barrier_none, the load barrier never revokes; tbre_no_inval, the TBRE
// sweeps but never clears a tag.
//
// Exit: 0 the run ended on the exit string or "All tests finished" (the verdict is the script's),
// 2 it ended otherwise (a failure line, UART idle, the cycle limit, the model stopped), 1 an error.

#include "rtos_soc.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "../eg2_mpact_cpu.h"
#include "../eg2_router.h"
#include "../eg2_spike_cpu.h"
#include "../eg2_targets.h"
#include "../ip/eg2_core_system.h"
#include "../ip/eg2_periph.h"

using namespace rtos_soc;

namespace {

// rv_core_ibex of the SoC: Ibex, opentitan configuration, CHERIoT mode from reset. Spike never
// runs (the core is in CHERIoT mode from reset); its configuration only has to be valid.
Eg2CoreConfig rtos_soc_core() {
  Eg2CoreConfig c{};
  c.isa = "rv32imc_Zba_Zbb_Zbc_Zbs_Zbkb_Zbkx_Zcb_Zcmp";
  c.boot_addr = kSramBase;
  c.pmp_num_regions = 16;
  c.pmp_granularity = 0;
  c.mhpm_counter_num = 10;
  c.secure_ibex = true;
  c.icache = true;
  c.dm_start = 0xb0000000;
  c.dm_end = 0xb0000fff;
  c.clock_hz = kClockHz;
  // Ibex's CHERIoT reset state (ibex_cs_registers.sv; doc/03_reference/cheriot.rst): PCC the
  // executable root at boot_addr + 0x80; MTCC (mtvec_cap) and MEPCC the executable root, MTDC the
  // memory root, MScratchC the sealing root; every register null. mtvec: boot_addr, direct mode in
  // CHERIoT mode; mepc 0; mstatus MPIE (MPP reads M in CHERIoT mode); misa X, E, B, C, M with I and
  // U clear in CHERIoT mode, MXL 1.
  c.cheriot_at_reset = true;
  c.cheriot_reset = Eg2Handover{};
  c.cheriot_reset.pc = kSramBase + 0x80;
  c.cheriot_reset.mtvec = kSramBase;
  c.cheriot_reset.mstatus = (1u << 7) | (3u << 11);
  c.cheriot_reset.misa = (1u << 30) | (1u << 23) | (1u << 12) | (1u << 4) | (1u << 2) | (1u << 1);
  return c;
}

const eg2_regs::Block &block(const eg2_regs::Block *blocks, size_t n, const char *window) {
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(blocks[i].window, window) == 0) return blocks[i];
  }
  std::fprintf(stderr, "eg2sim: no register block %s\n", window);
  std::exit(1);
}

// The interrupt table: Sonata's (rtos_soc_regs.h), and the TBRE's tbre_done, which goes to the
// rev_ctl shim, not to a PLIC source.
const eg2_irqs::Intr kTbreIntrs[] = {{"tbre_done", 0, Eg2System::kNoPlicSource, 0}};

}  // namespace

int rtos_soc_main(int argc, char **argv,
                  bool (*load_elf)(const std::string &, std::vector<Eg2Memory *> &)) {
  std::string boot, test, spike_log, mutate;
  uint64_t max_insns = 0;
  bool rsp_error = false;
  RtosRunMonitor monitor;
  Eg2System &sys = Eg2System::get();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    bool has = i + 1 < argc;
    if (a == "--board" && has) ++i;
    else if (a == "--boot" && has) boot = argv[++i];
    else if (a == "--test" && has) test = argv[++i];
    else if (a == "--log-dir" && has) sys.log_dir = argv[++i];
    else if (a == "--max-cycles" && has) monitor.max_cycles = std::stoull(argv[++i]);
    else if (a == "--uart-idle-cycles" && has) monitor.idle_cycles = std::stoull(argv[++i]);
    else if (a == "--max-insns" && has) max_insns = std::stoull(argv[++i]);
    else if (a == "--spike-log" && has) spike_log = argv[++i];
    else if (a == "--mutate" && has) mutate = argv[++i];
    else if (a == "--default-rsp-error") rsp_error = true;
    else {
      std::fprintf(stderr, "eg2sim --board rtos_soc: unknown option %s\n", a.c_str());
      return 1;
    }
  }
  if (boot.empty() || test.empty() ||
      !(mutate.empty() || mutate == "barrier_none" || mutate == "tbre_no_inval")) {
    std::fprintf(stderr,
                 "usage: eg2sim --board rtos_soc --boot <sim_boot_stub> --test <firmware> "
                 "[--log-dir <dir>] [--max-cycles N] [--uart-idle-cycles N] [--max-insns N] "
                 "[--spike-log <file>] [--default-rsp-error] "
                 "[--mutate barrier_none|tbre_no_inval]\n");
    return 1;
  }

  // One clock: the core, the UART's baud clock (the UART model's peripheral clock, io_div4) and
  // the timer, before any model times anything by it.
  sys.set_clock_hz(Eg2System::Clk::kMain, kClockHz, 0);
  sys.set_clock_hz(Eg2System::Clk::kIoDiv4, kClockHz, 0);
  std::vector<eg2_irqs::Ip> ips(std::begin(rtos_soc_regs::kIps), std::end(rtos_soc_regs::kIps));
  ips.push_back({"cheriot_mem_sys", "cheriot_mem_sys.regs", kTbreIntrs, 1, nullptr, 0});
  sys.ips = ips.data();
  sys.nips = ips.size();

  std::vector<Eg2Router::Window> windows = {
      {"sram", kSramBase, kSramSize},       {"revbm", kRevbmBase, kRevbmSize},
      {"code_ram", kCodeBase, kCodeSize},   {"rev_ctl", kRevCtlBase, kRevCtlSize},
      {"rv_timer", kTimerBase, kTimerSize}, {"uart0", kUart0Base, kUart0Size},
      {"rv_plic", kPlicBase, kPlicSize},    {"default", 0, 0},
  };
  Eg2Router bus("bus", windows);
  Eg2SpikeCpu cpu("cpu", rtos_soc_core(), max_insns, spike_log);
  cpu.socket.bind(bus.socket);

  // The meta SRAM, starting all-zero (cheriot_mem_subsys.sv): no tag set, nothing revoked.
  std::vector<uint8_t> meta(kMetaSize, 0);
  bool barrier_none = mutate == "barrier_none";
  Eg2MpactConfig mc;
  mc.meta = &meta;
  mc.tag_regions = {{kSramBase, kSramSize, kMetaSramTags}, {kCodeBase, kCodeSize, kMetaCodeTags}};
  mc.barrier = true;
  mc.heap_base = kSramBase;
  // Ibex's barrier reads the bitmap through its own port; a base past the bitmap is not looked up
  // (ibex_trvk.sv revbm_out_of_range): not revoked.
  mc.revbm = [&meta, barrier_none](uint32_t off, uint8_t *b) {
    *b = off < kRevbmSize && !barrier_none ? meta[off] : 0;
    return true;
  };
  mc.ibex_csrs = true;
  Eg2MpactCpu iss([&cpu](tlm::tlm_command c, uint32_t a, size_t n, uint8_t *b) {
                    return cpu.transport(c, a, n, b);
                  },
                  &mc);
  cpu.set_cheriot_iss(&iss);

  Eg2Memory sram("t_sram", kSramBase, kSramSize), code("t_code_ram", kCodeBase, kCodeSize);
  RtosRevbmWindow revbm("t_revbm", &meta);
  using namespace rtos_soc_regs;
  RtosRevCtl revctl("t_rev_ctl", block(kBlocks, std::size(kBlocks), "rev_ctl"), kRevCtlBase);
  RtosClint clint("t_rv_timer", block(kBlocks, std::size(kBlocks), "rv_timer"), kTimerBase);
  eg2_model::Eg2Uart uart("t_uart0", block(kBlocks, std::size(kBlocks), "uart0"), kUart0Base);
  eg2_model::Eg2RvPlic plic("t_rv_plic", block(kBlocks, std::size(kBlocks), "rv_plic"), kPlicBase,
                            /*msip=*/false);
  RtosDefaultRsp dflt("t_default");
  dflt.error_mode = rsp_error;
  bus.to(0).bind(sram.socket);
  bus.to(1).bind(revbm.socket);
  bus.to(2).bind(code.socket);
  bus.to(3).bind(revctl.socket);
  bus.to(4).bind(clint.socket);
  bus.to(5).bind(uart.socket);
  bus.to(6).bind(plic.socket);
  bus.to(7).bind(dflt.socket);
  sys.memories["sram"] = &sram;
  sys.memories["code_ram"] = &code;
  // Instruction fetch reaches the two memories only (rtl/cheriot_rtos_xbar.sv; the debug memory
  // too, while debug access is enabled, which it never is here): any other fetch is an access fault.
  sys.fetch_map = [](uint32_t a) {
    return (a >= kSramBase && a - kSramBase < kSramSize) ||
           (a >= kCodeBase && a - kCodeBase < kCodeSize);
  };

  // The memory subsystem's TBRE, behind rev_ctl's shim (not on the crossbar).
  eg2_model::Eg2CheriotMemSys memsys(
      "t_cheriot_mem_sys", block(eg2_regs::kBlocks, std::size(eg2_regs::kBlocks),
                                 "cheriot_mem_sys.regs"), 0);
  eg2_model::Eg2TbreLayout layout;
  layout.meta = &meta;
  layout.heap_base = kSramBase;
  layout.revbm_bytes = kRevbmSize;
  layout.regions = {{kSramBase, kSramBase + kSramSize, kMetaSramTags, &sram.data()},
                    {kCodeBase, kCodeBase + kCodeSize, kMetaCodeTags, &code.data()}};
  layout.cheriot = [] { return true; };  // cheri_en_i tied 1
  layout.clk = kClk;
  memsys.configure(layout);
  memsys.no_invalidate = mutate == "tbre_no_inval";
  revctl.attach(&memsys);

  std::vector<Eg2Memory *> mems = {&sram, &code};
  if (!load_elf(boot, mems) || !load_elf(test, mems)) return 1;
  if (!monitor.open(sys.log_dir + "/uart0.log")) {
    std::fprintf(stderr, "eg2sim: cannot write %s/uart0.log\n", sys.log_dir.c_str());
    return 1;
  }
  sys.on_emit = [&monitor](const std::string &inst, const std::string &item) {
    if (inst == "uart0") monitor.byte(uint8_t(std::stoul(item.substr(0, 2), nullptr, 16)));
  };
  monitor.start();

  std::printf("eg2sim: board rtos_soc (Ibex CHERIoT RTOS test SoC), %zu windows, %s%s\n",
              windows.size(), test.c_str(),
              mutate.empty() ? "" : (" (mutation " + mutate + ")").c_str());
  auto t0 = std::chrono::steady_clock::now();
  sc_core::sc_start();
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fflush(stdout);
  bool model_stop = !Eg2ModelStop::reason().empty();
  const std::string &why = sys.finished() ? sys.verdict()
                           : model_stop   ? Eg2ModelStop::reason()
                                          : cpu.stop_reason();
  uint64_t cycles = sys.clock_cycles(kClk, cpu.now_ps());
  std::printf("eg2sim: %s after %llu instructions, %llu cycles (%.3f ms at 40 MHz), pc 0x%08x, "
              "%.2f s wall\n",
              why.c_str(), (unsigned long long)cpu.retired(), (unsigned long long)cycles,
              double(cpu.now_ps()) / 1e9, cpu.pc(), wall);
  std::printf("eg2sim: UART0 %llu bytes; TBRE %u sweep(s), %llu capabilities revoked%s\n",
              (unsigned long long)monitor.bytes(), revctl.sweeps(),
              (unsigned long long)memsys.revoked(), revctl.failed() ? "; a sweep FAILED" : "");
  dflt.report();
  monitor.close();
  sys.close_logs();
  return sys.finished() && sys.passed() ? 0 : 2;
}
