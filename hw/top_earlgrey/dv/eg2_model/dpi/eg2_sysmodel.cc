// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// libeg2sysmodel.so: the EG2 SystemC model driven by the chip bench (the API: eg2_sysmodel.h).
//
// The platform is eg2sim's (eg2sim.cc): the generated window table, an IP model for every window
// with a register block, a memory for every other. Its initiator is Eg2BusMaster, which the caller
// drives one access at a time, instead of eg2sim's Spike. Nothing runs as a SystemC process: the
// IP models act on accesses and on the events they schedule (Eg2System::schedule), which step()
// runs at the caller's time. SystemC is elaborated once (sc_start of zero time) so the sockets are
// bound; its own time stays 0 and Eg2System's clock is the caller's.
//
// What the model knows of memory: the bytes of the images, of the bench's backdoor writes, of the
// stores it has seen, and of the chip's values of loads it could not predict (load_event). Every
// other byte (SRAM after a reset, which the chip re-scrambles; the parts of the ROM and RRAM no
// image fills) is unknown: a load of it is an input event, not a prediction.

#include "eg2_sysmodel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

#include "eg2_dvlog.h"
#include "eg2_ip_model.h"
#include "eg2_regblock.h"
#include "eg2_router.h"
#include "eg2_targets.h"
#include "ip/eg2_core_system.h"
#include "ip/eg2_periph_stim.h"
#include "sw/device/lib/testing/eg2/eg2_tables.h"

// libsystemc.so's main() refers to sc_main; a process that loads this library has its own main.
int sc_main(int, char **) { return 1; }

namespace {

// Why eg2_sysmodel_create returned null (eg2_sysmodel_create_error): a simulator may not show the
// library's stderr, so the bench prints it.
std::string g_create_error;

void create_error(const char *msg) {
  g_create_error = msg;
  std::fprintf(stderr, "eg2_sysmodel: %s\n", msg);
}

template <typename Arg, typename... Args>
void create_error(const char *fmt, Arg arg, Args... args) {
  char buf[512];
  std::snprintf(buf, sizeof buf, fmt, arg, args...);
  create_error(buf);
}

// The bus initiator the caller drives.
class Eg2BusMaster : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Eg2BusMaster> socket;

  explicit Eg2BusMaster(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}

  // One access; false if the target answers an error.
  bool access(tlm::tlm_command cmd, uint32_t addr, unsigned len, uint8_t *bytes) {
    tlm::tlm_generic_payload trans;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    trans.set_command(cmd);
    trans.set_address(addr);
    trans.set_data_ptr(bytes);
    trans.set_data_length(len);
    trans.set_streaming_width(len);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    socket->b_transport(trans, delay);
    return trans.is_response_ok();
  }
};

// A window of the top and what the model knows of it.
struct Window {
  std::string name;
  uint32_t base, size;
  Eg2Memory *mem = nullptr;     // a memory, or
  Eg2RegBlock *blk = nullptr;   // a register block
  std::vector<bool> known;      // memory: the bytes the model knows
  std::map<uint32_t, uint8_t> reg_flags;  // register block: each register's flags, by offset
};

// The memories the chip loses on a reset: the ROM re-scrambles main SRAM at every boot; the
// retention SRAM keeps its contents through every reset but a power-on.
bool lost_on_reset(const std::string &window, bool por) {
  if (window == "sram_ctrl_ret.ram") return por;
  return window == "sram_ctrl_main.ram" || window == "sram_ctrl_sec.ram" ||
         window == "sram_ctrl_meta.ram";
}

struct Model {
  std::unique_ptr<Eg2Router> bus;
  std::unique_ptr<Eg2BusMaster> master;
  std::vector<std::unique_ptr<sc_core::sc_module>> targets;
  std::vector<Window> windows;
  std::map<std::string, std::deque<std::string>> out;
  Eg2DvLog dvlog;
  uint64_t now = 0;
  bool reset_seen = false;
  bool released = false;
  bool ena_low = false;  // fault cheriot_ena_low injected
  std::string stop, verdict, pulled, halted;

  Window *find(uint32_t addr) {
    for (Window &w : windows) {
      if (addr >= w.base && addr - w.base < w.size) return &w;
    }
    return nullptr;
  }

  void mark(uint32_t addr, uint32_t len, bool known) {
    for (uint32_t i = 0; i < len; ++i) {
      Window *w = find(addr + i);
      if (w && w->mem) w->known[addr + i - w->base] = known;
    }
  }

  // Whether the model predicts a load's value (see eg2_sysmodel.h).
  bool predicts(uint32_t addr, uint32_t len) {
    Window *w = find(addr);
    if (!w) return true;  // the crossbar's error
    if (w->mem) {
      for (uint32_t i = 0; i < len; ++i) {
        Window *wi = find(addr + i);
        if (!wi || wi != w || !w->known[addr + i - w->base]) return false;
      }
      return true;
    }
    uint32_t off = addr - w->base;
    const eg2_regs::Block &d = w->blk->desc();
    for (uint16_t i = 0; i < d.nwins; ++i) {
      if (off >= d.wins[i].offset && off - d.wins[i].offset < d.wins[i].size) return false;
    }
    auto it = w->reg_flags.find(off & ~3u);
    if (it == w->reg_flags.end()) return true;  // no register: the block's error
    return (it->second & (1 | 4)) == 0;         // neither hwext nor updated by hardware
  }

  // A stop request from a model: software polling a register no model updates stops eg2sim, but
  // here the chip's value of that register is an event, so it is cleared; any other stops the
  // model's predictions.
  void check_stop() {
    std::string &r = Eg2ModelStop::reason();
    if (r.empty()) return;
    if (r.rfind("software polls ", 0) == 0) {
      r.clear();
      return;
    }
    if (stop.empty()) {
      stop = r;
      std::printf("eg2_sysmodel: the model stopped: %s\n", stop.c_str());
    }
  }

  void step(uint64_t t) {
    Eg2System &sys = Eg2System::get();
    if (t < now) t = now;
    // Each event at its own time, in time order; a bound so that a model that keeps asking for a
    // time already past cannot hold the caller.
    for (unsigned n = 0; n < 1000000; ++n) {
      uint64_t e = sys.next_event_ps();
      if (e > t) break;
      if (e > now) now = e;
      sys.run_due(now);
    }
    now = t;
    check_stop();
  }
};

Model *g_model = nullptr;
bool g_created = false;

Model *model(void *h) {
  Model *m = static_cast<Model *>(h);
  return m && m == g_model && !m->released ? m : nullptr;
}

// Every segment of a 32-bit ELF at its load address (p_paddr), as eg2sim's load_elf.
bool load_elf(Model &m, const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    create_error("cannot open %s", path.c_str());
    return false;
  }
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1) {
    create_error("%s is not a 32-bit ELF", path.c_str());
    return false;
  }
  uint32_t phoff = rd(28, 4), phentsize = rd(42, 2), phnum = rd(44, 2);
  for (uint32_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd(ph, 4), offset = rd(ph + 4, 4), paddr = rd(ph + 12, 4),
             filesz = rd(ph + 16, 4);
    if (type != 1 || filesz == 0) continue;
    if (size_t(offset) + filesz > elf.size()) return false;
    Window *w = m.find(paddr);
    if (!w || !w->mem || !w->mem->write(paddr, &elf[offset], filesz)) {
      create_error("%s: segment at 0x%08x is in no memory", path.c_str(),
                   paddr);
      return false;
    }
    m.mark(paddr, filesz, true);
  }
  return true;
}

// An OTP image (vmem, "@<word address> <24-bit word>", data in bits 15:0) into otp_ctrl's software
// read window, as eg2sim's load_otp.
bool load_otp(Model &m, const std::string &path) {
  std::ifstream f(path);
  Window *otp = nullptr;
  for (Window &w : m.windows) {
    if (w.name == "otp_ctrl.core") otp = &w;
  }
  if (!f || !otp || !otp->blk) {
    create_error("cannot load OTP image %s (no file, or no otp_ctrl window)", path.c_str());
    return false;
  }
  std::string line;
  size_t words = 0;
  while (std::getline(f, line)) {
    unsigned addr, word;
    if (std::sscanf(line.c_str(), "@%x %x", &addr, &word) != 2) continue;
    uint8_t b[2] = {uint8_t(word), uint8_t(word >> 8)};
    if (otp->blk->win_write("SW_CFG_WINDOW", 2 * addr, b, 2)) ++words;
  }
  if (words == 0) {
    create_error("OTP image %s: no \"@<word address> <24-bit word>\" line lands in otp_ctrl "
                 "(the RRAM-row image instead of the 24-bit one?)", path.c_str());
    return false;
  }
  return true;
}

}  // namespace

extern "C" {

void *eg2_sysmodel_create(const char *rom_elf, const char *test_elf, const char *otp_vmem,
                          const char *log_dir) {
  if (g_created) {
    create_error("one model per process (SystemC elaborates once)");
    return nullptr;
  }
  g_created = true;
  setenv("SYSTEMC_DISABLE_COPYRIGHT_MESSAGE", "1", 0);
  auto m = std::make_unique<Model>();
  Eg2System &sys = Eg2System::get();
  sys.log_dir = log_dir && *log_dir ? log_dir : ".";
  Model *mp = m.get();
  sys.clock = [mp] { return mp->now; };
  sys.on_emit = [mp](const std::string &inst, const std::string &line) {
    mp->out[inst].push_back(line);
  };
  // The instruction set is the bench's (its ISS wrapper hands over to its CHERIoT-mode model at
  // the lock); the devices go on in CHERIoT mode as before it.
  sys.on_cheriot_lock = [mp] {
    if (mp->ena_low) {
      Eg2System::get().fault_applied(
          "cheriot_ena_low",
          "CHERIoT mode locked, but the core's cheriot_enable_i is forced to MuBi4False: the core "
          "stays in RISC-V mode while cheriot_mem_sys sees CHERIoT mode");
    }
  };
  sys.run_name = "eg2_sysmodel";
  sys.fault_handlers["cheriot_ena_low"] = [mp] {
    mp->ena_low = true;
    auto *cfg = dynamic_cast<eg2_model::Eg2RvCoreIbexCfg *>(Eg2System::get().model("rv_core_ibex.cfg"));
    if (cfg && cfg->cheriot()) {
      Eg2System::get().fault_applied(
          "cheriot_ena_low",
          "the core's cheriot_enable_i forced to MuBi4False in CHERIoT mode: the core goes on in "
          "RISC-V mode; cheriot_mem_sys still sees CHERIoT mode");
    }
    return true;
  };

  struct Win {
    const char *name;
    uint32_t base, size;
  };
  static const Win kWindows[] = {
#define EG2_WINDOW(name, base, size) {name, uint32_t(base), uint32_t(size)},
      EG2_WINDOW_TABLE(EG2_WINDOW)
#undef EG2_WINDOW
  };
  std::vector<Eg2Router::Window> rw;
  for (const Win &w : kWindows) rw.push_back({w.name, w.base, w.size});
  m->bus = std::make_unique<Eg2Router>("bus", rw);
  m->master = std::make_unique<Eg2BusMaster>("bench");
  m->master->socket.bind(m->bus->socket);
  for (size_t i = 0; i < rw.size(); ++i) {
    const Win &w = kWindows[i];
    Window win;
    win.name = w.name;
    win.base = w.base;
    win.size = w.size;
    std::string inst = std::string("t_") + w.name;
    for (char &ch : inst) if (ch == '.') ch = '_';
    const eg2_regs::Block *b = nullptr;
    for (const eg2_regs::Block &blk : eg2_regs::kBlocks) {
      if (win.name == blk.window) b = &blk;
    }
    if (b) {
      Eg2IpModel *r = make_ip_model(inst.c_str(), *b, w.base);
      m->bus->to(i).bind(r->socket);
      win.blk = r;
      for (uint16_t k = 0; k < b->nregs; ++k) win.reg_flags[b->regs[k].offset] = b->regs[k].flags;
      m->targets.emplace_back(r);
    } else {
      auto *mem = new Eg2Memory(inst.c_str(), w.base, w.size);
      m->bus->to(i).bind(mem->socket);
      win.mem = mem;
      win.known.assign(w.size, false);
      sys.memories[w.name] = mem;
      m->targets.emplace_back(mem);
    }
    m->windows.push_back(std::move(win));
  }
  sc_core::sc_start(sc_core::SC_ZERO_TIME);  // elaboration: the sockets are bound

  if (!rom_elf || !*rom_elf || !load_elf(*m, rom_elf)) return nullptr;
  if (test_elf && *test_elf && !load_elf(*m, test_elf)) return nullptr;
  if (otp_vmem && *otp_vmem && !load_otp(*m, otp_vmem)) return nullptr;
  m->dvlog.add_image(rom_elf);
  if (test_elf && *test_elf) m->dvlog.add_image(test_elf);
  m->dvlog.log_path = sys.log_dir + "/sw.model.log";
  sys.on_dv_log = [mp](uint32_t w) { mp->dvlog.feed(w); };
  std::printf("eg2_sysmodel: %zu windows; ROM %s, test %s, OTP %s\n", m->windows.size(), rom_elf,
              test_elf && *test_elf ? test_elf : "-", otp_vmem && *otp_vmem ? otp_vmem : "-");
  g_model = m.release();
  return g_model;
}

int eg2_sysmodel_reset(void *h, uint64_t now_ps) {
  Model *m = model(h);
  if (!m) return -1;
  m->step(now_ps);
  Eg2System &sys = Eg2System::get();
  bool predicted = sys.reset_pending();
  Eg2ResetCause cause = predicted ? sys.reset_cause() : Eg2ResetCause::kPor;
  sys.apply_reset(cause);
  sys.reset_done();
  // The first reset is the power-on the model was created at: memory stays as loaded.
  if (m->reset_seen) {
    for (Window &w : m->windows) {
      if (w.mem && lost_on_reset(w.name, cause == Eg2ResetCause::kPor)) {
        w.known.assign(w.known.size(), false);
      }
    }
  }
  m->reset_seen = true;
  m->check_stop();
  return predicted ? int(cause) : -1;
}

void eg2_sysmodel_step(void *h, uint64_t now_ps) {
  if (Model *m = model(h)) m->step(now_ps);
}

int eg2_sysmodel_read(void *h, uint32_t addr, uint32_t size, uint32_t *data, int *err) {
  Model *m = model(h);
  *data = 0;
  *err = 1;
  if (!m || size == 0 || size > 4) return 0;
  uint8_t b[4] = {0, 0, 0, 0};
  bool ok = m->master->access(tlm::TLM_READ_COMMAND, addr, size, b);
  std::memcpy(data, b, 4);
  *err = ok ? 0 : 1;
  int exact = m->stop.empty() && m->predicts(addr, size);
  m->check_stop();
  return exact;
}

void eg2_sysmodel_load_event(void *h, uint32_t addr, uint32_t size, uint32_t data) {
  Model *m = model(h);
  if (!m || size == 0 || size > 4) return;
  Window *w = m->find(addr);
  if (!w || !w->mem) return;
  uint8_t b[4];
  std::memcpy(b, &data, 4);
  if (w->mem->write(addr, b, size)) m->mark(addr, size, true);
}

int eg2_sysmodel_write(void *h, uint32_t addr, uint32_t size, uint32_t data) {
  Model *m = model(h);
  if (!m || size == 0 || size > 4) return 1;
  uint8_t b[4];
  std::memcpy(b, &data, 4);
  bool ok = m->master->access(tlm::TLM_WRITE_COMMAND, addr, size, b);
  if (ok) m->mark(addr, size, true);
  m->check_stop();
  return ok ? 0 : 1;
}

void eg2_sysmodel_forget(void *h, uint32_t addr, uint32_t size) {
  if (Model *m = model(h)) m->mark(addr, size, false);
}

void eg2_sysmodel_backdoor_write8(void *h, uint32_t addr, uint8_t data) {
  Model *m = model(h);
  if (!m) return;
  Window *w = m->find(addr);
  if (w && w->mem && w->mem->write(addr, &data, 1)) m->mark(addr, 1, true);
}

int eg2_sysmodel_input(void *h, const char *key, uint32_t value) {
  Model *m = model(h);
  if (!m || !key) return 1;
  Eg2System &sys = Eg2System::get();
  auto it = sys.inputs.find(key);
  int r = 0;
  if (it != sys.inputs.end()) {
    it->second(value);
  } else if (std::string(key) == "clk.main" || std::string(key) == "clk.io_div4" ||
             std::string(key) == "clk.aon") {
    // A chip clock's measured frequency in Hz, from now on (Eg2System's clocks).
    std::string k(key);
    Eg2System::Clk c = k == "clk.main" ? Eg2System::Clk::kMain
                       : k == "clk.aon" ? Eg2System::Clk::kAon : Eg2System::Clk::kIoDiv4;
    if (sys.set_clock_hz(c, value, sys.now_ps())) sys.note(k + " = " + std::to_string(value) + " Hz");
  } else if (std::string(key) == "gpio.in") {
    // A level the GPIO model reads when it needs it (ip/eg2_periph_stim.h).
    char v[16];
    std::snprintf(v, sizeof v, "0x%x", value);
    eg2_model::Eg2PeriphStim::all()[key] = v;
  } else {
    r = 1;
  }
  m->check_stop();
  return r;
}

void eg2_sysmodel_wfi(void *h) {
  Model *m = model(h);
  if (!m) return;
  Eg2System &sys = Eg2System::get();
  if (sys.low_power_requested) sys.low_power_requested();
  m->check_stop();
}

int eg2_sysmodel_fault(void *h, const char *name, uint64_t now_ps) {
  Model *m = model(h);
  if (!m || !name) return 1;
  m->step(now_ps);
  Eg2System &sys = Eg2System::get();
  if (!sys.fault_handlers.count(name)) return 1;
  std::printf("eg2_sysmodel: fault %s injected at %llu ps\n", name, (unsigned long long)now_ps);
  sys.inject_fault(name);
  m->check_stop();
  return 0;
}

const char *eg2_sysmodel_core_halted(void *h) {
  Model *m = model(h);
  if (!m) return "";
  m->halted = Eg2System::get().halt_reason();
  return m->halted.c_str();
}

int eg2_sysmodel_pending(void *h, const char *instance) {
  Model *m = model(h);
  if (!m || !instance) return 0;
  auto it = m->out.find(instance);
  return it == m->out.end() ? 0 : int(it->second.size());
}

const char *eg2_sysmodel_pull(void *h, const char *instance) {
  Model *m = model(h);
  if (!m || !instance) return "";
  auto it = m->out.find(instance);
  if (it == m->out.end() || it->second.empty()) return "";
  m->pulled = it->second.front();
  it->second.pop_front();
  return m->pulled.c_str();
}

const char *eg2_sysmodel_verdict(void *h) {
  Model *m = model(h);
  if (!m) return "";
  Eg2System &sys = Eg2System::get();
  if (sys.finished() && m->verdict.empty()) {
    m->verdict = (sys.passed() ? "passed: " : "failed: ") + sys.verdict();
  }
  return m->verdict.c_str();
}

const char *eg2_sysmodel_stop_reason(void *h) {
  Model *m = model(h);
  return m ? m->stop.c_str() : "";
}

const char *eg2_sysmodel_create_error(void) { return g_create_error.c_str(); }

void eg2_sysmodel_release(void *h) {
  Model *m = model(h);
  if (!m) return;
  m->dvlog.close();
  m->targets.clear();  // the UARTs' partial lines
  Eg2System::get().close_logs();
  m->released = true;
}

}  // extern "C"
