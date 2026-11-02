# asm-eg2sim: the assembler software suite on the chip model

This runner builds the same riscv-tests and RISC-V architecture tests as `../asm/`, but links them
for the Earl Grey 2 chip and runs them on `eg2sim`. It needs no Bazel-built chip images:

```
make eg2sim
make sw-suite-asm-eg2sim                    # all; SUITE_JOBS=3 by default
make sw-suite-asm-eg2sim PATTERN=rv32mi     # names containing rv32mi
make sw-suite-asm-eg2sim UPDATE=1           # rewrite expected.txt; review the diff
```

Output is in `scratch/sw-suites/asm-eg2sim/` in eg2_top_verification: `report.txt`, `results.txt`, and one build/run directory
per test under `tests/`.

## What it exercises

Each ELF contains a small reset handoff at the generated chip ROM address. The handoff opens an
unlocked PMP entry and jumps to the test at RRAM + 0x80. Test data and architecture-test signatures
are in main SRAM. PASS and FAIL use the OpenTitan test framework's values in
`rv_core_ibex.DV_SIM_WINDOW`; an unexpected-trap `tohost` write is translated to the failure value.
Thus `eg2sim` ends with its normal success or failure status. This covers
the chip reset PC and PMP state, RRAM instruction fetch, SRAM data accesses, the chip memory map and
the DV status path without using the test ROM or an OTP image.

The architecture-test sources in this vendored revision have no reference signatures. Their halt
therefore reports that the program completed and produced its signature; unlike the Simple System
runner, this runner has no independent Spike co-simulation comparison.

## Outcomes

`pass` and `fail` are terminal DV status writes; `model-stop` is an explicit model limitation;
`no-end` is the instruction limit; `no-build` is a compiler failure; `not-run` is a test outside
this runner. `expected.txt` records one outcome for every listed test, and a changed or new outcome
makes the command fail.

The P tests remain `not-run` because Ibex has no packed-SIMD extension. The directed tests remain
listed but `not-run` until their UVM stimulus/signature protocol is supplied.

## CHERIoT programs left to add

CHERIoT-mode assembler programs need a separate environment: switch and lock CHERIoT mode before
executing CHERIoT instructions; use `crt_cheriot.S`-equivalent capability roots, bounded PCC/CSP/CGP,
metadata SRAM initialisation and capability relocations; link code and tagged data into the chip
memories; translate their signature protocol to the DV status word; and provide each directed test's
interrupt, debug, bus-error or revocation stimulus. They must run with the MPACT-CHERIoT ISS rather
than this RISC-V Spike path.
