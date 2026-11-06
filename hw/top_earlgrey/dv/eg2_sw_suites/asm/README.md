# asm: the assembler software suite

The core's instruction tests (doc/sw_test_suites.md, section 1) on Ibex's **Simple System** with
**Spike co-simulation** (Verilator, no licence, no chip RTL): riscv-tests and the RISC-V
architecture tests, each run once, its result compared with `expected.txt`.

```
make sw-suite-asm                    # all; JOBS=4 by default
make sw-suite-asm PATTERN=rv32mi     # only tests whose name contains it
make sw-suite-asm UPDATE=1           # rewrite expected.txt from this run (review the diff)
```

It runs in the Ibex submodule's Nix shell. The first run builds the simulator (configuration
`maxperf-pmp-bmfull`: RV32IMCB, PMP; a few minutes); the tests then take about 15 s at 8 jobs.
Output: `scratch/sw-suites/asm/` in eg2_top_verification (`report.txt`, `results.txt`, a directory per test under `tests/`).

## What a pass means

The test printed `PASS` on Simple System's console, the simulator ended cleanly, and Spike, running
the same program beside the core, matched every instruction the core completed (no `FAILURE`, at
least one instruction matched). A riscv-test prints `FAIL` when one of its checks fails; Spike
catches a wrong result in either kind of test, including the architecture tests' signature stores
(the vendored architecture tests have no reference signatures, so the signature itself is not
compared). A test that traps where it should not stops through `tohost` without a verdict: a fail.
Both failure paths were seen to fire: a riscv-test changed to expect a wrong sum printed `FAIL`, and
`rv32mi.breakpoint` failed on a co-simulation mismatch (before it was marked not-run, below).

## Where the tests come from

The Ibex submodule's own copies (no copies here but `env/arch_test.h`): `vendor/riscv-tests/isa/rv32{ui,um,uc,mi}`,
`vendor/riscv-arch-tests/riscv-test-suite/rv32i_m/*`, and the directed tests' testlists in
`dv/uvm/core_ibex/directed_tests`. `env/` holds this suite's own environment for Simple System: the
memory map (`link.ld`: code at 0x100000, Ibex starts at 0x100080), `riscv_test.h` (Ibex's, with the
verdict on the console), `model_test.h` (the architecture tests' target macros), `arch_test.h` (their environment, its trap
handler 256-byte aligned) and `ss_verdict.h`.

## Outcomes that are not a pass (expected.txt)

| Tests | Outcome | Why |
|---|---|---|
| `rvt.rv32mi.breakpoint` | not-run | it expects a hardware trigger to raise a breakpoint exception (tdata1.action 0); Ibex's triggers only enter debug mode (action is read-only 1, ibex `doc/03_reference/cs_registers.rst`), and Simple System has no debug module. This configuration also has no triggers (DbgTriggerEn=0), which Spike does: it failed on a co-simulation mismatch |

The architecture tests run with their own case-1 settings from their `RVTEST_CASE` lines
(`rvtest_mtrap_routine` for the tests that trap), as riscof passes them, and with `env/arch_test.h`,
a copy of the vendored one whose trap handler is 256-byte aligned (Ibex's mtvec base). `-march`
adds `b`: the Nix shell's GCC 10.2 knows `rol` only under the draft B name.

Not listed: the architecture tests of extensions Ibex does not implement (K, scalar crypto; P,
packed SIMD), and the Ibex directed tests, which need the UVM bench (its interrupts, debug
requests, bus errors, signature address) and run in the Ibex UVM regression.

## Left to do

- The CHERIoT and RISC-V directed tests: a CHERIoT runner (CHERIoT mode, `crt_cheriot.S`) and the
  bench stimulus they need; Simple System gives neither.
- The chip runner the plan asks for (the tests on `eg2sim` and the chip bench, verdict in the
  chip's status word).
