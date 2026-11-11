# CHERIoT memory subsystem reference model

A C++ model of OpenTitan's CHERIoT memory subsystem (`cheriot_mem_sys`) for its testbenches. It is
written from the documents, never from the RTL: the theory of operation, the programmer's guide, the
registers, the CHERIoT ISA, and the Requirements Review's detailed requirements (REQ_TF, REQ_WTRC,
REQ_TAG, REQ_TBRE, REQ_META, REQ_ALT). Unresolved behavior must stay explicitly unjudged or stop as undocumented; a DV interpretation
is not an independent specification. Q43 is one such open case. The combined plan is in
eg2_top_verification's `test-specifications/cheriot_mem_sys_revocation_verification_spec.md`.

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
sweep), the model keeps both values in its independent state. `check_core_tag` compares observed
responses with each other: the two halves must agree, and repeated reads must remain stable when
no mutation is possible. These comparison witnesses do not resolve model memory or feed expected
values back from the DUT. Refused tag writes preserve the previous tag and its stability witness.

**Not modelled:** the MUBI-encoded tag sideband and the lockstep copy, which the RTL does not
implement (the known gaps of REQ_CMS_01 and 02), and cycle timing, which is the bench's to check.

The UVM bench uses the split interface: `cms_predict_core` / `cms_update_core`, the corresponding
bitmap and CSR calls, and `cms_check_core_tag` for response relations. The scoreboard compares the
predictions. Other benches may use `cms_core`, which combines prediction, comparison and update.
Both paths use the same rules. START timing, integrity checking and external request tracking belong
to the surrounding UVM components, not this clock-independent model.

**Users:**
- the OpenTitan UVM bench, through `cheriot_mem_sys_ref_model` and `cheriot_mem_sys_scoreboard`;
- eg2sim.

Build: `make test`, `make lib` (in a shell with a C++17 compiler).

### Black-box fault attribution (11 November 2026)

No prediction, requester ownership or check depends on DUT internals. The UVM reference model
receives only external agent items. A meta-memory fault with several possible external requesters
opens their documented error/tag alternatives, including sweep_err and the sweep counter outcome.
The model retains those alternatives independently; it does not select one from the DUT response.
Unhinted and unaligned loads do not perform tag lookups. Optional internal taps only print routing
for debugging and have no connection to this model or its scoreboard.

The retired standalone `cms` bench is no longer a model consumer.

The source directory is `hw/ip/cheriot_mem_sys/dv/cmodel`. FuseSoC resolves
`lowrisc:dv:cheriot_mem_sys_cmodel:0.1` here; eg2sim includes the same header.
