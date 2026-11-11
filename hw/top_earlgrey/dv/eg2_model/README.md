# EG2 functional system model

This directory models the software-visible behavior of Earl Grey 2 in C++ and
SystemC. It lets firmware configure registers, access memory, receive interrupts,
and interact with peripherals without running an RTL simulation.

This directory is the functional reference model. The UVM testbench that can
consume its DPI predictions lives outside it.

The same IP implementations serve three entry points:

| Entry point | What drives the model | Typical use |
|---|---|---|
| `eg2sim` | Spike, then MPACT-CHERIoT after the CHERIoT lock | Run firmware and inspect device behavior |
| `libeg2sysmodel.so` | Explicit bus transactions and external inputs through a C API | Independently predict results for a chip testbench |
| `eg2sim --board rtos_soc` | MPACT-CHERIoT on the RTOS test SoC address map | Run the CHERIoT RTOS suites |

This is a functional model, not a cycle-accurate implementation. Scheduled events
allow timers, FIFOs, interrupts and sleep to make progress; their ordering does not
establish RTL latency or throughput. Timing-dependent software tests must be
identified separately, rather than made to pass by tuning model delays.

Start with [the architecture guide](docs/architecture.md) for a walk through one
bus access, an interrupt and a reset. [The review record](docs/review.md) separates
confirmed fixes, open findings and validation limits. The generated register and
interrupt tables are reference data; they are not the recommended reading order.
The [follow-up review](docs/review-followup.md) records the latest transport,
image-loading and OTP fixes and the remaining fidelity gaps. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the evidence and tests expected with changes.

Expected behavior comes from the IP documentation, register descriptions and top
configuration. Any RTL-derived exception must be identified in its source comment.
When the documents leave a required result undefined, request an
`Eg2ModelStop::request("undocumented: ...")` and record a documentation question.
Observed DUT loads must never become expected values for subsequent comparisons.

## Building

The model includes a few files from the OpenTitan tree it models (the top's generated header,
`eg2_tables.h`, the AES, HMAC and KMAC reference code), so it builds at its place in that tree:
`EG2_ROOT` is the tree's root, by default four directories up, as at
`hw/top_earlgrey/dv/eg2_model`. Its tools come from this repository's flake:

    nix develop --command make OUT=<dir> <dir>/eg2sim    # the model on its own
    nix develop --command make OUT=<dir> lib replay      # the chip bench's library and its replay check
    nix develop --command make OUT=<dir> nist            # the NIST CAVP checker

The register and interrupt headers are generated and committed here. See the
generator commands below the file table; do not edit these headers by hand.

To build and run the local C++ checks from this directory:

```sh
nix develop --command make OUT=build check
```

`core_tests` runs the 15 existing standalone core tests and checks that each test's
deliberately broken variant fails. `model_test` exercises the shared register,
interrupt, GPIO and TBRE paths through TLM sockets. `sysmodel_test` exercises the
public C API, including independent memory predictions and the meta-SRAM alias.
`replay_test` checks that valid evidence passes and corrupt or missing evidence
fails. These checks require no RTL simulator or prebuilt firmware. `core_tests`
needs a C++17 compiler; the other targets also need SystemC. The pinned development
shell supplies the compatible toolchain and library paths.

For a firmware run, use a test ROM, a test ELF and an OTP image from the same
Earl Grey checkout. ROM verification also needs the existing scrambled VMEM,
including its independently stored expected digest. Prepare its checker input once:

```sh
python3 tools/prepare_rom_check.py --elf path/to/test_rom.elf \
    --vmem path/to/test_rom.39.scr.vmem --function-name "" --customization ROM_CTRL
```

The converter uses the checkout's ROM image-format utilities; its Python environment
needs `pycryptodome`, `pyelftools`, `hjson` and `mako`. It verifies the decoded
VMEM body against the ELF and writes `test_rom.elf.romcheck`; it does not calculate
or replace the stored expected digest. The explicit N/S configuration above is
for the current public test-image producer. The ROM specification names the prefix
but leaves its N/S split unspecified. A different image configuration must supply
its own documented settings. Missing checker input stops as **not modelled**.

Both standalone and DPI loaders look for `<rom ELF>.romcheck`. The standalone
`--rom-checker` option can select a different path. Then run:

```sh
mkdir -p build/run
build/eg2sim --rom path/to/test_rom.elf --test path/to/test.elf \
    --otp path/to/img_rma.24.vmem --log-dir build/run
```

The exit status distinguishes a passing test (0), setup failure (1), model stop
(2), failing test (3), and instruction limit (4). A model stop is not a passing
hardware check. Read the reason in the run output before interpreting a result.

## Files

| File | What |
|---|---|
| `eg2sim.cc` | Standalone board setup, command-line options, the run and its report |
| `eg2_image.{h,cc}` | ELF loading and symbols, RRAM images, OTP loading and persistence |
| `eg2_rom_image.h`, `tools/prepare_rom_check.py` | Independent stored ROM checker input, including integrity bits and expected digest |
| `eg2_elf.h` | Validated ELF32 reader shared by the standalone and DPI loaders |
| `eg2_transaction.h` | Contiguous TLM payload validation and overflow-safe range checks |
| `eg2_stimulus.{h,cc}` | Triggered stimulus descriptions, parsing and image-symbol expansion |
| `eg2_spike_cpu.{h,cc}` | Spike as TLM initiator: Ibex reset state, interrupt lines, WFI and sleep, chip resets |
| `eg2_spike_debug.cc` | Spike debug entry/resume, register access and program-buffer execution |
| `eg2_iss.h` | The CHERIoT-mode instruction-set model the core switches to at the CHERIoT lock, and the handover state (REQ_SOC_07) |
| `eg2_mpact_cpu.{h,cc}` | MPACT-CHERIoT (`EG2_MPACT`, flake package `mpact-cheriot`) as that model: TLM fetch, loads and stores, tags in the meta SRAM, FENCE.I and bit manipulation executed for it |
| `eg2_router.h`, `eg2_targets.h` | The address decoder; memories with DMI; `Eg2ModelStop` |
| `eg2_regblock.h` | A register block from its HJSON: reset values, access policies, REGWEN, windows |
| `eg2_system.h` | What the models share: time and events, interrupt lines, alerts, escalation signals, the core's halt, faults, reset, verdict, output logs |
| `eg2_ip_model.h` | `Eg2IpModel`: a register block plus common interrupt and alert behavior |
| `eg2_ip_models.cc` | The registry: which window gets which model |
| `eg2_dvlog.h` | The software's DV log (LOG_* through the DV window), decoded from the images' `.logs.fields` to stdout and `<log-dir>/sw.model.log` |
| `ip/*.h` | The IP models |
| `ip/eg2_otp_crypto.h` | Shared PRESENT-128 cipher and specified OTP partition digest |
| `ip/*_core.h`, `dpi/*_test.cc` | Reusable behavioral cores and their focused tests; many cores do not need SystemC |
| `tests/` | Shared-model and public-API regressions, plus the core-test runner |
| `eg2_model_regs.h` | Generated: every register block (`scripts/gen_eg2_model_regs.py`) |
| `eg2_model_irqs.h` | Generated: every interrupt with its PLIC ID, every alert with its ID, the always-on modules (`scripts/gen_eg2_model_irqs.py`) |
| `dpi/eg2_sysmodel.{h,cc}` | `libeg2sysmodel.so`: the platform without an instruction-set model, as a C (DPI-C) library for the chip bench (build mode `en_eg2_model`, `env/eg2_sysmodel.sv`): the bench drives the bus, gives time and the agents' inputs, pulls the output items |
| `dpi/eg2_sysmodel_replay.cc` | Drives the library from an `eg2sim --spike-log` trace as the bench would, and checks the items against eg2sim's |
| `rtos_soc/rtos_soc.h` | The second board, Ibex's CHERIoT RTOS test SoC: its address map, timer (CLINT), rev_ctl and the shim to the TBRE, revocation bitmap window, perf device and its DMA, default responder, the debug module's system bus access as the testbench drives it, and the testbench's run monitor |
| `rtos_soc/rtos_soc.cc` | `eg2sim --board rtos_soc`: that platform, its image loading and its run |
| `nix/`, `flake.nix` | SystemC, Spike and MPACT-CHERIoT, pinned; MPACT behind its C interface (`nix/mpact-cheriot/`) |
| `rtos_soc/rtos_soc_regs.h` | Generated: the board's register blocks from Sonata's hjson, and its interrupt table (`scripts/gen_rtos_soc_model_regs.py --sonata <sonata-system>`) |

`make gen` in eg2_top_verification regenerates the Earl Grey register and interrupt
headers. The RTOS board's register header has its own generator,
`scripts/gen_rtos_soc_model_regs.py --sonata <sonata-system>`. Never hand-edit
generated tables.

The IP group headers are small include entry points. Read the individual device
header when changing behavior:

| Group | Implementations |
|---|---|
| `ip/eg2_mem.h` | `eg2_rram_ctrl.h`, `eg2_sram_ctrl.h`, `eg2_rom_ctrl.h`, `eg2_cheriot_mem_sys.h`, plus OTP/life cycle |
| `ip/eg2_mem_otp.h` | `eg2_otp_data.h`, `eg2_otp_ctrl.h`, `eg2_lc_ctrl.h`; their cross-device methods are in `eg2_otp_ctrl.cc` |
| `ip/eg2_periph_sys.h` | `eg2_sysrst_ctrl.h`, `eg2_adc_ctrl.h`, `eg2_pinmux.h`; shared wake-up/logging helpers in `eg2_periph_wakeup.h` |
| `ip/eg2_crypto_entropy.h` | `eg2_entropy_common.h`, `eg2_entropy_src.h`, `eg2_csrng.h`, `eg2_edn.h` |
| `ip/eg2_crypto_hash.h` | `eg2_hmac.h`, `eg2_kmac.h` |
| `ip/eg2_crypto_keymgr.h` | `eg2_keymgr_dpe.h`, `eg2_otbn_ctrl.h` |

The OTBN processor is separate from its register adapter: `ip/eg2_otbn.h` holds
its state and interface, `eg2_otbn_wide.h` the 256-bit arithmetic,
`eg2_otbn_keccak.h` the sponge, `eg2_otbn_execute.cc` instruction execution,
and `eg2_otbn_accelerators.cc` the KMAC and masking interfaces. A standalone C++
consumer of `Eg2OtbnCore` must link both implementation files; it needs no
SystemC. The model Makefile includes these files in both executable and library.

## In the chip bench

`make eg2sysmodel` in eg2_top_verification builds `libeg2sysmodel.so` and `eg2_sysmodel_replay` into
`scratch/eg2sysmodel/`; the tests `chip_sw_eg2_*_model` (build mode `en_eg2_model`) use it. Without
a simulator, from an eg2sim run with `--spike-log trace.log --log-dir ref`:

    eg2_sysmodel_replay --rom <rom.elf> --test <test.elf> --otp <img.24.vmem> --trace trace.log \
        --expect uart0=ref/uart0.model.log

must report every predicted load matched and the UART items identical.

The library tracks which memory bytes are independently known from images,
explicit stores and backdoor initialization. An unknown load remains unknown:
the compatibility function `eg2_sysmodel_load_event()` does not copy the DUT's
answer into the model. PLIC claims likewise come from the model's own pending
interrupts. The read API reports whether a value is predicted; a replay with
unpredicted loads is not proof that every load was checked.

GPIO inputs use `gpio0.in` and `gpio1.in`, one value per bank. `gpio.in` is a
compatibility alias for bank 0. The same names work for standalone stimuli and
`eg2_sysmodel_input()`.

## Adding an IP model

1. Write the class in `ip/`, derived from `Eg2IpModel` (`eg2_ip_model.h`), from the IP's
   documentation. Do not use RTL or observed chip values to fill missing rules.
   Stop on an unspecified required outcome and record the documentation question.
2. In its constructor, take its registers by name (`reg("CTRL")`) and mark those it keeps up to
   date with `set_modelled`.
3. Override what it needs:
   - `reg_write(reg, old, wdata, wmask)`: after a software write (`value(reg)` is the new value);
   - `reg_before_read(reg)`, `reg_after_read(reg)`: bring a register up to date, read side effects;
   - `reg_before_write(reg, wdata, wmask)`: before a software write; return false to drop it;
   - `win_write(win, off, len)`: a write into a memory window of the block;
   - `win_read(win, off, data, len)`: a read from a memory window; fill `data` and return true to
     answer it (a FIFO that pops), false to read the window's memory;
   - `fail_access()`, from a hook: the bus access answers an error (TL-UL d_error);
   - `idle()`: false while the block is busy (clkmgr's CLK_HINTS_STATUS reads it);
   - `on_time(now)`: at the time asked for with `sys_.schedule(this, t)`;
   - `on_reset(cause)`: the default restores the reset values (an always-on IP keeps them except
     at power-on); call it first.
   A memory that can refuse instruction fetches registers a query with
   `sys_.add_exec_region("<window>", [this] { return ...; })`; a refused fetch is an instruction
   access fault.
4. Interrupts: `set_intr("name")` for an event, `set_intr_level("name", level)` for a status
   interrupt; INTR_STATE / INTR_ENABLE / INTR_TEST and the PLIC lines are handled by the base.
   Alerts: `raise_alert("name")`; ALERT_TEST is handled by the base.
5. Outputs for the agent comparisons: `sys_.emit("<instance>", line)`, one line per item in
   the chip bench's `<instance>.chip.log` format, into `<instance>.model.log`.
6. Another IP: `dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))`; a RAM:
   `sys_.memories["sram_ctrl_main.ram"]`.
7. Register the class in `eg2_ip_models.cc` or its group's `EG2_MODELS_*` list,
   keyed by IP type (every instance) or window. Add a regression that fails when
   the new behavior is deliberately broken, and document unsupported behavior.

Time is the core's: one instruction per CPU cycle (100 MHz, sim_dv). A model never busy-waits; it
computes its state when software reads it and schedules the next time it must act. Because the
UART transmits at its baud rate (1 Mbaud in sim_dv: 10 us, 1000 instructions per character),
a test that prints spends most of its instructions waiting for the TX FIFO.

## Alerts and escalation

The alert handler (`ip/eg2_core_system.h`, from `hw/top_earlgrey/ip_autogen/alert_handler/doc`)
classifies every alert and local alert, raises the class interrupt, accumulates, runs the interrupt
timeout and each class's escalation (Idle, Timeout, Phase0–3, Terminal; phase lengths, the
phase-to-signal map, `CTRL.LOCK`, the clear and `CLR_REGWEN`) on its clock, io_div4, and latches
the crash dump at the configured phase. Its shadowed registers take a value on two equal writes;
two different ones are the local alert "shadow reg update error". The four escalation signals reach
the receivers the top connects (`top_earlgrey.hjson` `alert_handler.esc_tx`):

| Severity | Receiver | Effect in the model |
|---|---|---|
| 0 | `rv_core_ibex` | `NMI_STATE.ALERT`; the core's NMI with `NMI_ENABLE.ALERT_EN` (Spike; in CHERIoT mode the run stops: MPACT-CHERIoT has no NMI) |
| 1, 2 | `lc_ctrl` (`esc_scrap_state0`, `esc_scrap_state1`) | the ESCALATE state until reset: `lc_cpu_en` off halts the core, `lc_escalate_en` on |
| 3 | `pwrmgr` (`esc_rst`) | the escalation reset; `RESET_INFO.HW_REQ` bit 6 |

A reset with `rstmgr` `ALERT_INFO_CTRL.EN` set captures the crash dump into `ALERT_INFO` (9 words).
A halted core (fetch disabled by `fatal_hw_err`'s local escalation or by `lc_cpu_en`) executes
nothing while time runs on to the next model event; if none will ever come the run stops, naming
the halt. The shared alert core also implements ping handling and escalation
state; consult `ip/eg2_alert_core.h` for its modeled inputs and limits. Signal
integrity and handshake latency are not cycle-accurate. The current SystemC
binding does not suppress alerts by low-power group. Every escalation step goes
to stderr and `<log-dir>/model.notes.log`.

## Faults

`eg2sim --fault <name>[@<when>]` injects a fault as an input event (the models are not changed):
`lockstep` (the core's lockstep comparator mismatches: `ERR_STATUS.FATAL_CORE_ERR`, `fatal_hw_err`,
the core halted; then whatever the software's alert configuration escalates to) or
`cheriot_ena_low` (the core's CHERIoT enable forced to MuBi4False from then on: at the CHERIoT lock
the core stays on Spike, or leaves MPACT-CHERIoT for Spike if already locked, while
`cheriot_mem_sys` sees CHERIoT mode). `<when>` is an instruction count, a PC (`0x...`) or a symbol
of the test image; none: from the start. The run prints `eg2sim: fault <name> applied at ...` when
the fault changes what the chip does and `eg2sim: fault <name> never applied` otherwise. The chip
bench tells the library the same with `eg2_sysmodel_fault(h, name, now_ps)` and reads the core's
halt with `eg2_sysmodel_core_halted(h)`; `eg2_sysmodel_replay --fault <name>@<instruction>` tries it
without a simulator.

`make mutation-model` in eg2_top_verification (`scripts/eg2_model_mutation.py`) runs the framework
tests and `eg2_alert_escalation_smoketest` clean and once per fault, and grades each run CAUGHT,
MISSED or INACTIVE.

## The RTOS test SoC board

`eg2sim --board rtos_soc` (an eg2sim built with `EG2_MPACT`) is Ibex's CHERIoT RTOS test SoC
(ibex_cheriot_verification: `ibex/dv/cheriot-rtos-test-suite`) instead of Earl Grey 2, so that the
CHERIoT RTOS suites, whose UART output is their verdict, judge the model on their own, in seconds:

    eg2sim --board rtos_soc --boot <sw/boot/sim_boot_stub/sim_boot_stub> --test <firmware ELF> \
        --log-dir <dir> | tee <dir>/run.log
    dv/scripts/cheriot_rtos_verdict.sh <dir>/uart0.log <dir>/run.log 1

The core is MPACT-CHERIoT from Ibex's CHERIoT reset state (no RISC-V phase), with the SoC's tags
and its load barrier; the UART, rv_plic and the TBRE are the models above, the timer, rev_ctl, the
bitmap window, the perf device (the performance tests' FIFO and DMA) and the default responder the
board's own (`rtos_soc/rtos_soc.h`). The debug module is not modelled; its system bus access is,
as the testbench drives it for the debug host test (a `DBGHOST REQ` line on UART0 is served: a bus
write that leaves the capability tags as they are, refused while debug access is disabled). `<dir>/uart0.log`
is UART0's raw bytes, as uartdpi writes them; the run ends where the testbench ends it, and prints
its lines. `--mutate barrier_none|tbre_no_inval|dbg_lock_open` breaks the load barrier, the TBRE's
invalidation or the debug-access gate, to show that the suites notice. `--slow-memories
<code>:<data>` stands in for the SoC's memory latencies in the revocation stress runs: the core at
1 / (1 + code) of the clock and the TBRE at 2 * (1 + data) cycles a capability (the model cannot
delay a memory access). Earl Grey 2 stays the default board (`--board earlgrey`).

`eg2sim --otp-out <file>` writes the OTP at the end of the run as a `.24.vmem` image (16 data
bits and 6 ECC bits per word), which a later run can load with `--otp`.

The [fidelity review](docs/fidelity-review.md) records the independent ROM input,
shared shadow protocol, keymgr documentation stops and unified OTP controller,
including every changed verdict in the 535-image comparisons. A build or unit
test pass is not a claim that the firmware suite or all hardware behavior passes.
