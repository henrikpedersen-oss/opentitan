# EG2 software suites on the SystemC model

The chip software on `eg2sim`, the model in `../eg2_model` (lowRISC/ot-model), each suite run
against its `expected.txt` and reported by category with what happened in every test. Ported from
eg2-git-proposal on 2 November 2026; run from eg2_top_verification, with this tree as its `eg2/`:

| Suite | What | `make` in eg2_top_verification |
|---|---|---|
| `bare-metal/` | every chip test image (OpenTitan's and EG2's), 1,253 runs | `sw-suite-bare-metal` |
| `cheriot-rtos/` | the CHERIoT RTOS test suite, CHERI-C, Juliet and revocation firmware on the model's RTOS SoC board | `sw-suite-rtos` (`FW_BUILD=1` builds the firmware) |
| `asm-eg2sim/` | riscv-tests, the RISC-V architecture tests and the `eg2_rand` framework (`sw/device/lib/testing/eg2_rand`) on eg2sim | `sw-suite-asm-eg2sim` |
| `nist-ref/` | every NIST CAVP vector file against the C references and OTBN's ISS, each also corrupted, where it must be caught | `sw-suite-nist-ref` |
| `rng/` | the model's random-number captures through SP 800-90B, SP 800-22, AIS 31, Dieharder, TestU01, PractRand and rngtest | `sw-suite-rng` |

`make sw-suites` runs them all. Options: `SUITE_JOBS=` (runs at once, default 3), `PATTERN=` (only
names containing it), `UPDATE=1` (rewrite `expected.txt` from the run, for review in the diff).
`env.sh` says what each runner needs from its surroundings (this tree, the model binary, the
output directory, the ibex_cheriot_verification checkout); the make targets set all of it.

A suite fails when an outcome changes against its `expected.txt`. When the model's pin
(`../eg2_model`) moves, the outcomes it changes are updated in the same commit, so the review sees
what the new model did.
