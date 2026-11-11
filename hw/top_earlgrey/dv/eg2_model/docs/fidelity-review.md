# Specification-based fidelity review

The fidelity changes were developed against `5985a2a326` and reapplied without conflicts onto
`6ed6886f4b` (`claude-1110`). Current worktree on nyx:
`~/work/eg2-codex-week-model-1110`. No changes have been committed by Codex.

The fresh comparison against `6ed6886f4b` is below. Earlier per-stage measurements are historical;
the old OTP-2 classification was incorrect and is explicitly corrected in section 4. Separate SRAM
test edits implement Henrik's option-2 decision and are validated after the fidelity comparison.

## 1. Independent ROM checker input

The old model computed both DIGEST and its supposed expectation from the ELF.
The checker now hashes all 39 stored bits of each body word, padded to five
little-endian bytes, and compares with the low 32 bits of the final eight stored
words. Sources: `hw/ip/rom_ctrl/doc/theory_of_operation.md`, startup ROM check;
`hw/top_earlgrey/doc/design/README.md`, lifecycle bypass. The VMEM converter uses
the existing image producer only to decode its input format. It checks the
ELF/VMEM pairing and never computes a replacement expectation.

This is a behavior change: a corrupt stored digest or integrity bit can now
fail boot. An absent checker input stops as not modelled; missing cSHAKE N/S
configuration stops as undocumented. The configuration is explicit because the
specification supplies a prefix but does not define the N/S split (ticket ROM-1).
The public test-image producer uses N empty and S `ROM_CTRL`.

Validation: build, all core/API/transport/parser/replay checks, and 10 ROM
checks pass. ROM known answers were generated independently with PyCryptodome;
the tests change stored digest bits, integrity bits and ELF binding separately.

Full suite: `scripts/model_tests.sh -j 12`, all 535 frozen images, before and
after. **Zero verdict changes**. Both runs have nine failures, zero stale and
zero excluded images. These are failures observed at the specified base, not
new failures dismissed as known:

- `eg2_sram_readback_cheriot_inject_fail`
- `eg2_mem_sys_e2e_cheriot`
- `eg2_sram_rekey_tag_cheriot_inject_fail`
- `eg2_trvk_truth_table_cheriot`
- `eg2_rv_dm_code_patch_cheriot`
- `eg2_connectivity_cheriot`
- `eg2_sram_readback_cheriot`
- `eg2_sram_rekey_tag_cheriot`
- `eg2_sram_scrambled_access_cheriot`

Evidence lives in the sibling `evidence/` and `stages/` directories on nyx;
`00-suite.log`, `01-suite.log`, `01-verdict-diff.txt`, image SHA-256 manifests,
and per-image logs preserve the comparison. This does not establish equivalence
to RTL or validate unspecified checker timing and FSM faults.

## 2. AES, KMAC and keymgr shadow registers

The previous model committed each bus write immediately. These three IPs now
opt into one implementation in `eg2_regblock.h`: first-write staging, second-write
comparison per field, read/reset cancellation, and REGWEN rejection before
staging. Only matching fields commit; reserved bits do not participate. Device
side effects run on commitment. Alert handler and clock manager retain their
existing standalone-core implementations.

Sources: `util/reggen/README.md`, Shadow registers; the three IPs' HJSON register
and alert descriptions. AES raises `recov_ctrl_update_err` and STATUS bit 5;
KMAC raises `recov_operation_err` and STATUS bit 29. Successful control rewrites
clear those recovery indications; a message cannot start before recovery.
Keymgr sets asynchronous `ERR_CODE.INVALID_SHADOW_UPDATE`, cleared explicitly
by software, and raises `recov_operation_err` without completing an operation.
AES CTRL/GCM busy writes do not stage; AUX follows its documented REGWEN.
Storage-fault injection and cycle timing remain outside this implementation.

This is a behavior change even though the full firmware verdicts are unchanged.
121 bus checks cover staging, cancellation, all four keymgr shadow registers,
partial-field mismatch, alerts/status, recovery, busy rejection and REGWEN.
Build and the full directed/API/replay suite pass. The final 535-image run has
**zero verdict changes** relative to the ROM stage, with the same nine failures.
See `02-verdict-diff.txt` and `02-shadow-final-suite.log` on nyx. An earlier
intermediate run is retained as evidence but is not the final-stage comparison.

## 3. Keymgr configuration and derivation boundary

Lifecycle/CFG_REGWEN gating is always enforced and refreshed before writes,
including writes with no preceding status read. The busy lock applies during
synchronous operations. The old stimulus cannot disable the policy. Root loading
uses the specified max-key-version zero. Advance destination validity follows
the documented retain-parent rule; only the two explicitly documented locks
are unlocked after advance.

The documents do not define AdvDataWidth/GenDataWidth values, byte serialization
or padding placement, and their seed-source description disagrees with the old
RRAM binding. Ticket KEYMGR-1 records these missing inputs. Derivation now stops
as undocumented before slot/output mutation or fabricated completion. The old
SHAKE-based netlist stand-ins and unpadded KDF are removed; no chip-key equivalence
is claimed. Padding and resulting keys remain **unmodelled pending documentation**.

Validation: 35 keymgr bus checks pass, alongside the existing directed checks.
All 535 images ran. **36 PASS verdicts became FAIL/model stop**, including 18
injected-failure images which no longer reach their intended failure checks.
The nine prior failures remain, for **45 total**. This is a behavior change and
a deliberate coverage loss, not a passing regression or an RTL finding.

| Image | Verdict change | Observed reason |
|---|---|---|
| `eg2_keymgr_dpe_diversification` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_diversification_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_diversification_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_diversification_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_locks` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_locks_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_locks_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_locks_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_sideload_kmac_error` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_sideload_kmac_error_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_sideload_kmac_error_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_keymgr_dpe_sideload_kmac_error_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_otbn_keymgr` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_otbn_keymgr_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_otbn_keymgr_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_otbn_keymgr_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_fault` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_fault_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_fault_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_fault_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_sb` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_sb_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_sb_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_keymgr_sb_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_sym_crypto_sb` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_sym_crypto_sb_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_sym_crypto_sb_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_pbf_sym_crypto_sb_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_reset_residue` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_reset_residue_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_reset_residue_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_reset_residue_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_rma_key_change` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_rma_key_change_cheriot` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_rma_key_change_cheriot_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |
| `eg2_sec_rma_key_change_inject_fail` | PASS → FAIL (rc 2) | undocumented: keymgr_dpe advance: AdvDataWidth and padded input layout (docs name the widths but do not define them; KEYMGR-1) |

## 4. One OTP controller in full-chip runs

`Eg2OtpCtrl` is now a SystemC binding around `Eg2OtpCtrlCore`; there is one
partition table and one DAI/check/zeroization implementation. Hardware consumers
use checked reset-sampled buffers. Runtime lifecycle programming updates the
core's OTP array and lifecycle buffer together; provisioning remains a backdoor
that takes effect at reset. The debug-disable input also uses the hardware buffer.
The LC startup handshake no longer treats the temporary pre-decode Invalid value
as a real escalation back into OTP.

Initial and explicit integrity/consistency checks now affect full-chip outputs,
error CSRs, interrupts and fatal alerts. DAI ZEROIZE is available in full-chip
runs. EG2's RRAM OTP Hardware Plug specifies read-set-write: programming can set additional
bits but must reject a request that clears a stored bit (`hw/ip/rram_ctrl/doc/theory_of_operation.md`,
lines 300–316). The model enforces that rule. Applying the generic fuse guide's prohibition on
reprogramming was a **model mistake**, corrected before this rebase. The low-power OTP tests were
right to expect the monotonic writes to succeed; the former OTP-2 test-bug claim is withdrawn.

OTP-1 records remaining boundaries: partial zeroization markers, contradictory
ZEROIZE RDATA descriptions, unspecified failed-read data, simultaneous command
bits and failed multiword LCI contents. Those observations stop explicitly.
Enabling randomized periodic checks stops as not modelled; explicit check
triggers are supported. The model does not implement macro ECC or physical
programming faults. It is not a timing or complete hardware-equivalence oracle.

Validation: 123 OTP core checks and 41 adapter checks pass with the existing directed/API/replay
checks. The earlier core also passed AddressSanitizer and UndefinedBehaviorSanitizer. The eight
OTP-1 suite stops involved reads of RDATA after a refused access. Commit `6ed6886f4b` fixes the
helpers and removes undocumented zero-value assertions while retaining access-error checks.
All eight stops disappear in the fresh comparison.

## Current comparison against 6ed6886f4b

Both executables used the same current MPACT dependency and the same 535 firmware images,
including the 12 rebuilt OTP-1 images. No stale or excluded images.

| Stage | Images | Passed expectations | Failed expectations | Change |
|---|---:|---:|---:|---|
| Committed baseline 6ed6886f4b | 535 | 528 | 7 | baseline |
| Fidelity pass | 535 | 492 | 43 | 36 PASS to undocumented keymgr stop |
| Approved SRAM option 2 | 535 | 499 | 36 | Seven FAIL to PASS; test behavior changed |
| Final F6 integration | 535 | 499 | 36 | No verdict changes |

The 36 changed image names are listed in section 3. The stop occurs before the test can exercise
its derivation-dependent checks; a negative image that never reaches its intended failure is
counted as a failed expectation. These stops do not establish RTL failures. KEYMGR-1 is drafted
in the parent project's `status/rtl_tickets.md`; Henrik files tickets.

Before the approved test changes, the seven unchanged failures were `eg2_sram_readback_cheriot` and its `_inject_fail` twin,
`eg2_sram_rekey_tag_cheriot` and its `_inject_fail` twin, `eg2_mem_sys_e2e_cheriot`,
`eg2_connectivity_cheriot`, and `eg2_sram_scrambled_access_cheriot`. Their requests to INIT or
rekey follow the boot stage's CTRL_REGWEN lock. Henrik approved testing the ignored commands in
CHERIoT mode and retaining active INIT/rekey checks in RISC-V mode. All seven now pass. This is a separate test behavior change; the documented register lock stays enforced.

`eg2_trvk_truth_table_cheriot` and `eg2_rv_dm_code_patch_cheriot` now pass both builds with
unchanged test images. The rebuilt dependency supplies the existing MPACT clear-capability-tag
and FENCE.I hooks; the earlier executable used an older library. This is a model dependency issue,
not evidence of an RTL defect.

## Evidence and limits

`~/work/eg2-codex-week-1110/evidence/fidelity-verdict-diff.md` names every changed verdict.
`model-input-sha256.txt` records the inputs; `stages/base/full-suite` and
`stages/fidelity/full-suite` contain every image's logs. `fidelity-rebased.patch` preserves the
source used for this comparison. Earlier stage logs remain under `eg2-codex-fidelity-1110`.

A real `eg2_dif_smoketest` trace was generated and replayed in the earlier pass through its base
and final DPI libraries: 195,920 instructions, 6,750 predicted loads matched, no mismatches or bus
errors, and all 177 expected UART items matched. The standalone UART logs were identical.
46,766 loads were unpredicted and 692 unchecked; replay success does not validate those loads or
prove hardware equivalence. This historical replay does not replace validation of later changes.

The derivation contract still needs a documented answer. No netlist stand-in is advertised as a
chip key. Expected verdict files and generated source headers are unchanged.

## Compatibility skips and the strict memory audit

The full comparisons above use the runner's established `model_gaps=1` setting for images that
expose it. The setting is now printed explicitly and can be disabled with `MODEL_GAPS=0`.
An additional 50-image audit, including the 36 core memory/revocation images, gives identical
32 passing and 18 non-passing expectations on base and final builds with skips disabled.
Thirteen bench actions are unsupported, two access-fault checks fail, two debug-halt cases hit
the instruction limit, and one TBRE word-integrity case stops on a specification question.
These are pre-existing coverage limits; the full-suite pass count must not hide them.
Every affected image and diagnostic is in the handover's `evidence/no-gap-audit.md`.

F6 adds independent tag-response relations (stable reads and W0/W1 agreement), without feeding
observed values into functional model memory. CMS unit and fixed-seed UVM checks pass. Q43
remains unjudged after integrity-only faults; a d_error still requires preservation of the old tag.
