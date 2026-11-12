# C-to-RTL equivalence: memory subsystem decisions

Run `make c2rtl-cms` in eg2_top_verification. Its own pinned `c2rtl` shell supplies
Bambu and Yosys with yosys-slang; no `C2RTL_FLAKE` override is needed. Output is in
`scratch/c2rtl/cms/` under that repository (or its configured `SCRATCH`). Direct use
of this directory's Makefile accepts `BUILD=/absolute/path`.

`make all` runs the revocation proof and its two negatives, then `make rules` runs
the additional address and WTRC proofs below. Every positive checks both equivalence
and HLS completion within 64 cycles. Every negative must produce a SAT counterexample;
a compiler, elaboration or solver error is a failed trial, not a caught mutation.

## Revocation decision

`make equiv` proves that the C reference model's revocation decision is the RTL's. The model
(`../cmodel`) decides, for one capability, whether its base needs a bitmap lookup and which bitmap
word and bit (a sealing capability never; a base outside the heap the bitmap covers never; otherwise
bit `(base - heap_base) >> 3`). `trvk_decision_hls.c` calls the model's own capability decoding
(`../cmodel/cms_cap.h`, shared with `cms_core.h`), and Bambu turns it into Verilog. Yosys proves it
equal to `cheriot_trvk_core` (`trvk_decision.ys`, the miter `trvk_decision_equiv.v`): the RTL's
stored pointer and the decision's flags are cut to free inputs (`expose -cut`), so the proof holds
for every pointer, capability word 1 and heap base, with the subsystem's bitmap (3072 bytes at
`0x1100_0000`). A second proof shows the HLS transaction finishes, so the comparison is not vacuous.

`make negative` runs the same proof on two broken copies, each of which must fail: the out-of-range
check off by one (`>` for `>=`), and a 4096-byte bitmap.

The method and tool pins come from the AES flow (`eg2-git-proposal`, `make c2rtl`).

Requirement: REQ_CMS_08 (the revocation decision of the subsystem's filter). Not covered: the
core's own filter in Ibex (a separate copy, ticket #1011), and the response side (the tag cleared
on a set bit, an error or an integrity fault), which the UVM bench checks.

## Address and WTRC decisions

`cms_rules.h` is plain C shared with the transaction model in `cms_core.h`.
The three `*_hls.c` wrappers call those functions. `run_rules.py` elaborates the
production RTL with the existing edagen file list, exposes the selected outputs and
cuts only the stated WTRC history inputs. It generates the miters and Yosys scripts
under `BUILD/rules`, with logs, negative counterexample waveforms and `summary.json`.
No RTL formula is copied into a substitute implementation for comparison.

| Proof | Scope | Negative |
|---|---|---|
| Access checker | `allow_forward`: all 32-bit addresses, 16 mode encodings, opcodes and sizes; bitmap and tag windows on both maps | Inclusive upper bound instead of exclusive |
| Tag mapping | Tagged/NVM classification, absolute meta-word address and bit selection for every 32-bit address, including unaligned and unmapped addresses, on both maps | Wrong SRAM bit; wrong meta-word stride |
| WTRC match | W0/W1 sequencing, refusal, data and seven-bit integrity comparison, response error and tag-request decision | Ignore failed W0; allow tag request without `d_valid` |

The maps are the default EG2 map (192 KiB SRAM, 2 MiB NVM) and the UVM `cms_map`
(16 KiB SRAM, 8 KiB NVM), with their derived bitmap and tag windows. The access
checker proof covers the common rules used by the core, system and TBRE bitmap
checkers and the tags checker. PutFull mask legality remains a separate transaction
model check: the RTL access checker tests opcode and size, not the mask.

The WTRC proof supplies free previous capability address, open/closed transaction
history, previous W0 match, command-error flag, request fields and response fields.
For sequence checking, `CapIdle` corresponds to closed history and `CapW0` to open
history. The expectation FIFO's head is the same request's independently preserved
data and integrity; its RTL-computed refusal bit is also checked against C. This is
a proof of the decisions, not of FIFO transport or reachable FSM history. The C
side takes only the free inputs, never an RTL output. Response integrity decoding,
alerts, pipelined `CapW1`/`CapTagWr` handoffs and complete protocol timing remain
outside this proof. The invalid-response negative exercises the decision changed
by RTL ticket 11, but is not a complete `d_ready` protocol proof.

The mapping proof supports the address part of REQ_TF_05/06; the WTRC proof supports
the matching/refusal decisions of REQ_WTRC_05–08 and 10–12. These partial proofs do
not mark those whole requirements complete. All comparisons are two-state, for
one latched transaction. The 64-cycle bound covers the HLS computation, not an RTL
transaction timeout. Black-box simulation checking is unchanged: exposed RTL nets
are confined to these formal miters and never feed the reference model or scoreboard.

## Full-system HLS trial

`cms_system_hls_trial.cpp` is a synthesis feasibility wrapper for the actual persistent
`cms::Model`, including core and bitmap accesses, CSRs, reset, sweep events and fault
alternatives. It is separate from the passing decision-proof targets: it does not
contain an RTL miter and cannot report an equivalence pass. Its inputs are model
transaction events, not cycle-level TL-UL ports.

A full-system comparison must also connect the production RTL's external ports to
independent memory models, track accepted transactions, and compare responses with
the model's allowed tag sets and data-care masks. HLS completion by itself would not
prove that protocol correspondence. Dynamic storage and set-valued race outcomes
must remain part of the model contract; using DUT internals to resolve them is not
allowed. Trial results are recorded in eg2_top_verification's memory subsystem
report and `evidence/cms-system-hls-1111/`, separately from the TRVK HLS report.
