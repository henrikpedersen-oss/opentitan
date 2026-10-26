# EG2 SystemC model

Earl Grey 2 as a SystemC TLM-2.0 model: Spike as the instruction-set model, the address decoder,
and every IP of the top. It runs on its own as `eg2sim` (`make eg2sim` in eg2_top_verification),
and is the system model of the chip bench's reference model (doc/system_model in
eg2_top_verification).

## Files

| File | What |
|---|---|
| `eg2sim.cc` | The platform from the window table; image, OTP and backdoor loading; the run and its report |
| `eg2_spike_cpu.{h,cc}` | Spike as TLM initiator: Ibex reset state, interrupt lines, WFI and sleep, chip resets |
| `eg2_iss.h` | The CHERIoT-mode instruction-set model the core switches to at the CHERIoT lock, and the handover state (REQ_SOC_07) |
| `eg2_mpact_cpu.{h,cc}` | MPACT-CHERIoT (`EG2_MPACT`, flake package `mpact-cheriot`) as that model: TLM fetch, loads and stores, tags in the meta SRAM, FENCE.I and bit manipulation executed for it |
| `eg2_router.h`, `eg2_targets.h` | The address decoder; memories with DMI; `Eg2ModelStop` |
| `eg2_regblock.h` | A register block from its hjson (M2): reset values, access policies, REGWEN, windows |
| `eg2_system.h` | What the models share: time and events, interrupt lines, alerts, escalation signals, the core's halt, faults, reset, verdict, output logs |
| `eg2_ip_model.h` | `Eg2IpModel`: a register block plus every IP's interrupts and alerts (M3) |
| `eg2_ip_models.cc` | The registry: which window gets which model |
| `eg2_dvlog.h` | The software's DV log (LOG_* through the DV window), decoded from the images' `.logs.fields` to stdout and `<log-dir>/sw.model.log` |
| `ip/*.h` | The IP models |
| `eg2_model_regs.h` | Generated: every register block (`scripts/gen_eg2_model_regs.py`) |
| `eg2_model_irqs.h` | Generated: every interrupt with its PLIC ID, every alert with its ID, the always-on modules (`scripts/gen_eg2_model_irqs.py`) |
| `dpi/eg2_sysmodel.{h,cc}` | `libeg2sysmodel.so`: the platform without an instruction-set model, as a C (DPI-C) library for the chip bench (build mode `en_eg2_model`, `env/eg2_sysmodel.sv`): the bench drives the bus, gives time and the agents' inputs, pulls the output items |
| `dpi/eg2_sysmodel_replay.cc` | Drives the library from an `eg2sim --spike-log` trace as the bench would, and checks the items against eg2sim's |
| `rtos_soc/rtos_soc.h` | The second board, Ibex's CHERIoT RTOS test SoC: its address map, timer (CLINT), rev_ctl and the shim to the TBRE, revocation bitmap window, default responder, and the testbench's run monitor |
| `rtos_soc/rtos_soc.cc` | `eg2sim --board rtos_soc`: that platform, its image loading and its run |
| `rtos_soc/rtos_soc_regs.h` | Generated: the board's register blocks from Sonata's hjson, and its interrupt table (`scripts/gen_rtos_soc_model_regs.py --sonata <sonata-system>`) |

`make gen` in eg2_top_verification regenerates both headers.

## In the chip bench

`make eg2sysmodel` in eg2_top_verification builds `libeg2sysmodel.so` and `eg2_sysmodel_replay` into
`scratch/eg2sysmodel/`; the tests `chip_sw_eg2_*_model` (build mode `en_eg2_model`) use it. Without
a simulator, from an eg2sim run with `--spike-log trace.log --log-dir ref`:

    eg2_sysmodel_replay --rom <rom.elf> --test <test.elf> --otp <img.24.vmem> --trace trace.log \
        --expect uart0=ref/uart0.model.log

must report every predicted load matched and the UART items identical.

## Adding an IP model

1. Write the class in `ip/`, derived from `Eg2IpModel` (`eg2_ip_model.h`), from the IP's
   documentation; where the documentation is silent and the RTL is read, say so in a comment.
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
5. Outputs for the agent comparisons (M6): `sys_.emit("<instance>", line)`, one line per item in
   the chip bench's `<instance>.chip.log` format, into `<instance>.model.log`.
6. Another IP: `dynamic_cast<Eg2Pwrmgr *>(sys_.model("pwrmgr"))`; a RAM:
   `sys_.memories["sram_ctrl_main.ram"]`.
7. Add one line to `kModels` in `eg2_ip_models.cc`, keyed by IP type (every instance) or window.

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
the halt. Not modelled: pings and their local alerts, signal integrity, the low-power groups, the
handshake's latency, the repetition of a fatal alert, the CPU crash dump (`CPU_INFO`). Every
escalation step goes to stderr and `<log-dir>/model.notes.log`.

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
bitmap window and the default responder the board's own (`rtos_soc/rtos_soc.h`). `<dir>/uart0.log`
is UART0's raw bytes, as uartdpi writes them; the run ends where the testbench ends it, and prints
its lines. `--mutate barrier_none|tbre_no_inval` breaks the load barrier or the TBRE's invalidation,
to show that the revocation suite notices. Earl Grey 2 stays the default board (`--board earlgrey`).

`eg2sim --otp-out <file>` writes the OTP at the end of the run as a `.24.vmem` image (16 data
bits and 6 ECC bits per word), which a later run can load with `--otp`.
