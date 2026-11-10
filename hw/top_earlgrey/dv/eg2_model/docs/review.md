# Model review — 10 November 2026

This is the historical record of the first pass and subsequent file split.
The [follow-up review](review-followup.md) records the later OTP digest,
power-boundary, transport and ELF fixes against checked-in commit `da6c4d0e4d`.
Its remaining-findings table supersedes the open findings below.

This review fixes shared register, interrupt, GPIO, memory-alias and replay
behavior and provides a reading guide and a repeatable local test entry point.
It does **not** establish complete chip equivalence. The open findings below
matter when using this model as a verification reference.

The control was a snapshot taken after the CMS adapter handoff, including the
existing uncommitted changes. The control and revised model used the same
SystemC, Spike, MPACT and firmware artifacts. Generated headers and the shared
`hw/ip/cheriot_mem_sys/dv/cms/cmodel/cms_core.h` were not edited.

## Fixed findings

Locations refer to the revised source. Each regression is exercised by
`make check`; the relevant tests also fail against the control implementation.

### F1. REGWEN blocked storage but allowed device actions — model bug

**Location:** [`eg2_regblock.h:218`](../eg2_regblock.h#L218).

**Source:** [`util/reggen/README.md`](../../../../../util/reggen/README.md), register
attribute `regwen`: the enabling register must be true to allow writes.
[`sram_ctrl.hjson`](../../../../../hw/ip/sram_ctrl/data/sram_ctrl.hjson),
`CTRL_REGWEN`: clearing it prevents further writes to `CTRL`.

**Failure:** writing `CTRL.INIT` after closing `CTRL_REGWEN` still ran the SRAM
initialization hook and changed memory. The field store rejected the write,
but the before/after hooks still executed.

**Fix:** check the lock before either device hook. The runtime test checks both
hooks; the C API test locks the actual main SRAM controller, attempts INIT,
and verifies that its live memory is preserved. Five existing firmware outcomes
change because their startup closes these locks; see the validation section.

### F2. Observed DUT loads became reference state — model bug

**Locations:** [`dpi/eg2_sysmodel.cc`](../dpi/eg2_sysmodel.cc),
`eg2_sysmodel_load_event`; [`ip/eg2_core_system.h:80`](../ip/eg2_core_system.h#L80),
PLIC claim handling.

**Source:** the review contract requires independent predictions. The
[`PLIC theory of operation`](../../../ip_autogen/rv_plic/doc/theory_of_operation.md),
“Interrupt Claims” and “Interrupt Completion”, defines pending/claimed state
from requests, claims and completions.

**Failure:** an unpredicted DUT load could make unknown RAM known or overwrite
known RAM. A DUT PLIC claim different from the model's claim could replace the
model's gateway decision, concealing the divergence. Rolling back a consumed
pending pulse could also lose it after its source went low.

**Fix:** the observation API remains available but does not alter reference
state. PLIC claims come only from the model. Tests cover unknown RAM, known
RAM, a pending pulse, and an observed competing claim. The legacy adoption
counters remain ABI-compatible and return zero.

### F3. DPI bitmap and meta SRAM had separate storage — model bug

**Location:** [`dpi/eg2_sysmodel.cc:385`](../dpi/eg2_sysmodel.cc#L385).

**Source:** [`cheriot_mem_sys` theory of operation](../../../../../hw/ip/cheriot_mem_sys/doc/theory_of_operation.md),
the revocation bitmap and meta-memory connections, and the Earl Grey address
map in [`top_earlgrey.hjson`](../../../data/top_earlgrey.hjson).

**Failure:** writing a bitmap word, then initializing meta SRAM, left the old
bitmap word visible through the library. The standalone platform already
shared the storage. Both windows start at `0x11000000`; the first 3 KiB is the
bitmap, selected first by the decoder.

**Fix:** share both memory and knowledge flags after constructing all windows.
The C API regression writes the bitmap, initializes meta SRAM, checks the
cleared word and exercises explicit knowledge invalidation.

### F4. ALERT_TEST ignored its write-once lock — model bug

**Locations:** [`eg2_ip_model.h:65`](../eg2_ip_model.h#L65) and its `on_write()`.

**Source:** [`util/reggen/reg_block.py`](../../../../../util/reggen/reg_block.py),
generated `ALERT_TEST.regwen` description: writing zero disables alert testing
until reset. Same-write ordering was confirmed in `uart_reg_top.sv` and is
explicitly identified in the implementation comment.

**Failure:** after clearing bit 31, another test write still raised an alert.

**Fix:** detect the optional lock from field metadata and gate alerts with its
pre-write value. Older board descriptions without this field keep working.
The regression checks ordinary testing, a final alert while clearing the lock,
a blocked subsequent write and reset recovery.

### F5. GPIO input handlers did not identify the bank — model bug

**Location:** [`ip/eg2_periph_gpio.h:76`](../ip/eg2_periph_gpio.h#L76).

**Source:** the two GPIO instances in `top_earlgrey.hjson`, and GPIO
[`theory of operation`](../../../ip_autogen/gpio/doc/theory_of_operation.md),
input sampling and edge interrupts. API inputs must reach the corresponding IP.

**Failure:** both banks registered the same stimulus name, so the last bank
replaced the first bank's handler. The library's old `gpio.in` special case
updated a stimulus dictionary without updating the GPIO's live input state.

**Fix:** register `gpio0.in` and `gpio1.in` for both numeric API inputs and string
stimuli. Keep `gpio.in` as a bank-0 alias. Apply the same instance naming to
pad expectations. Tests verify separate bank values and an input edge interrupt.

### F6. An all-high GPIO output suppressed the next output item — model bug

**Location:** [`ip/eg2_periph_gpio.h:240`](../ip/eg2_periph_gpio.h#L240).

**Source:** GPIO output/output-enable semantics and the model's ordered output
contract: each change in driven pads must produce an item.

**Failure:** `0xffffffff` represented both a valid driven value and “no previous
value”. Driving every pad high, then low, omitted the transition from the log.

**Fix:** use a separate validity flag. The runtime regression observes the
all-high to all-low transition and its output-enable mask.

### F7. PLIC threshold incorrectly prevented software claims — model bug

**Location:** [`ip/eg2_core_system.h`](../ip/eg2_core_system.h),
`Eg2RvPlic::reg_before_read()`.

**Source:** PLIC theory of operation, “Interrupt Claims”: the target threshold
affects notification, not the claim operation.

**Failure:** with an enabled pending interrupt at priority 2 and threshold 2,
the external interrupt correctly stayed low, but reading `CC0` returned zero.

**Fix:** keep threshold filtering in notification, remove it from claim
selection. The regression checks suppressed notification, successful polling
of `CC0`, and consumption of the pending request.

### F8. Replay could pass without verification evidence — model bug

**Location:** [`dpi/eg2_sysmodel_replay.cc`](../dpi/eg2_sysmodel_replay.cc),
`read_lines()` and final verdict calculation.

**Source:** the replay's own interface contract: compare retired accesses and
the requested output files. A successfully retired access must not receive a
model bus error.

**Failure:** an empty trace passed. A trace with no comparisons passed.
A missing expected-output file was treated as an empty file. Bus errors were
counted but did not affect the verdict.

**Fix:** require instructions and at least one comparison; reject unreadable
expected files, mismatches, bus errors and model stops. The six replay cases
include valid evidence and deliberately corrupt or absent evidence.

### F9. TBRE completion selected an undocumented sweep_err value — documentation gap

**Location:** [`ip/eg2_cheriot_mem_sys.h:426`](../ip/eg2_cheriot_mem_sys.h#L426), `mirror()`.

**Source:** [`programmer's guide, Errors`](../../../../../hw/ip/cheriot_mem_sys/doc/programmers_guide.md#errors)
and theory of operation, revocation errors and `TBRE_EPOCH`. Errors set
`sweep_err`, software clears it with a write-one, and a faulty sweep does not
advance the epoch. The documents do not say whether completion reasserts the
bit after software cleared it during that sweep.

**Failure:** the shared CMS core left the completion value open using a care
mask, but the adapter selected zero and continued.

**Fix:** request `undocumented:` at completion instead of presenting an exact
result. The question is recorded in the verification workspace's
`status/dv_tickets.md`, “TBRE sweep_err cleared during a sweep”. The runtime
regression injects a read error during a 32-capability sweep, clears the bit,
finishes the sweep, and requires the stop and unchanged epoch.

The adapter's `configure()` rebuilds the core and must run before programming
TBRE registers. Current board setup follows that precondition. Sweep bounds
remain calculated in both the adapter and core; exposing the core's accepted
sweep span would reduce this duplication, but requires coordination with its
owner. The core's cumulative alert count deliberately survives `dut_reset()`;
it is not a reset-counter defect.

## Open findings and limits

These are not fixed by the changes above. They must remain visible in any
public description of what this model verifies.

| Finding | Source and concrete consequence | Classification / next action |
|---|---|---|
| ROM expected digest is derived from the computed digest | `ip/eg2_rom_ctrl.h:78` selects `expected = v` unless a synthetic corruption flag is set. ROM controller theory of operation says the expected hash comes from the top eight ROM words. Changing those words alone therefore cannot exercise the real comparison. | Model fidelity bug. Supply an independently specified ROM representation including ECC/expected digest, or stop when this check is required. The current synthetic failure switch does not prove ROM-image validation. |
| OTP digest uses FNV-1a | `ip/eg2_otp_ctrl.h:361` and `ip/eg2_otp_ctrl_core.h:553` use a model digest. OTP theory of operation, “Scrambling Datapath”, specifies a PRESENT-based Davies–Meyer construction. A DAI digest result cannot be compared with the chip's digest. | Model fidelity bug, already disclosed in source. Implement the documented algorithm and configuration before claiming digest equivalence. |
| Key derivation uses stand-ins and omits message padding | `ip/eg2_keymgr_dpe.h:22` documents substituted constants, missing padding and OTP-share handling. Key manager theory of operation, “Key Derivation”, specifies the inputs and zero-padding to `AdvDataWidth`. Matching command status does not imply matching keys. | Model fidelity bug. Requires an explicit source for constants and byte-accurate input construction; crypto was not rewritten in this review. |
| Power glitch during low-power entry chooses a reset classification | `ip/eg2_pwrmgr_core.h:151` explicitly assumes `LOW_POWER_EXIT`. Power-manager theory of operation, “Main Power Unstable Reset Requests”, requires an internal reset but does not settle that boundary classification. | Documentation gap still requiring a stop/ticket and clarification; currently an unresolved violation of the review's no-guessing rule. |
| Register-only fallback is not an implemented engine | `eg2_ip_models.cc` falls back to `Eg2IpModel`; the generic register layer does not implement shadow-write sequencing. Some specialized cores do. A register readback test alone cannot establish either engine or shadow semantics. | Coverage limitation. Inventory remaining generic instances and audit each shadowed register before making whole-top coverage claims. |

No suspected RTL defect is asserted by this review. Timing constants and the
RTOS slow-memory scaling remain functional scheduling approximations. They
were not tuned to obtain passing results. CPU instruction semantics, all
crypto vectors, all peripheral modes and reset interleavings have not received
an exhaustive proof or line-by-line audit here.

## Readability and crossbar scope

The address decoder is 98 lines, with 56 windows in the tested Earl Grey build.
It forwards to one selected target; it does not allocate an RTL crossbar matrix.
The generated register table is 7,376 lines of data. Sparse register offsets
do not allocate a full register array across the corresponding address span.

The cleanup names the decoder operations clearly, replaces historical phase
labels in the IP registry with functional groups, explains the shared layers
in [architecture.md](architecture.md), and makes the supported workflows and
limits visible in the README. Generated data, imported USB code and existing
dirty CPU/platform work were not blanket-reformatted.

## Validation

All simulation batches used at most two concurrent simulator processes.

| Check | Result |
|---|---|
| Standalone executable, DPI library and replay | Control and revised builds succeed with the pinned model development shell |
| Existing standalone C++ core tests | All 15 pass before and after; all 15 deliberately failing variants are rejected by the new runner |
| Shared runtime regressions | Control: 57 checks, 12 failures (the missing GPIO input handler prevents four follow-up checks). Revised: 61 checks, no failures |
| Public C API regressions | Control: 20 checks, 10 failures. Revised: 20 checks, no failures |
| Replay regression cases | Revised: six expected outcomes, including rejection of a retired store receiving a bus error |
| RRAM DPI target | 1,293 checks pass; its deliberately broken expectation fails |
| Earl Grey firmware selection | 47 images. Control meets all expected outcomes. Revised: 42 unchanged, five SRAM outcomes change as detailed below |
| Earl Grey UART comparison | 47 UART files compared; exactly the same five SRAM runs differ |
| RTOS board | 23 locally available firmware runs: 132 passing checks and 12 expected mutation failures. All verdicts and all 23 UART files match the control |
| Main RTOS suite | 21/21 checks pass, including its overall verdict |
| Earl Grey fault mutations | Five clean passes, seven applied faults caught, three expected inactive faults; grades and 18 UART files match the control |
| Real DPI replay (`eg2_dif_smoketest`) | 195,920 instructions; 6,750 predicted loads match, zero mismatches or bus errors; all 177 UART items identical, control and revised |

The real replay contains 54,208 loads: 46,766 are unpredicted and 692 are
unchecked multi-register/no-destination cases. This is parity evidence for the
compared subset, **not** evidence that every load was checked.

The five SRAM runs are:

- `eg2_sram_readback_cheriot` and its `_inject_fail` twin;
- `eg2_sram_rekey_tag_cheriot` and its `_inject_fail` twin;
- `eg2_sram_scrambled_access_cheriot`.

Their common startup, `sw/device/lib/testing/eg2/cheriot/eg2_cheriot_start.S:184`,
closes all SRAM `CTRL_REGWEN` registers unless an image defines
`eg2_cheriot_boot_locks_open`. These tests then request INIT or key renewal.
The fixed model ignores those locked commands, as the HJSON requires. The two
negative twins now fail before the intended injected check; the scrambled
access run reaches its instruction limit. These are not successful negative
tests. Correct their boot setup explicitly and rebuild the firmware; do not
restore the model's bypass to recover the previous pass counts. No firmware
source or expected verdict file was changed in this review.

The RTOS runner reports two differences from its checked-in expectations:
`dbg_host.tag_clear` and `dbg_host.verdict` are now passing. Both are already
passing in the control, so neither is caused by this review. The additional
`rtos_prerelease` Nix-built image was excluded from the scratch suite after its
package acquisition did not complete; it is not included in the 23-run claim.
The full firmware collection, RTL benches and NIST vectors were not run.

### Reproducing the checks

From the verification workspace, with `REVIEW_OUT` set to a scratch directory:

```sh
nix develop .#model --command make -C eg2/hw/top_earlgrey/dv/eg2_model \
    OUT="$REVIEW_OUT/build" "$REVIEW_OUT/build/eg2sim" lib replay check rram_test

EG2SIM="$REVIEW_OUT/build/eg2sim" bash scripts/model_tests.sh \
    -o "$REVIEW_OUT/firmware" -j 2 \
    eg2_rand_regs eg2_gpio eg2_revocation eg2_tbre eg2_memsys \
    eg2_framework eg2_alert eg2_sram

python3 scripts/eg2_model_mutation.py --top eg2 \
    --eg2sim "$REVIEW_OUT/build/eg2sim" --out "$REVIEW_OUT/mutations" --jobs 2
```

Use the same firmware artifacts for the control, compare per-test verdicts
without depending on parallel output order, and compare UART files byte for
byte. The broader RTOS suite is
`eg2/hw/top_earlgrey/dv/eg2_sw_suites/cheriot-rtos/run.sh -j 2` with `EG2SIM` and
`SW_SUITES_OUT` set. It requires all images listed in its `runs.txt`.

## Subsequent file split

The structural cleanup uses a fresh control snapshot that already contains the
fixes above. Nine large files were split along existing device and implementation
boundaries. The old IP group headers remain include entry points. No generated
tables or CMS reference-core code changed.

| Original file | Original lines | Responsibility after the split |
|---|---:|---|
| `ip/eg2_mem.h` | 1,379 | Four controller headers and the memory factory entries |
| `ip/eg2_mem_otp.h` | 1,055 | OTP data, OTP controller, life-cycle controller and their integration methods |
| `ip/eg2_periph_sys.h` | 1,136 | Sysrst, ADC, pinmux and shared wake-up/logging helpers |
| `ip/eg2_crypto_entropy.h` | 952 | Entropy source, CSRNG, EDN and shared helpers |
| `ip/eg2_crypto_hash.h` | 740 | HMAC and KMAC |
| `ip/eg2_crypto_keymgr.h` | 790 | Key manager and OTBN register adapter |
| `ip/eg2_otbn.h` | 1,420 | 531-line state/interface header, arithmetic, sponge, instruction execution and accelerator implementations |
| `eg2sim.cc` | 1,093 | Board/run driver, image operations and stimulus parsing |
| `eg2_spike_cpu.cc` | 928 | CPU stepping/transport and a separate debug implementation |

Both executable and DPI builds succeed. All 21 new IP headers compile when
included individually. The 15 core tests and their negative checks, 61 shared
runtime checks, 20 C API checks and six replay cases pass. NIST reference vectors
pass, and the corrupted-vector run detects every deliberately altered file.

The firmware comparison has 47 actual runs and five stale images skipped by
the existing runner. Every verdict and all 47 UART logs match the fresh control,
including the five known SRAM failures. The 23 available RTOS runs likewise
have identical verdicts and UART logs. A real DPI replay matches all 6,750
predicted loads and 177 UART items in both builds; its unpredicted/unchecked
loads remain as described above.

The standalone OTBN core also ran the available KMAC and masking-accelerator
test ELFs before and after the split. Both terminate without model errors;
instruction/cycle counts, GPRs, WDRs, flags and the entire data memory are
identical. This checks the moved execution and accelerator implementations
without relying on the stale chip-level crypto firmware images.
