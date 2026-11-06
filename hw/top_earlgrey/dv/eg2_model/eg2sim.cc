// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// eg2sim: the EG2 SystemC model run on its own (doc/system_model in eg2_top_verification).
//
//   eg2sim --rom <rom.elf> --test <test.elf> [--otp <image.24.vmem>] [--max-insns N]
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

// Copy every loadable segment of a 32-bit ELF to its load address (p_paddr); collect its symbols.
bool load_elf(const std::string &path, std::vector<Eg2Memory *> &mems,
              std::map<std::string, uint32_t> *symbols = nullptr) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "eg2sim: cannot open %s\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd32 = [&](size_t off) {
    uint32_t v = 0;
    if (off + 4 <= elf.size()) std::memcpy(&v, &elf[off], 4);
    return v;
  };
  auto rd16 = [&](size_t off) {
    uint16_t v = 0;
    if (off + 2 <= elf.size()) std::memcpy(&v, &elf[off], 2);
    return v;
  };
  if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1) {
    std::fprintf(stderr, "eg2sim: %s is not a 32-bit ELF\n", path.c_str());
    return false;
  }
  uint32_t phoff = rd32(28);
  uint16_t phentsize = rd16(42), phnum = rd16(44);
  if (symbols) {
    uint32_t shoff = rd32(32);
    uint16_t shentsize = rd16(46), shnum = rd16(48);
    for (uint16_t i = 0; i < shnum; ++i) {
      size_t sh = shoff + size_t(i) * shentsize;
      if (rd32(sh + 4) != 2) continue;  // SHT_SYMTAB
      uint32_t off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24);
      size_t strsh = shoff + size_t(link) * shentsize;
      uint32_t stroff = rd32(strsh + 16), strsize = rd32(strsh + 20);
      for (uint32_t e = 0; e + 16 <= size; e += 16) {
        uint32_t name = rd32(off + e), value = rd32(off + e + 4);
        if (name == 0 || name >= strsize || stroff + name >= elf.size()) continue;
        const char *n = reinterpret_cast<const char *>(&elf[stroff + name]);
        size_t len = strnlen(n, elf.size() - (stroff + name));
        (*symbols)[std::string(n, len)] = value;
      }
    }
  }
  for (uint16_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd32(ph), offset = rd32(ph + 4), paddr = rd32(ph + 12),
             filesz = rd32(ph + 16);
    if (type != 1 || filesz == 0) continue;
    if (size_t(offset) + filesz > elf.size()) return false;
    bool done = false;
    for (Eg2Memory *m : mems) done = done || m->write(paddr, &elf[offset], filesz);
    if (!done) {
      std::fprintf(stderr, "eg2sim: %s: segment at 0x%08x is in no memory\n", path.c_str(),
                   paddr);
      return false;
    }
  }
  return true;
}

// Load an OTP image (vmem, "@<word address> <24-bit word>": data in bits 15:0) into otp_ctrl's
// software read window.
bool load_otp(const std::string &path, std::vector<Eg2RegBlock *> &blocks) {
  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "eg2sim: cannot open %s\n", path.c_str());
    return false;
  }
  Eg2RegBlock *otp = nullptr;
  for (Eg2RegBlock *b : blocks) {
    if (std::string(b->desc().window) == "otp_ctrl.core") otp = b;
  }
  if (!otp) return false;
  std::string line;
  size_t words = 0;
  while (std::getline(f, line)) {
    unsigned addr, word;
    if (std::sscanf(line.c_str(), "@%x %x", &addr, &word) != 2) continue;
    uint8_t b[2] = {uint8_t(word), uint8_t(word >> 8)};
    if (otp->win_write("SW_CFG_WINDOW", 2 * addr, b, 2)) ++words;
  }
  std::printf("eg2sim: OTP image %s: %zu words in otp_ctrl.core SW_CFG_WINDOW\n", path.c_str(),
              words);
  return words > 0;
}

// --otp-out: the OTP array as it is at the end of the run, in the image format --otp reads
// (gen-otp-img.py: one 16-bit word per line with its 6 ECC bits, OTP's extended Hamming code of
// hw/ip/lc_ctrl/data/lc_ctrl_state.hjson "secded"), so that a life-cycle walkthrough can go on in
// the next run, and the chip bench can load it.
uint32_t otp_ecc_word(uint16_t d) {
  static const std::vector<std::vector<int>> kMatrix = {
      {0, 1, 3, 4, 6, 8, 10, 11, 13, 15}, {0, 2, 3, 5, 6, 9, 10, 12, 13},
      {1, 2, 3, 7, 8, 9, 10, 14, 15},     {4, 5, 6, 7, 8, 9, 10},
      {11, 12, 13, 14, 15},               {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                           11, 12, 13, 14, 15, 16, 17, 18, 19, 20}};
  uint32_t w = d;
  for (size_t j = 0; j < kMatrix.size(); ++j) {
    uint32_t b = 0;
    for (int k : kMatrix[j]) b ^= (w >> k) & 1;
    w |= b << (16 + j);
  }
  return w;
}

bool save_otp(const std::string &path, std::vector<Eg2RegBlock *> &blocks) {
  Eg2RegBlock *otp = nullptr;
  for (Eg2RegBlock *b : blocks) {
    if (std::string(b->desc().window) == "otp_ctrl.core") otp = b;
  }
  int w = otp ? otp->win_index("SW_CFG_WINDOW") : -1;
  FILE *f = w >= 0 ? std::fopen(path.c_str(), "w") : nullptr;
  if (!f) {
    std::fprintf(stderr, "eg2sim: cannot write %s\n", path.c_str());
    return false;
  }
  const std::vector<uint8_t> &m = otp->win_mem(uint16_t(w));
  const size_t words = 0x810 / 2;  // the OTP array (otp_ctrl_mmap.md), kEg2OtpSize
  std::fprintf(f, "//\n// OTP MEM file with %zu x 24bit layout (eg2sim --otp-out)\n", words);
  for (size_t i = 0; i < words && 2 * i + 1 < m.size(); ++i) {
    std::fprintf(f, "@%06zx %06x\n", i, otp_ecc_word(uint16_t(m[2 * i] | (m[2 * i + 1] << 8))));
  }
  std::fclose(f);
  std::printf("eg2sim: OTP written to %s\n", path.c_str());
  return true;
}

// The size of a symbol of a 32-bit ELF (st_size), or 0 if there is no such symbol.
uint32_t elf_symbol_size(const std::string &path, const std::string &symbol) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52) return 0;
  uint32_t shoff = rd(32, 4), shentsize = rd(46, 2), shnum = rd(48, 2);
  for (uint32_t i = 0; i < shnum; ++i) {
    size_t sh = shoff + size_t(i) * shentsize;
    if (rd(sh + 4, 4) != 2) continue;  // SHT_SYMTAB
    uint32_t off = rd(sh + 16, 4), size = rd(sh + 20, 4), link = rd(sh + 24, 4);
    uint32_t stroff = rd(shoff + size_t(link) * shentsize + 16, 4);
    for (uint32_t e = 0; e + 16 <= size; e += 16) {
      uint32_t name = rd(off + e, 4);
      if (name == 0 || stroff + name >= elf.size()) continue;
      const char *n = reinterpret_cast<const char *>(&elf[stroff + name]);
      if (symbol == std::string(n, strnlen(n, elf.size() - (stroff + name)))) {
        return rd(off + e + 8, 4);
      }
    }
  }
  return 0;
}

// The initial bytes of a symbol of a 32-bit ELF (its value in the file's loadable segments): false
// if there is no such symbol or its bytes are not in the file.
bool elf_symbol_bytes(const std::string &path, const std::string &symbol, size_t len,
                      std::vector<uint8_t> *out) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52) return false;
  uint32_t shoff = rd(32, 4), shentsize = rd(46, 2), shnum = rd(48, 2), addr = 0;
  bool found = false;
  for (uint32_t i = 0; i < shnum && !found; ++i) {
    size_t sh = shoff + size_t(i) * shentsize;
    if (rd(sh + 4, 4) != 2) continue;  // SHT_SYMTAB
    uint32_t off = rd(sh + 16, 4), size = rd(sh + 20, 4), link = rd(sh + 24, 4);
    uint32_t stroff = rd(shoff + size_t(link) * shentsize + 16, 4);
    for (uint32_t e = 0; e + 16 <= size && !found; e += 16) {
      uint32_t name = rd(off + e, 4);
      if (name == 0 || stroff + name >= elf.size()) continue;
      const char *n = reinterpret_cast<const char *>(&elf[stroff + name]);
      if (symbol == std::string(n, strnlen(n, elf.size() - (stroff + name)))) {
        addr = rd(off + e + 4, 4);
        found = true;
      }
    }
  }
  if (!found) return false;
  uint32_t phoff = rd(28, 4), phentsize = rd(42, 2), phnum = rd(44, 2);
  for (uint32_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd(ph, 4), offset = rd(ph + 4, 4), va = rd(ph + 8, 4), filesz = rd(ph + 16, 4);
    if (type != 1 || addr < va || addr - va + len > filesz) continue;
    size_t at = size_t(offset) + (addr - va);
    if (at + len > elf.size()) return false;
    out->assign(elf.begin() + at, elf.begin() + at + len);
    return true;
  }
  return false;
}

// Stimulus values may name bytes the bench takes from the test image, as the chip bench's
// sequences do (they write data into the image with sw_symbol_backdoor_overwrite and then send the
// same bytes): {sym:<name>:<length>} is the symbol's initial bytes, {rep:<hex byte>:<count>} a byte
// repeated. They become C escapes in a UART's text (uart<N>.*) and hex bytes everywhere else
// (ip/eg2_periph_stim.h). False, with a message, if a symbol is missing.
// A stimulus applied while the model runs, as a chip-bench sequence drives it at a point of the
// test: --stim <key>=<value>@<point>[@<point>...][+<delay>], or --stim <key>=<value>@<delay> once,
// <delay> after the start (the header comment gives the points). The points are what the sequence
// waits for (sw_logger_if printed_log, sw_test_status_if, the core's sleep, pwrmgr_low_power_if),
// in order: the stimulus applies when the last one happens, after <delay> (<n>us, <n>ns, or
// <n>cyc: main-clock cycles, the counter that times it against the chip), and the points start
// again. The keys are the models' (Eg2System::stim_handlers), each documented where its model
// registers it.
struct TrigStim {
  struct Point {
    enum Kind { kLog, kLogPrefix, kStatus, kEvent } kind;
    std::string text;  // the message, its prefix, or the event's name
    uint32_t status = 0;
  };
  std::string key, arg;
  std::vector<std::string> values;  // one: every time; more: the n-th time the n-th
  std::vector<Point> points;        // none: once, from the start
  uint64_t ps = 0, cycles = 0;
  size_t next = 0;   // the point waited for
  size_t times = 0;  // how often the points have all happened
  std::vector<std::string> nums;  // the numbers of the last log message a point matched
};

// The test status words (sw/device/lib/testing/test_framework/status.h), by the name the bench's
// sw_test_status_e gives them without its SwTestStatus prefix.
bool status_word(const std::string &name, uint32_t *w) {
  static const std::map<std::string, uint32_t> kWords = {
      {"InBootRom", 0xb090}, {"InBootRomHalt", 0xb057}, {"InTest", 0x4354},
      {"InWfi", 0x1d1e},     {"Passed", 0x900d},        {"Failed", 0xbaad}};
  auto it = kWords.find(name);
  if (it != kWords.end()) return *w = it->second, true;
  if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) return false;
  *w = uint32_t(std::stoul(name, nullptr, 0));
  return true;
}

// Parses "<key>=<value>@<when>"; false if the argument has no trigger (a plain --stim).
bool parse_trig_stim(const std::string &arg, TrigStim *t, std::string *err) {
  size_t eq = arg.find('=');
  if (eq == std::string::npos) return false;
  size_t at = arg.find('@', eq);
  if (at == std::string::npos || at + 1 >= arg.size()) return false;
  std::string when = arg.substr(at + 1);
  bool from_start = std::isdigit(static_cast<unsigned char>(when[0]));
  static const char *kPoints[] = {"log:", "status:", "event:", "wfi", "lowpower"};
  bool point = false;
  for (const char *p : kPoints) point |= when.rfind(p, 0) == 0;
  if (!from_start && !point) return false;
  t->key = arg.substr(0, eq);
  t->arg = arg;
  std::string v = arg.substr(eq + 1, at - eq - 1);
  for (size_t s = 0;;) {
    size_t c = v.find(',', s);
    t->values.push_back(v.substr(s, c == std::string::npos ? std::string::npos : c - s));
    if (c == std::string::npos) break;
    s = c + 1;
  }
  // The delay: the last "+<n><unit>" of the last point, or the whole of a start trigger.
  std::string delay;
  if (from_start) {
    delay = when;
  } else {
    size_t plus = when.rfind('+');
    if (plus != std::string::npos && plus + 1 < when.size() &&
        std::isdigit(static_cast<unsigned char>(when[plus + 1]))) {
      delay = when.substr(plus + 1);
      when = when.substr(0, plus);
    }
    for (size_t s = 0; s <= when.size();) {
      size_t a = when.find('@', s);
      std::string p = when.substr(s, a == std::string::npos ? std::string::npos : a - s);
      s = a == std::string::npos ? when.size() + 1 : a + 1;
      // %20 is a space (bench.txt splits its lines at spaces).
      for (size_t q; (q = p.find("%20")) != std::string::npos;) p.replace(q, 3, " ");
      TrigStim::Point pt;
      if (p.rfind("log:", 0) == 0) {
        pt.text = p.substr(4);
        pt.kind = TrigStim::Point::kLog;
        if (!pt.text.empty() && pt.text.back() == '*') {
          pt.kind = TrigStim::Point::kLogPrefix;
          pt.text.pop_back();
        }
        if (pt.text.empty()) return *err = "an empty log message", true;
      } else if (p.rfind("status:", 0) == 0) {
        pt.kind = TrigStim::Point::kStatus;
        pt.text = p.substr(7);
        if (!status_word(pt.text, &pt.status)) return *err = "test status '" + pt.text + "'", true;
      } else if (p.rfind("event:", 0) == 0) {
        pt.kind = TrigStim::Point::kEvent;
        pt.text = p.substr(6);
        if (pt.text.empty()) return *err = "an empty event name", true;
      } else if (p == "wfi" || p == "lowpower") {
        pt.kind = TrigStim::Point::kEvent;
        pt.text = p;
      } else {
        return *err = "point '" + p + "' (log:, status:, event:, wfi or lowpower)", true;
      }
      t->points.push_back(pt);
    }
  }
  if (!delay.empty()) {
    size_t n = 0;
    uint64_t v = std::stoull(delay, &n);
    std::string unit = delay.substr(n);
    if (unit == "us") t->ps = v * 1000000ull;
    else if (unit == "ns") t->ps = v * 1000ull;
    else if (unit == "cyc") t->cycles = v;
    else return *err = "delay unit '" + unit + "' (us, ns or cyc)", true;
  }
  return true;
}

bool expand_stim(const std::string &test) {
  for (auto &kv : eg2_model::Eg2PeriphStim::all()) {
    std::string &v = kv.second;
    std::string out;
    bool text = kv.first.rfind("uart", 0) == 0;
    size_t pos = 0;
    while (pos < v.size()) {
      size_t open = v.find('{', pos), close = v.find('}', open);
      if (open == std::string::npos || close == std::string::npos) break;
      out += v.substr(pos, open - pos);
      std::string tok = v.substr(open + 1, close - open - 1);
      size_t c1 = tok.find(':'), c2 = tok.rfind(':');
      std::vector<uint8_t> bytes;
      if (c1 != std::string::npos && c2 > c1) {
        std::string kind = tok.substr(0, c1), arg = tok.substr(c1 + 1, c2 - c1 - 1);
        size_t n = std::stoul(tok.substr(c2 + 1), nullptr, 0);
        if (kind == "rep") bytes.assign(n, uint8_t(std::stoul(arg, nullptr, 16)));
        if (kind == "sym" && !elf_symbol_bytes(test, arg, n, &bytes)) {
          std::fprintf(stderr, "eg2sim: --stim %s: the test image has no %zu bytes at %s\n",
                       kv.first.c_str(), n, arg.c_str());
          return false;
        }
      }
      char hex[8];
      for (uint8_t b : bytes) {
        std::snprintf(hex, sizeof hex, text ? "\\x%02x" : "%02x", b);
        out += hex;
      }
      pos = close + 1;
    }
    v = out + v.substr(pos);
  }
  return true;
}

// The load address of run-time address vaddr in a 32-bit ELF: a symbol of an initialised data
// section (.data) lives at its run-time address only after the start-up code has copied it from
// its load image, so a write before the run must go to the load image (p_paddr). vaddr unchanged if
// no segment with a different load address holds it.
uint32_t elf_load_address(const std::string &path, uint32_t vaddr) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52) return vaddr;
  uint32_t phoff = rd(28, 4), phentsize = rd(42, 2), phnum = rd(44, 2);
  for (uint32_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd(ph, 4), va = rd(ph + 8, 4), pa = rd(ph + 12, 4), filesz = rd(ph + 16, 4);
    if (type == 1 && va != pa && vaddr >= va && vaddr - va < filesz) return pa + (vaddr - va);
  }
  return vaddr;
}

// Write a 32-bit little-endian word at a symbol of the test image, as the bench's
// sw_symbol_backdoor_overwrite does.
bool write_symbol(const std::map<std::string, uint32_t> &syms, std::vector<Eg2Memory *> &mems,
                  const std::string &name, uint32_t value) {
  auto it = syms.find(name);
  if (it == syms.end()) {
    std::fprintf(stderr, "eg2sim: the test image has no symbol %s\n", name.c_str());
    return false;
  }
  uint8_t b[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
  for (Eg2Memory *m : mems) {
    if (m->write(it->second, b, 4)) {
      std::printf("eg2sim: %s = %u (0x%08x) at 0x%08x\n", name.c_str(), value, value, it->second);
      return true;
    }
  }
  std::fprintf(stderr, "eg2sim: %s at 0x%08x is in no memory\n", name.c_str(), it->second);
  return false;
}

}  // namespace

#ifdef EG2_MPACT
int rtos_soc_main(int argc, char **argv,
                  bool (*load_elf)(const std::string &, std::vector<Eg2Memory *> &));
#endif

int sc_main(int argc, char **argv) {
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
  std::string rom, test, otp, otp_out, spike_log;
  std::vector<std::string> sets, params, syms_set, faults;
  std::vector<TrigStim> trig_stims;
  std::string seed, cheriot_iss = "mpact", rng = "model";
  uint64_t max_insns = 10000000;
  Eg2System &sys = Eg2System::get();
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--rom") rom = argv[i + 1];
    else if (a == "--test") test = argv[i + 1];
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
    std::fprintf(stderr, "usage: eg2sim --rom <rom.elf> [--test <test.elf>] [--otp <vmem>] "
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
  std::map<std::string, uint32_t> syms;
  if (!load_elf(rom, mems) || (!test.empty() && !load_elf(test, mems, &syms))) return 1;
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
  dvlog.on_msg = [point](const std::string &msg) { point(TrigStim::Point::kLog, msg, 0); };
  sys.on_dv_status = [point](uint32_t w) { point(TrigStim::Point::kStatus, "", w); };
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
