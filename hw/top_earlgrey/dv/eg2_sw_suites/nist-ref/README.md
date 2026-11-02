# NIST host-reference software suite

`run.sh` runs every applicable vector from the archives pinned by the EG2 fork against the C and
OTBN ISS references used by the SystemC model and chip scoreboards. Each vector-file line is a test
case. Its paired `.corrupt` case flips the first expected result and passes only when exactly one
comparison fails. The report therefore uses `pass` for the clean result and `pass (caught)` for that red
check (`CAUGHT` in `expected.txt`; a corrupt case that passes is `fail (not caught)`, `MISSED`).

The OTBN lines are separate cases. RSA signing is split into one case per vector, so every runnable
case stays below the suite's 30-minute limit. An RSA-3072 signature alone exceeds that limit; those
unsplittable cases are listed as `on hold`, with measured or extrapolated cost and a generated
`rerun.sh` command in the report.

Run it from eg2_top_verification with `make sw-suite-nist-ref SUITE_JOBS=3`, which first builds its inputs
(`nist-cavp`, `nist-cavp-check`, `nist-otbn-elfs`). Output is under `scratch/sw-suites/nist-ref/` there; `report.txt` has the same outcome comparison and per-category list as
the other software suites.
