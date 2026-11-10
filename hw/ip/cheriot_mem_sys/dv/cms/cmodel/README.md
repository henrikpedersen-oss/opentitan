# CHERIoT memory subsystem reference model

A C++ model of OpenTitan's CHERIoT memory subsystem (`cheriot_mem_sys`) for its testbenches. It is
written from the documents, never from the RTL: the theory of operation, the programmer's guide, the
registers, the CHERIoT ISA, and the Requirements Review's detailed requirements (REQ_TF, REQ_WTRC,
REQ_TAG, REQ_TBRE, REQ_META, REQ_ALT). Where the documents are silent it follows DV's readings, and
says so in the code. The plan is in eg2_top_verification's `cheriot_mem_sys_verification_spec.md`.

| File | What |
|---|---|
| `cms_core.h` | The model, header only: the tag filter and the WTRC on the core data port, the mode (MuBi4), the tag store and the bitmap in the meta SRAM map, the bitmap windows and their access checks, the registers, the revocation engine (start checks, sweep, sealing, base and bitmap lookup, the core write watch, errors, epoch, interrupt), the alert causes |
| `cms_dpi.cc`, `cms_cmodel_pkg.sv` | Its C interface for DPI and the SystemVerilog package |
| `cms_core_test.cc` | Unit test: each documented scenario, and a wrong response the model must flag |

**How a bench uses it.** One model per bench (`cms_new`). The scoreboard feeds it every complete
transaction its monitors see:
- core data port: request, the host's and the meta path's errors, the response (`cms_core`);
- the two bitmap windows (`cms_bitmap`); the registers (`cms_csr`);
- the sweep's end and faults (`cms_sweep_end`, `cms_sweep_read_error`, `cms_sweep_fault`).

Each check returns its number of mismatches; `cms_last_messages()` gives the text. At the end of a
test, `cms_check_meta_word` compares the meta SRAM word by word (bits a race left open are not
compared). Where a race leaves a tag open (a store racing a sweep, the bitmap changed during a
sweep), the model keeps both values until a later load resolves it.

**Not modelled:** the MUBI-encoded tag sideband and the lockstep copy, which the RTL does not
implement (the known gaps of REQ_CMS_01 and 02), and cycle timing, which is the bench's to check.

**Users:**
- the OpenTitan UVM bench, as `cheriot_mem_sys_cmodel_sb`;
- the `cms` bench, in place of `cms_sb`;
- eg2sim.

Build: `make test`, `make lib` (in a shell with a C++17 compiler).
