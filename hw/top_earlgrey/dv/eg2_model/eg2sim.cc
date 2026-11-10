// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// eg2sim: the EG2 SystemC model run on its own (doc/system_model in eg2_top_verification).
//
//   eg2sim --rom <rom.elf> --test <test.elf> [--otp <image.24.vmem>] [--max-insns N]
//   eg2sim --rom <mask_rom.elf> --rram-image <test>_rom_ext_assembled.img [--test <test_rom_ext.elf>] --otp <keyed .24.vmem>
//     (secure boot: the test ELF only for its symbols and log strings)
//          [--spike-log <file>] [--set <window>.<REGISTER>=<value> ...]
//          [--seed N] [--param <name>=<value> ...] [--log-dir <dir>] [--stim <key>=<value> ...]
//          [--sym <name>=<value>[:<bytes>] ...] [--cheriot-iss mpact|none]
//          [--fault <name>[@<when>] ...] [--periph-debug <level>[,stderr]]
//   eg2sim --board rtos_soc --boot <elf> --test <elf> ...     (rtos_soc/rtos_soc.cc)
//
// --board selects the platform: earlgrey (the default: Earl Grey 2, below) or rtos_soc, Ibex's
// CHERIoT RTOS test SoC (rtos_soc/rtos_soc.h; needs EG2_MPACT), which has its own options.
//
// The core runs Spike in RISC-V mode. When software locks CHERIoT mode (CHERIOT_LOCK), it hands
// over to MPACT-CHERIoT from the next instruction (eg2_mpact_cpu.h; eg2sim built with EG2_MPACT),
// until the next chip reset. --cheriot-iss none, or an eg2sim built without MPACT, stops the run at
// the lock instead. --spike-log logs MPACT's instructions too, marked (MPACT).
//
// The platform is built from the generated window table: a window with a register block (from the
// IP's hjson, eg2_model_regs.h) is that IP's model (eg2_ip_models.cc: its behaviour where milestone
// M3 or later has modelled it, its registers, interrupts and alerts otherwise), every other window
// is a memory with DMI (*.ram, rom_ctrl.rom, rram_ctrl.host, cheriot_mem_sys.revbm). The
// ROM and test ELFs are loaded as the bench loads them into the chip; Spike starts at the ROM's
// reset vector. --otp loads an OTP image (the bench's img_*.24.vmem: one 16-bit word and its ECC
// per line) into otp_ctrl's SW_CFG_WINDOW, which presents the OTP bytes at their addresses; the
// window's access control (secret partitions, locks) is not modelled yet. --set presets a register
// after reset with the value hardware would give it (for example sensor_ctrl.STATUS=1 for the
// AST's initialisation done), to see what stops the run next until a model does it (M3).
//
// --seed and --param write the EG2 framework's software seed (kEg2Seed) and test parameters
// (kEg2Param_<name>) into the test image by symbol, as the chip bench's chip_sw_eg2_base_vseq does
// with sw_symbol_backdoor_overwrite; --sym does the same for any symbol of the test image (a test
// that waits for the bench to set one, kUartIdx for example). --stim gives the peripheral models
// what an agent would drive into the chip (ip/eg2_periph_stim.h lists the keys). The items the
// modelled peripherals send and receive go to <log-dir>/<instance>.model.log (default: the current
// directory), in the format of the chip bench's <instance>.chip.log where the bench has one.
// A stimulus can also come at a point of the run, as a bench sequence drives it:
// --stim <key>=<value>@<point>[@<point>...][+<delay>] applies it after <delay> each time the points
// happen in that order, or <key>=<value>@<delay> once after the start. A point: log:<message>
// (software logs exactly <message>; <prefix>* any message starting with <prefix>),
// status:<name> (software writes that test status: InBootRom, InTest, InWfi, ... or a number),
// wfi (the core sleeps on a WFI), lowpower (pwrmgr starts a low-power entry). <delay> is <n>us,
// <n>ns or <n>cyc (main-clock cycles); without one, the stimulus applies during the event. A value
// list v1,v2,... applies v1 the first time, v2 the second, and so on (an empty item: nothing that
// time); {<n>} in a value is the n-th number of the last log message a point matched (counting
// from 0), as a bench sequence reads printed_arg. The keys are the models' own
// (Eg2System::stim_handlers, e.g. sensor_ctrl.ast_event in eg2_aon.h), and sym.<symbol>, which
// writes a symbol of the test image as the bench's sw_symbol_backdoor_overwrite does (its ELF size,
// 1, 2 or 4 bytes); each application prints "eg2sim: stimulus ...".
// The debugger outside the chip is driven the same way, through rv_dm (debugger.* keys, eg2_rv_dm.h:
// halt, abstract register access, program buffer, resume, system bus access, ndmreset, an SRAM
// program loaded and started).
//
// --periph-debug <level>[,stderr]: each peripheral model's readable debug lines for software
// engineers in <log-dir>/<instance>.debug.log (configuration written, transfers started and
// finished, FIFO overflows and underflows, interrupts raised). Level 1, the default, is one line
// per software action or transfer; 2 adds every byte and FIFO access; 0 writes none. With
// ",stderr" the lines also go to stderr.
//
// --fault injects a fault as an input event, once (the model's behaviour is never changed: the
// fault is what the chip would see): `lockstep`, the core's dual-core lockstep comparator reports a
// mismatch (rv_core_ibex's fatal_hw_err, the core halted); `cheriot_ena_low`, the core's CHERIoT
// enable forced to MuBi4False from then on while the rest of the chip sees the locked mode. <when>
// is an instruction count (decimal: before that instruction), a PC (0x...: the first time the core
// is about to execute it) or a symbol of the test image (its address as a PC); without it, the fault
// is injected before the first instruction. The run prints `eg2sim: fault <name> applied at ...` when
// the fault changes what the chip does, and at the end `eg2sim: fault <name> never applied` for a
// fault that did not (cheriot_ena_low in a run that never locks CHERIoT mode).
//
// At the end the run reports its verdict or why it stopped, the first access to an offset of a block
// with no register there, and the registers hardware updates that software read most: what to model
// next.
//
// Exit: 0 the test passed (DV status 0x900d), 3 it failed (0xbaad), 2 stopped by the model (software
// polls a register no model updates, a feature not modelled yet), 4 instruction limit reached, 1 an
// error.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <systemc>

#include "eg2_image.h"
#include "eg2_stimulus.h"

#include "eg2_bench.h"
#include "eg2_bench_seq.h"
#include "eg2_dvlog.h"
#include "eg2_ip_model.h"
#include "eg2_regblock.h"
#include "eg2_router.h"
#include "eg2_spike_cpu.h"
#ifdef EG2_MPACT
#include "eg2_mpact_cpu.h"
#endif
#include "eg2_targets.h"
#include "ip/eg2_crypto_entropy.h"
#include "ip/eg2_jtag_host.h"
#include "ip/eg2_test_hosts.h"
#include "ip/eg2_periph_stim.h"
#include "sw/device/lib/testing/eg2/eg2_tables.h"

namespace {

struct Window {
  const char *name;
  uint32_t base;
  uint32_t size;
};

// Every register window and memory of top_earlgrey.gen.hjson.
const Window kWindows[] = {
#define EG2_WINDOW(name, base, size) {name, uint32_t(base), uint32_t(size)},
    EG2_WINDOW_TABLE(EG2_WINDOW)
#undef EG2_WINDOW
};

// The generated register block of a window, or nullptr: the window is a memory.
const eg2_regs::Block *reg_block(const std::string &name) {
  for (const eg2_regs::Block &b : eg2_regs::kBlocks) {
    if (name == b.window) return &b;
  }
  return nullptr;
}

// rv_core_ibex's configuration in Earl Grey 2 (top_earlgrey.hjson; PMP reset values from
// hw/top_earlgrey/rtl/ibex_pmp_reset_pkg.sv).
Eg2CoreConfig earlgrey_core() {
  Eg2CoreConfig c{};
  c.isa = "rv32imc_Zba_Zbb_Zbc_Zbs_Zbkb_Zbkx_Zcb_Zcmp_Zihpm";  // RV32BFull, RV32ZcaZcbZcmp
  c.boot_addr = TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR;
  c.pmp_num_regions = 16;
  c.pmp_granularity = 0;
  c.mhpm_counter_num = 2;
  c.secure_ibex = true;
  c.icache = true;
  c.dm_start = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR;
  c.dm_end = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR + TOP_EARLGREY_RV_DM_MEM_SIZE_BYTES - 1;
  // HaltAddress, the debug ROM's entry (PULP debug-system.md "Debug Memory Map": 0x800).
  c.dm_halt_addr = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR + 0x800;
  // Region 2: ROM, NAPOT, L R X; 11: MMIO [region 10, region 11), TOR, L R W; 13: DV ROM, NAPOT,
  // L R W X. pmpcfg byte {L, 00, A, X, W, R}; pmpaddr the byte address >> 2.
  c.pmpcfg_rst[2] = 0x9d;
  c.pmpaddr_rst[2] = 0x000403fc >> 2;
  c.pmpaddr_rst[10] = 0x40000000 >> 2;
  c.pmpcfg_rst[11] = 0x8b;
  c.pmpaddr_rst[11] = 0x42010000 >> 2;
  c.pmpcfg_rst[13] = 0x9f;
  c.pmpaddr_rst[13] = 0x000107fc >> 2;
  c.mseccfg_rst = 0x6;  // RLB, MMWP
  c.clock_hz = 100000000;  // kClockFreqCpuHz, device_sim_dv.c
  return c;
}

}  // namespace

#ifdef EG2_MPACT
int rtos_soc_main(int argc, char **argv,
                  bool (*load_elf)(const std::string &, std::vector<Eg2Memory *> &));
#endif

int sc_main(int argc, char **argv) {
  using namespace eg2_sim;
  // The board: Earl Grey 2 unless --board names another.
  for (int i = 1; i + 1 < argc; ++i) {
    std::string b = argv[i + 1];
    if (std::string(argv[i]) != "--board" || b == "earlgrey") continue;
#ifdef EG2_MPACT
    if (b == "rtos_soc") {
      return rtos_soc_main(argc, argv, [](const std::string &p, std::vector<Eg2Memory *> &m) {
        return load_elf(p, m);
      });
    }
#endif
    std::fprintf(stderr, "eg2sim: --board %s: earlgrey%s\n", b.c_str(),
#ifdef EG2_MPACT
                 " or rtos_soc"
#else
                 " (rtos_soc needs an eg2sim built with EG2_MPACT)"
#endif
    );
    return 1;
  }
  std::string rom, test, otp, otp_out, spike_log, rram_image;
  std::vector<std::string> sets, params, syms_set, faults;
  std::vector<TrigStim> trig_stims;
  std::string seed, cheriot_iss = "mpact", rng = "model";
  uint64_t max_insns = 10000000;
  Eg2System &sys = Eg2System::get();
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--rom") rom = argv[i + 1];
    else if (a == "--test") test = argv[i + 1];
    else if (a == "--rram-image") rram_image = argv[i + 1];
    else if (a == "--otp") otp = argv[i + 1];
    else if (a == "--otp-out") otp_out = argv[i + 1];
    else if (a == "--set") sets.push_back(argv[i + 1]);
    else if (a == "--seed") seed = argv[i + 1];
    else if (a == "--param") params.push_back(argv[i + 1]);
    else if (a == "--log-dir") sys.log_dir = argv[i + 1];
    else if (a == "--max-insns") max_insns = std::stoull(argv[i + 1]);
    else if (a == "--spike-log") spike_log = argv[i + 1];
    else if (a == "--sym") syms_set.push_back(argv[i + 1]);
    else if (a == "--cheriot-iss") cheriot_iss = argv[i + 1];
    else if (a == "--fault") faults.push_back(argv[i + 1]);
    else if (a == "--rng") rng = argv[i + 1];
    else if (a == "--periph-debug") {
      // <level>[,stderr]: the peripheral models' debug lines (Eg2System::debug).
      std::string v = argv[i + 1];
      sys.periph_debug = std::atoi(v.c_str());
      sys.periph_debug_stderr = v.find(",stderr") != std::string::npos;
    } else if (a == "--board") {
      // earlgrey: this platform
    }
    else if (a == "--stim" && [&] {
               // A stimulus with a trigger (...@log:<message>[+<delay>], ...@<delay>): TrigStim.
               TrigStim t;
               std::string err;
               if (!parse_trig_stim(argv[i + 1], &t, &err)) return false;
               if (!err.empty()) {
                 std::fprintf(stderr, "eg2sim: --stim %s: %s\n", argv[i + 1], err.c_str());
                 std::exit(1);
               }
               trig_stims.push_back(t);
               return true;
             }()) {
    } else if (a == "--stim" && eg2_model::Eg2PeriphStim::add(argv[i + 1])) {
      // External stimulus for the peripheral models (ip/eg2_periph_stim.h).
    } else {
      std::fprintf(stderr, "eg2sim: unknown option %s\n", a.c_str());
      return 1;
    }
  }
  if (rom.empty()) {
    std::fprintf(stderr, "usage: eg2sim --rom <rom.elf> [--test <test.elf> | --rram-image <img>] [--otp <vmem>] "
                         "[--otp-out <vmem>] [--seed N] [--param name=value] [--log-dir <dir>] "
                         "[--max-insns N] [--spike-log <file>] [--set win.REG=value] "
                         "[--stim key=value] [--sym name=value[:bytes]] "
                         "[--cheriot-iss mpact|none] [--fault name[@count|@0xpc|@symbol]] "
                         "[--rng model|host|file:<path>|pattern:<name>[,seed=N][,record=<path>]] "
                         "[--periph-debug 0|1|2[,stderr]]\n");
    return 1;
  }
  // The noise source of entropy_src (dv/eg2_rng/eg2_rng_source.h): `model` keeps the model's own
  // generator.
  static eg2_rng_t noise;
  if (rng != "model") {
    char err[256] = {0};
    if (eg2_rng_parse(&noise, rng.c_str(), err, sizeof(err)) != 0) {
      std::fprintf(stderr, "eg2sim: --rng %s: %s\n", rng.c_str(), err);
      return 1;
    }
    eg2_model::eg2_noise_source() = &noise;
    std::printf("eg2sim: noise source %s%s\n", rng.c_str(),
                noise.kind == kEg2RngHost ? (noise.use_rdrand ? " (RDRAND)" : " (getrandom)") : "");
  }
  if (cheriot_iss != "mpact" && cheriot_iss != "none") {
    std::fprintf(stderr, "eg2sim: --cheriot-iss %s: mpact or none\n", cheriot_iss.c_str());
    return 1;
  }
  // Before the peripheral models read their stimulus.
  if (!expand_stim(test)) return 1;

  std::vector<Eg2Router::Window> windows;
  for (const Window &w : kWindows) windows.push_back({w.name, w.base, w.size});
  Eg2Router bus("bus", windows);
  Eg2SpikeCpu cpu("cpu", earlgrey_core(), max_insns, spike_log);
  cpu.socket.bind(bus.socket);
  // The CPU switch at the CHERIoT lock: MPACT-CHERIoT on the core's bus.
  std::unique_ptr<Eg2Iss> cheriot;
#ifdef EG2_MPACT
  if (cheriot_iss == "mpact") {
    cheriot = std::make_unique<Eg2MpactCpu>(
        [&cpu](tlm::tlm_command c, uint32_t a, size_t n, uint8_t *b) {
          return cpu.transport(c, a, n, b);
        });
    cpu.set_cheriot_iss(cheriot.get());
  }
#endif
  std::vector<std::unique_ptr<sc_core::sc_module>> targets;
  std::vector<Eg2Memory *> mems;
  std::vector<Eg2RegBlock *> blocks;
  for (size_t i = 0; i < windows.size(); ++i) {
    const Window &w = kWindows[i];
    auto &to = bus.to(i);
    std::string inst = std::string("t_") + w.name;
    for (char &ch : inst) if (ch == '.') ch = '_';
    if (const eg2_regs::Block *b = reg_block(w.name)) {
      Eg2IpModel *r = make_ip_model(inst.c_str(), *b, w.base);
      to.bind(r->socket);
      blocks.push_back(r);
      targets.emplace_back(r);
    } else {
      auto *m = new Eg2Memory(inst.c_str(), w.base, w.size);
      to.bind(m->socket);
      mems.push_back(m);
      sys.memories[w.name] = m;
      targets.emplace_back(m);
    }
  }
  // The revocation bitmap window is the base of the meta SRAM (Eg2Memory::share_start_of).
  auto revbm = sys.memories.find("cheriot_mem_sys.revbm");
  auto meta = sys.memories.find("sram_ctrl_meta.ram");
  if (revbm != sys.memories.end() && meta != sys.memories.end()) {
    revbm->second->share_start_of(*meta->second);
  }
  if (revbm != sys.memories.end()) revbm->second->set_word_only();
  // rram_ctrl's host window: read-only, and its OTP region (pages 4090 to 4095) answers every access
  // with an error (Eg2Memory::set_read_only / set_error_range; rram_ctrl README.md "Features",
  // "RRAM Memory Overview"). The core models ask Eg2System::bus_refuses before a direct-pointer store.
  auto host = sys.memories.find("rram_ctrl.host");
  if (host != sys.memories.end()) {
    host->second->set_read_only();
    host->second->set_error_range(kRramOtpFirstPage * kRramPageBytes,
                                  kRramOtpLastPage1 * kRramPageBytes);
  }
  sys.bus_refuses = [&sys](uint32_t addr, bool write) {
    for (auto &m : sys.memories) {
      Eg2Memory *mem = m.second;
      if (addr >= mem->base() && addr - mem->base() < mem->size()) {
        return mem->refuses(addr - mem->base(), write);
      }
    }
    return false;
  };
  std::map<std::string, uint32_t> syms;
  // With --rram-image the test ELF gives its symbols and log strings only: its segments would
  // overwrite the signed image.
  if (!load_elf(rom, mems) ||
      (!test.empty() && !load_elf(test, mems, &syms, rram_image.empty())))
    return 1;
  if (!rram_image.empty()) {
    auto r = sys.memories.find("rram_ctrl.host");
    if (!load_rram_image(rram_image, r == sys.memories.end() ? nullptr : r->second)) return 1;
  }
  if (!otp.empty() && !load_otp(otp, blocks)) return 1;
  // Some chip sequences end a software test after their own checks. This only accepts "pass";
  // model-side bench checks registered in verdict_checks still run before the verdict is accepted.
  sys.stim_handlers["bench.finish"] = [&sys](const std::string &v) {
    if (v != "pass") return false;
    sys.finish(true, "the chip-bench stimulus completed its checks");
    return true;
  };
  if (!seed.empty() && !write_symbol(syms, mems, "kEg2Seed", uint32_t(std::stoul(seed, nullptr, 0))))
    return 1;
  for (const std::string &p : params) {
    size_t eq = p.find('=');
    if (eq == std::string::npos ||
        !write_symbol(syms, mems, "kEg2Param_" + p.substr(0, eq),
                      uint32_t(std::stoul(p.substr(eq + 1), nullptr, 0))))
      return 1;
  }
  // --sym <name>=<value>[:<bytes>]: a test-image symbol overwritten before the run, as the chip
  // bench's sw_symbol_backdoor_overwrite does (1, 2 or 4 bytes, default 4).
  for (const std::string &sv : syms_set) {
    size_t eq = sv.find('='), colon = sv.find(':', eq == std::string::npos ? 0 : eq);
    auto it = eq == std::string::npos ? syms.end() : syms.find(sv.substr(0, eq));
    unsigned len = colon == std::string::npos ? 4 : unsigned(std::stoul(sv.substr(colon + 1)));
    if (it == syms.end() || !(len == 1 || len == 2 || len == 4)) {
      std::fprintf(stderr, "eg2sim: --sym %s: no such symbol, or a size other than 1, 2, 4\n",
                   sv.c_str());
      return 1;
    }
    uint32_t v = uint32_t(std::stoul(sv.substr(eq + 1, colon - eq - 1), nullptr, 0));
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    uint32_t addr = elf_load_address(test, it->second);
    bool done = false;
    for (Eg2Memory *m : mems) done = done || m->write(addr, b, len);
    if (!done) {
      std::fprintf(stderr, "eg2sim: --sym %s: the symbol is in no memory\n", sv.c_str());
      return 1;
    }
    std::printf("eg2sim: %s = 0x%x (%u bytes) at 0x%08x\n", it->first.c_str(), v, len, addr);
  }
  // The test image's symbols while the model runs (Eg2System::read_test_symbol /
  // write_test_symbol), for the models' bench-style stimulus.
  sys.read_test_symbol = [&syms, &mems, test](const std::string &name, uint8_t *out, size_t n) {
    auto it = syms.find(name);
    if (it == syms.end()) return false;
    uint32_t addr = elf_load_address(test, it->second);
    for (Eg2Memory *m : mems) {
      if (m->read(addr, out, n)) return true;
    }
    return false;
  };
  sys.test_symbol_addr = [&syms](const std::string &name, uint32_t *addr) {
    auto it = syms.find(name);
    if (it == syms.end()) return false;
    *addr = it->second;
    return true;
  };
  sys.mem_rw = [&mems](bool write, uint32_t addr, uint8_t *b, size_t n) {
    for (Eg2Memory *m : mems) {
      if (write ? m->write(addr, b, n) : m->read(addr, b, n)) return true;
    }
    return false;
  };
  sys.write_test_symbol = [&syms, &mems, &sys, test](const std::string &name, const uint8_t *in,
                                                     size_t n) {
    auto it = syms.find(name);
    if (it == syms.end()) return false;
    uint32_t addr = elf_load_address(test, it->second);
    for (Eg2Memory *m : mems) {
      if (m->write(addr, in, n)) {
        if (name == "kDestState" && n == 1) {
          sys.has_lc_destination = true;
          sys.lc_destination = in[0];
        }
        std::printf("eg2sim: %s backdoor-written (%zu bytes) at 0x%08x\n", name.c_str(), n, addr);
        return true;
      }
    }
    return false;
  };
  // The chip bench's steps (EG2 BENCH lines), done by the model (eg2_bench.h).
  static Eg2Bench bench(sys);
  // The FPGA/silicon host programs a host test runs against the software (host.program,
  // ip/eg2_test_hosts.h).
  static eg2_model::Eg2TestHosts test_hosts(sys);
  // The fault and backdoor steps of OpenTitan's chip-test sequences, as stimulus keys
  // (eg2_bench_seq.h; bench.txt gives them per test).
  static Eg2BenchSeq bench_seq(sys);
  // The debugger on a host over JTAG: TAP selection and the RISC-V DTM in front of rv_dm
  // (jtag.* keys, ip/eg2_jtag_host.h).
  for (Eg2IpModel *m : sys.models()) {
    if (auto *dm = dynamic_cast<eg2_model::Eg2RvDm *>(m)) {
      static eg2_model::Eg2JtagHost jtag_host(*dm);
      break;
    }
  }
  // A chip-bench sequence reloads the software image after an RMA wipe. This input repeats that
  // host action; the lifecycle model itself only requests the documented wipe.
  sys.stim_handlers["test.reload"] = [&test, &mems, &sys](const std::string &v) {
    bool loaded = !test.empty() && std::stoul(v, nullptr, 0) != 0 && load_elf(test, mems);
    if (loaded && sys.has_lc_destination && sys.write_test_symbol) {
      loaded = sys.write_test_symbol("kDestState", &sys.lc_destination, 1);
    }
    return loaded;
  };
  // rv_dm's system bus access host: on the bus the core's data side reaches, without the core's
  // PMP or address translation (rv_dm doc interfaces.md "TL-UL host for System Bus Access").
  sys.sba = [&cpu](bool write, uint32_t addr, uint32_t len, uint8_t *bytes) {
    return cpu.transport(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND, addr, len, bytes);
  };
  for (const std::string &set : sets) {
    size_t eq = set.find('='), dot = set.rfind('.', eq);
    bool done = false;
    if (eq != std::string::npos && dot != std::string::npos) {
      std::string win = set.substr(0, dot), reg = set.substr(dot + 1, eq - dot - 1);
      for (Eg2RegBlock *b : blocks) {
        int r = std::string(b->desc().window) == win ? b->reg_index(reg) : -1;
        if (r >= 0) {
          b->set_value(uint16_t(r), uint32_t(std::stoul(set.substr(eq + 1), nullptr, 0)));
          done = true;
        }
      }
    }
    if (!done) {
      std::fprintf(stderr, "eg2sim: --set %s: no such register\n", set.c_str());
      return 1;
    }
  }

  // --fault <name>[@<count>|@0x<pc>|@<symbol>]
  for (const std::string &fs : faults) {
    size_t at = fs.find('@');
    std::string name = fs.substr(0, at), when = at == std::string::npos ? "" : fs.substr(at + 1);
    if (!sys.fault_handlers.count(name)) {
      std::fprintf(stderr, "eg2sim: --fault %s: no such fault (", fs.c_str());
      for (auto &h : sys.fault_handlers) std::fprintf(stderr, " %s", h.first.c_str());
      std::fprintf(stderr, " )\n");
      return 1;
    }
    Eg2SpikeCpu::FaultWhen kind = Eg2SpikeCpu::kAtStart;
    uint64_t value = 0;
    if (when.rfind("0x", 0) == 0) {
      kind = Eg2SpikeCpu::kAtPc;
      value = std::stoull(when, nullptr, 16);
    } else if (!when.empty() && when.find_first_not_of("0123456789") == std::string::npos) {
      kind = Eg2SpikeCpu::kAtCount;
      value = std::stoull(when);
    } else if (!when.empty()) {
      auto it = syms.find(when);
      if (it == syms.end()) {
        std::fprintf(stderr, "eg2sim: --fault %s: the test image has no symbol %s\n", fs.c_str(),
                     when.c_str());
        return 1;
      }
      kind = Eg2SpikeCpu::kAtPc;
      value = it->second & ~1u;
    }
    cpu.add_fault(name, kind, value);
    char b[160];
    if (kind == Eg2SpikeCpu::kAtStart) {
      std::snprintf(b, sizeof b, "before the first instruction");
    } else if (kind == Eg2SpikeCpu::kAtCount) {
      std::snprintf(b, sizeof b, "before instruction %llu", (unsigned long long)value);
    } else {
      std::snprintf(b, sizeof b, "at pc 0x%08x%s%s%s", uint32_t(value),
                    when.rfind("0x", 0) == 0 ? "" : " (", when.rfind("0x", 0) == 0 ? "" : when.c_str(),
                    when.rfind("0x", 0) == 0 ? "" : ")");
    }
    std::printf("eg2sim: fault %s requested %s\n", name.c_str(), b);
  }

  // The software's DV log, decoded from both images (eg2_dvlog.h).
  Eg2DvLog dvlog;
  dvlog.add_image(rom);
  if (!test.empty()) dvlog.add_image(test);
  dvlog.log_path = sys.log_dir + "/sw.model.log";
  sys.on_dv_log = [&dvlog](uint32_t w) { dvlog.feed(w); };

  // sym.<symbol>: the test image's symbol written while the model runs, as the bench's
  // sw_symbol_backdoor_overwrite writes it (Eg2System::write_test_symbol; little-endian, the
  // symbol's ELF size).
  for (const TrigStim &t : trig_stims) {
    if (t.key.rfind("sym.", 0) != 0 || sys.stim_handlers.count(t.key)) continue;
    std::string name = t.key.substr(4);
    uint32_t len = test.empty() || !syms.count(name) ? 0 : elf_symbol_size(test, name);
    if (!(len == 1 || len == 2 || len == 4)) {
      std::fprintf(stderr, "eg2sim: --stim %s: no such symbol, or a size other than 1, 2, 4\n",
                   t.arg.c_str());
      return 1;
    }
    sys.stim_handlers[t.key] = [&sys, name, len](const std::string &v) {
      uint32_t x = uint32_t(std::stoul(v, nullptr, 0));
      uint8_t b[4] = {uint8_t(x), uint8_t(x >> 8), uint8_t(x >> 16), uint8_t(x >> 24)};
      return sys.write_test_symbol(name, b, len);
    };
  }
  // Triggered stimulus (TrigStim): every key must have a model that takes it.
  for (const TrigStim &t : trig_stims) {
    if (!sys.stim_handlers.count(t.key)) {
      std::fprintf(stderr, "eg2sim: --stim %s: no model takes this key while running\n",
                   t.key.c_str());
      return 1;
    }
  }
  // Applies value v of t at its delay after `from` (picoseconds), as an input event at that time;
  // with no delay, at once (during the event that triggered it).
  auto fire = [&sys](const TrigStim &t, const std::string &v, uint64_t from) {
    auto apply = [&sys, key = t.key, v] {
      bool ok = sys.apply_stim(key, v);
      std::printf("eg2sim: stimulus %s=%s at %.3f us%s\n", key.c_str(), v.c_str(),
                  double(sys.now_ps()) / 1e6, ok ? "" : ": the model did not take the value");
    };
    if (!t.ps && !t.cycles) return apply();
    uint64_t when = from + t.ps;
    if (t.cycles) {
      uint64_t c = sys.clock_cycles(Eg2System::Clk::kMain, from) + t.cycles;
      when = sys.clock_time_of_cycle(Eg2System::Clk::kMain, c);
    }
    sys.at(when, apply);
  };
  // The value for this time: the next of a list (empty: nothing), with {<n>} replaced by the n-th
  // number of the last log message a point matched.
  auto value_now = [](TrigStim &t, std::string *v) {
    size_t i = t.times++;
    if (t.values.size() > 1 && i >= t.values.size()) return false;
    *v = t.values.size() > 1 ? t.values[i] : t.values[0];
    for (size_t o; (o = v->find('{')) != std::string::npos;) {
      size_t c = v->find('}', o);
      if (c == std::string::npos) break;
      size_t n = std::stoul(v->substr(o + 1, c - o - 1));
      v->replace(o, c - o + 1, n < t.nums.size() ? t.nums[n] : "0");
    }
    return !v->empty();
  };
  for (TrigStim &t : trig_stims) {
    std::string v;
    if (t.points.empty() && value_now(t, &v)) fire(t, v, 0);
  }
  // A point happened: each stimulus waiting for it moves on, and applies after its last point.
  auto point = [&trig_stims, &sys, fire, value_now](TrigStim::Point::Kind kind,
                                                    const std::string &text, uint32_t status) {
    for (TrigStim &t : trig_stims) {
      if (t.points.empty()) continue;
      const TrigStim::Point &p = t.points[t.next];
      bool hit = p.kind == TrigStim::Point::kStatus  ? kind == p.kind && status == p.status
                 : p.kind == TrigStim::Point::kLogPrefix ? kind == TrigStim::Point::kLog &&
                                                               text.rfind(p.text, 0) == 0
                                                         : kind == p.kind && text == p.text;
      if (!hit) continue;
      if (kind == TrigStim::Point::kLog) {
        // The numbers of the message (decimal, or hex with 0x), as printed_arg gives them.
        t.nums.clear();
        for (size_t i = 0; i < text.size();) {
          if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            continue;
          }
          size_t j = i;
          if (text.compare(i, 2, "0x") == 0) j += 2;
          while (j < text.size() && std::isxdigit(static_cast<unsigned char>(text[j]))) ++j;
          t.nums.push_back(text.substr(i, j - i));
          i = j;
        }
      }
      if (++t.next < t.points.size()) continue;
      t.next = 0;
      std::string v;
      if (value_now(t, &v)) fire(t, v, sys.now_ps());
    }
  };
  dvlog.on_msg = [point, &sys](const std::string &msg) {
    point(TrigStim::Point::kLog, msg, 0);
    for (auto &w : sys.sw_log_watchers) w(msg);
  };
  sys.on_dv_status = [point, &sys](uint32_t w) {
    point(TrigStim::Point::kStatus, "", w);
    for (auto &f : sys.sw_status_watchers) f(w);
  };
  sys.on_chip_event = [point](const std::string &e) { point(TrigStim::Point::kEvent, e, 0); };
  // The core asks pwrmgr at each WFI it sleeps on: that is the core's sleep (core_sleep_o).
  if (auto lpr = sys.low_power_requested) {
    sys.low_power_requested = [&sys, lpr] {
      sys.chip_event("wfi");
      return lpr();
    };
  }

  std::printf("eg2sim: %zu windows (%zu IP models, %zu memories), Spike at 0x%08x\n",
              bus.windows().size(), blocks.size(), mems.size(), cpu.pc());
  sc_core::sc_start();
  std::fflush(stdout);
  bool model_stop = !Eg2ModelStop::reason().empty();
  const std::string &why = sys.finished() ? sys.verdict()
                           : model_stop   ? Eg2ModelStop::reason()
                                          : cpu.stop_reason();
  std::printf("eg2sim: %s after %llu instructions, %.3f us, %u resets, pc 0x%08x\n", why.c_str(),
              (unsigned long long)cpu.retired(), double(cpu.now_ps()) / 1e6, cpu.resets(),
              cpu.pc());
  if (cpu.cheriot()) std::printf("eg2sim: ended in CHERIoT mode (%s)\n", cheriot->name());
  if (sys.core_halted()) std::printf("eg2sim: ended with the core halted: %s\n",
                                     sys.halt_reason().c_str());
  for (const std::string &fs : faults) {
    std::string name = fs.substr(0, fs.find('@'));
    const auto &ap = sys.faults_applied();
    if (std::find(ap.begin(), ap.end(), name) == ap.end()) {
      std::printf("eg2sim: fault %s never applied\n", name.c_str());
    }
  }
  for (Eg2RegBlock *b : blocks) {
    if (!b->first_error().empty()) {
      std::printf("eg2sim: first access with no register: %s\n", b->first_error().c_str());
      break;
    }
  }
  std::vector<std::pair<std::string, uint64_t>> reads;
  for (Eg2RegBlock *b : blocks) {
    for (auto &r : b->hw_reads()) reads.push_back(r);
  }
  std::sort(reads.begin(), reads.end(), [](auto &a, auto &b) { return a.second > b.second; });
  for (size_t i = 0; i < reads.size() && i < 8; ++i) {
    std::printf("eg2sim:   %8llu reads of %s (hardware updates it, not modelled)\n",
                (unsigned long long)reads[i].second, reads[i].first.c_str());
  }
  if (eg2_model::eg2_noise_source()) {
    std::printf("eg2sim: noise source %s: %llu symbols, the first 64: %08x %08x %08x %08x %08x "
                "%08x %08x %08x\n", noise.desc, (unsigned long long)noise.n,
                noise.first_words[0], noise.first_words[1], noise.first_words[2],
                noise.first_words[3], noise.first_words[4], noise.first_words[5],
                noise.first_words[6], noise.first_words[7]);
    eg2_rng_close(&noise);
  }
  if (!otp_out.empty() && !save_otp(otp_out, blocks)) return 1;
  dvlog.close();
  targets.clear();  // flush the UARTs' partial lines
  sys.close_logs();
  if (sys.finished()) return sys.passed() ? 0 : 3;
  if (model_stop) return 2;
  return cpu.stop_reason() == "instruction limit reached" ? 4 : 2;
}
