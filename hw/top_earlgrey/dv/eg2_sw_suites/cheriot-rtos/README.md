# CHERIoT RTOS software suite

The CHERIoT RTOS firmware we test the core and memory subsystem with, run on the EG2 SystemC
model's second board, Ibex's CHERIoT RTOS test SoC (`eg2sim --board rtos_soc`), with no RTL. One of
the three software regression suites (`doc/sw_test_suites.md`); its 24 runs take about 5 seconds,
where the RTOS test suite alone takes minutes on the RTL.

```
make eg2sim                 # the model, from hw/top_earlgrey/dv/eg2_model (lowRISC/ot-model)
make sw-suite-rtos          # every run; SUITE_JOBS=, PATTERN= (runs whose name contains it), UPDATE=1
make sw-suite-rtos FW_BUILD=1   # rebuild the firmware first (make rtos-firmware)
```

## What it runs

`runs.txt` lists the runs: one firmware image each, and for some the lines it must print or the
checks a mutation must break.

| Runs | What |
|---|---|
| `rtos` | The upstream CHERIoT RTOS test suite (cheriot-rtos `tests/`, 20 tests), from the cheriot-rtos submodule, board sonata-simulator |
| `rtos_prerelease` | The same suite as the flake builds it: cheriot-rtos 31e24610 (23 tests), board sonata-prerelease (real timer waits and interrupts), the flake's CHERIoT LLVM (ee468e7b) |
| `revocation` | The load barrier both ways, revocation by base, preemption, and the hardware revoker's sweeps (10 checks) |
| `juliet` | Juliet CWE-121, 122, 190, 415, 416: compartment enforcement |
| `cheri_c` | The Cambridge CHERI-C tests: 24 of the 25 clang-purecap tests, `noexec_call` (a CHERIoT one), and 5 of the 7 libc tests |
| `regress_drs`, `regress_tei`, `hwrev_irq` | cheriot-rtos `tests.extra`: two scheduler regressions, the hardware revoker's interrupt |
| `dbg_host` | The debug module's system bus access over a capability: with debug access enabled it must clear the tag, disabled it must be refused |
| `perf` | The performance test's ROM_EXT hash feed, by the core and by DMA: the data only (the model does not time the memories, so its cycle counts say nothing) |
| `stress_c<code>d<data>.*` | Revocation stress: revocation, Juliet and the revoker interrupt with slow memories, at the RTL's three latency pairs. The model cannot delay a memory access; `eg2sim --slow-memories` slows the core and the revoker against the timer instead |
| `barrier_none.*`, `tbre_no_inval.*`, `dbg_lock_open.*` | Mutations: the model with one check broken (`eg2sim --mutate`): the load barrier never revokes, the revoker never clears a tag, the debug-access gate stays open. The tests that must notice are listed and must fail |

A run gives many tests, one per check the firmware reports on its UART, named `<run>.<check>`
(`rtos.allocator`, `revocation.phase_2`, `juliet.cwe121`, `cheri_c.array`, `cheri_c.libc_malloc`),
and one more, `<run>.verdict`: the run as a whole, judged as the RTL flow judges it (it ended on the
firmware's exit string, with no failure line, and printed "All tests finished" or the lines
`runs.txt` asks for). The report groups them by `categories.txt`: a category per RTOS suite test and
per Juliet CWE group, and every revocation test (the barrier, the sweeps, the revoker interrupt,
the RTOS allocator, the stress runs, the mutations) under Revocation.

## Outcomes

Each test ends in one of five outcomes: `pass`; `must-fail` (a mutation broke the check, as it
must; the reports show it as "pass (must fail)"); `fail` (the check failed, or the run ended
without reporting it); `model-stop` (the model stopped the run because the firmware needs
something it does not model); or `no-end` (the UART fell silent, or a cycle or instruction limit).
The report gives the reason for every outcome that is not a pass, and `known.txt` the known
failures and why. `expected.txt` holds the outcome each test must give; the suite fails when a
test's outcome changes, and lists tests not in the file as new. A check the firmware stopped
reporting keeps its line and shows how the run ended. `make sw-suite-rtos UPDATE=1` rewrites
`expected.txt` from the run: review the diff.

Known failures (`known.txt`): `dbg_host.tag_clear`, as on the RTL (the debug module's bus writes do
not pass the memory subsystem's tag filter, so a write over a capability leaves its tag set); and
one `rtos_prerelease` test that fails on the RTL as on the model: `futex` (`mcycle` stops while the
core sleeps in WFI, the RTOS counts time in `mcycle` and wakes on `mtime`; GAP-RTLREQ-10 in the
EG2 specification).

## Not run, and why

- CHERI-C `clang_purecap_va_die`: it needs `va_arg` past the last argument to fault, and a CHERIoT
  `va_list` has the stack's bounds, not the arguments': a correct core does not fault.
- CHERI-C `libc_qsort` (the RTOS has no `qsort`) and `libc_string` (it calls `strcpy`; the RTOS's
  string library has `strncpy` and `strlcpy` only). The 16 `clang-hybrid` tests are for hybrid
  mode, which CHERIoT does not have.
- The RTL mutations the model cannot make: `bounds_off`, `ldst_perm_off`, `exec_off` and
  `datastore_tag` break checks inside the core (MPACT-CHERIoT); `trvk_fault_*` are single-cycle
  faults graded on the lockstep alert, which this board does not model; `tbre_read_err` and
  `tbre_fatal` are graded on the testbench's monitor, not on a firmware check; `barrier_all` is
  disabled on the RTL as well.
- The RTL's performance budget (15M cycles): cycles are the RTL's to measure.

## Firmware

The firmware is built in eg2_top_verification (`RTOS_TOP`, default the checkout above `eg2/`):
`make rtos-firmware` builds `cheriot-rtos-test-suite/` (the suite's firmware, `cheriot-rtos` and
`cheri-c-tests`, plain files since 2026-11-06), and `rtos_prerelease` comes from this repository's
flake (`nix build .#cheriot-rtos-test-suite`, done by the runner). Output: `scratch/sw-suites/cheriot-rtos/` in eg2_top_verification (`report.txt`, `results.txt`,
a directory per run with `run.log` and `uart0.log`).
