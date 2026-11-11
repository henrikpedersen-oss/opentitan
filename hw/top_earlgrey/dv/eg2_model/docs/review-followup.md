# Follow-up review — 10 November 2026

The later [fidelity pass from 5985a2a326](fidelity-review.md) addresses the four
remaining gaps below and records its complete 535-image verdict comparisons.
The findings and results here describe the earlier pass.

This pass starts from EG2 commit `da6c4d0e4d`, after the earlier cleanup and CMS
review fixes were checked in. It fixes shared transport, image-loading and OTP
digest defects, and removes one undocumented power-reset prediction. It does
not establish full RTL equivalence or resolve every known fidelity gap below.

The model is C++/SystemC. Its standalone executable supplies an instruction-set
simulator; its DPI library supplies transactions and predictions to an external
testbench. UVM phases, agents, sequences and scoreboards belong to that external
testbench. The model's address decoder is a small ordered window lookup, with
deliberate bitmap/meta-SRAM overlap; the large generated register table is data.

## Fixed defects

### R1. Malformed ELF files could be accepted as successfully loaded

**Files:** `eg2_elf.h`, `eg2_image.cc`, `dpi/eg2_sysmodel.cc`.

The old readers checked the magic and ELF class but silently substituted zero
for out-of-range header fields. A truncated program table could therefore
become an apparently successful empty load. Symbol readers duplicated this
logic, did not constrain names to their string table, and used host-endian
`memcpy` to interpret little-endian file fields.

One SystemC-independent `ElfImage` now validates ELF32 little-endian RISC-V
headers, table strides and bounds, loadable segments, address wraparound,
symbol links and string termination. Both front ends use it. Destination
validation precedes memory writes so an invalid later segment does not leave
a partially loaded image. Physical load addresses remain distinct from runtime
symbol addresses; firmware startup remains responsible for BSS initialization.
Extended ELF numbering is explicitly unsupported.

**Evidence:** 71 parser checks; the replay's new malformed-image case fails on
the control and is rejected by the revised loader. The replay's empty fixture
was corrected to contain a valid ELF header rather than relying on the defect.

### R2. Unsupported TLM payloads could perform real register writes

**Files:** `eg2_transaction.h`, `eg2_regblock.h`, `eg2_targets.h`, `eg2_router.h`.

A register target treated any non-read command as a write, truncated a 64-bit
address to 32 bits and ignored byte enables and streaming widths. Consequently
an IGNORE command, an address of `0x100000000`, or an unsupported masked/burst
write could change register zero and invoke device hooks.

Targets now reject unsupported payloads before touching storage or invoking
hooks. A hook that calls `fail_access()` prevents subsequent storage and
after-access actions even if its boolean return would otherwise allow them.
This also prevents a failed read from consuming a read-clear field or FIFO.

**Evidence:** the transport regression observes storage and hook counts;
malformed transactions fail against the control implementation.

### R3. Overflow and DMI could bypass memory bounds and alias restrictions

**Files:** `eg2_targets.h`, `eg2_router.h`.

`offset + length` could wrap, allowing an oversized backdoor copy. A zero-sized
memory could advertise a DMI range ending at `UINT64_MAX`. The router forwarded
the backing allocation's DMI extent without clipping it to the decoded window.
A DMI request beyond the bitmap alias could also expose the whole meta SRAM,
bypassing the earlier window's word-write restriction on subsequent accesses.

Range checks now use subtraction, empty/out-of-range DMI requests are refused,
and returned DMI ranges are clipped to the selected window. A backing window
overlapping an earlier window stays on the checked transport path. Zero-length
backdoor copies are safe; zero-length bus transactions are rejected.

**Evidence:** 16 transport checks. The control fails the boundary and alias
checks; AddressSanitizer also detects its oversized copy. Revised transport,
ELF and OTP utility tests pass with AddressSanitizer and UndefinedBehaviorSanitizer.

### R4. OTP used the wrong digest and accepted corruption present at reset

**Files:** `ip/eg2_otp_crypto.h`, `ip/eg2_otp_ctrl.h`,
`ip/eg2_otp_ctrl_core.h`.

The [OTP theory of operation](../../../ip_autogen/otp_ctrl/doc/theory_of_operation.md),
“Digest Calculation”, requires PRESENT Davies–Meyer, duplicated trailing
64-bit blocks and a finalization round. Both model implementations used FNV-1a.
The plain C++ core also calculated its reference checksum from the image at
reset, so data corrupted after digest programming became its new expectation.

The shared `OtpCrypto` implements the specified construction using the public
`RndCnstDigestIV0`, `RndCnstDigestConst0` and scrambling keys from
`hw/top_earlgrey/data/autogen/top_earlgrey.secrets.testing.gen.hjson`.
The two duplicated PRESENT implementations were removed. Both DAI digest paths
and synthetic token provisioning now use the same real digest algorithm.

The plain C++ core checks the independently stored digest at power-up and on
subsequent integrity requests, as specified under “Buffered Partition”. It
withholds corrupt partition outputs and raises the fatal check alert. Its
previous test changed a token after DIGEST but still expected valid outputs at
reset. Valid provisioning and corruption are now separate test cases.

**Evidence:** PRESENT encryption/decryption known answers, six partition-digest
vectors from the independent Python PRESENT reference, a DAI known answer for
the eight-byte HW_CFG1 payload, and reset-corruption checks. The extended core
test has three failures against the control and none against the revised core.

**Scope:** the SystemC OTP controller remains a separate implementation. It
shares the corrected digest, but does not acquire the plain core's complete
buffer-integrity and zeroization behavior through this change. A misleading
comment claiming it was already a thin adapter was corrected.

### R5. Deep-sleep entry glitch guessed a reset classification

**Files:** `ip/eg2_pwrmgr_core.h`, `ip/eg2_aon.h`, `dpi/eg2_pwrmgr_dpi.cc`.

The [power-manager theory of operation](../../../ip_autogen/pwrmgr/doc/theory_of_operation.md),
“Main Power Unstable Reset Requests”, defines the internal reset and sticky
fault bit but not the exact `LOW_POWER_EXIT` classification during entry.
The core previously assumed that classification.

The core now reports that boundary as unclassified, while preserving the
documented fault bit. The SystemC binding requests an `undocumented:` stop.
The DPI binding cannot currently inject a callback inside entry; it nevertheless
handles that result by withdrawing register predictions if such a callback is
added later. Active-state glitch resets retain their documented prediction.
The question is recorded in the workspace's `status/dv_tickets.md`.

**Evidence:** a directed reentrant entry callback checks the unclassified
result, absence of a guessed reset, sticky fault bit and normal active-state
reset behavior.

## Remaining fidelity gaps

These are substantive limits on using the model as an independent verification
reference, not cosmetic follow-up work.

| Priority | Finding | Consequence / next step |
|---|---|---|
| High | `ip/eg2_rom_ctrl.h::check()` computes a synthetic expected digest from its own computed value, with ECC bytes replaced by zero. | A passing boot does not validate the ROM image's stored expected digest. Supply the actual checker input, including ECC and independent expected words, or stop when image-integrity validation is required. Synthetic fault injection only tests the response to a declared failure. |
| High | `ip/eg2_keymgr_dpe.h` substitutes derivation constants and omits fixed-width message padding. Its `CFG_REGWEN` life-cycle gating is opt-in, although commands separately check life-cycle enable. | Command/status and sideload tests do not prove correct key derivation. Resolve all specified inputs and audit the register-gating policy before using this as a key oracle. OTP share descrambling is already implemented; the old key-manager header claiming otherwise was stale. |
| High | AES, KMAC and key-manager shadowed CSRs reach ordinary generic register storage. | A first write can commit without a matching second write. Alert-handler and clock-manager cores implement their own shadow protocol; this is not a whole-model feature. Add field-level staging, read/reset cancellation and each IP's specified update-error response, with independent tests. |
| High | `Eg2OtpCtrl` and `Eg2OtpCtrlCore` remain separate controller implementations. | The plain core is used by its self-test, not by the standalone/DPI system controller. Its buffer checks and zeroization coverage cannot be claimed for full-chip runs. Consolidate through a tested binding, including life-cycle ordering and hardware-output validity. |
| Medium | `ast`, `rram_macro.prim` and `rv_dm.mem` use the register-only factory fallback in the Earl Grey table. | The presence of their register windows does not establish complete analog, macro or debug-memory behavior. Check the relevant separate interfaces and stimulus paths for each use case. |

Source inspection covered the shared platform, CPU/ISS handover and transport,
IP registry, major IP groups, DPI loader/prediction path and RTOS board bindings.
CPU instruction semantics and all device modes were not exhaustively audited.
No RTL defect is asserted by this pass. Generated headers, the shared CMS core,
firmware sources and expected firmware verdicts were not changed.

## Validation

Control and revised builds use the same pinned SystemC, Spike and MPACT tools
and the same firmware artifacts. Evidence is retained in the review workspace
under `/tmp/eg2-community-review-1110`; that temporary directory is not a public
dependency of the tests.

| Check | Result |
|---|---|
| Standalone executable, DPI library and replay | Build successfully |
| Existing device-core tests | 15 pass; all 15 negative variants rejected |
| Shared runtime / public C API | 65 / 20 checks pass |
| ELF / OTP crypto / transport | 71 / 11 / 16 checks pass; sanitizer runs pass |
| Replay cases | Seven expected outcomes, including rejection of malformed ELF |
| Earl Grey firmware | 59 images; all verdicts and all 59 UART files identical to control |
| Known firmware failures | The same five SRAM outcomes fail in control and revised builds; these are not new regressions or successful negative tests |
| RTOS board | 23 runs, 132 passing checks and 12 expected mutation failures in each build |
| Real DPI replay | 195,920 instructions; 6,750 predicted loads match, no mismatches or bus errors; all 177 expected UART items match |
| RRAM standalone DPI checks | 1,293 pass; the deliberately broken variant fails |
| Earl Grey fault mutations | Five clean passes, seven applied faults caught, three expected inactive faults in both builds |
| NIST CAVP reference checks | All available vector files pass; corrupting one expected result per file is detected in every file |

The real replay also has 46,766 unpredicted and 692 unchecked loads. Its success
is evidence only for the compared subset. The RTOS runner exits 1 in both builds
because `dbg_host.tag_clear` and `dbg_host.verdict` pass while its checked-in
expectations still say fail. The unavailable `rtos_prerelease` image is excluded
from the 23-run scratch selection. No RTL simulation or complete firmware
collection was run in this pass.

Reproduce local regressions with `make check` in the model development shell.
See [CONTRIBUTING.md](../CONTRIBUTING.md) for the validation contract and the
[earlier review](review.md#reproducing-the-checks) for firmware runner commands.
