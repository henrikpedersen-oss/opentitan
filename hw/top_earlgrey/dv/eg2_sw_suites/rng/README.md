# Random-number statistical suite

This suite captures the EG2 random-number path on `eg2sim` and runs the published statistical
batteries required by the EG2 verification specification's “Random Number Generation Test Suite”
and REQ_PBF_24. It covers raw post-health-test symbols, conditioned entropy, CSRNG output and the
EDN0 CPU endpoint in boot, FIPS and bypass modes. RISC-V and CHERIoT images are both run.

`make sw-suite-rng SUITE_JOBS=4` builds the tools (`make rng-tools`), captures 3,200,000 words per tap and
grades every stream. `PATTERN=text` selects capture runs whose names contain `text`.
`RNG_WORDS=n` is for focused runs; tools that need more data report `SKIPPED` and the exact minimum.
`UPDATE=1` rewrites `expected.txt`; review that diff before committing it.

The `_inject_fail` images duplicate every word. SP 800-22 and rngtest must report `pass (caught)`. The
`pattern:biased` raw capture must make SP 800-90B report `pass (caught)`. A `fail (not caught)`, a pass-criterion
failure, a tool that did not build (`NOT RUN`), or a result different from `expected.txt` fails the
suite. Output is in `scratch/sw-suites/rng/report.txt` in eg2_top_verification; individual tool logs are under `assessment/`.

`rng_assess.py` and `rng_extract.py` are copied from `eg2_top_verification/scripts`. Their verdict
logic is unchanged. Only documentation paths and target names were adapted for this suite. They are
the temporary Python exception recorded in `codex-agent.md`; the assessment driver will move to C.
