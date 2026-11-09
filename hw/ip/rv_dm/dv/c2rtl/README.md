# C = RTL proof of rv_dm's Access Register abstract commands

An Access Register command makes `dm_mem` build a ten-instruction program that the halted hart
runs. This proof shows that, for every command the debugger can write, what that program does
on an EG2 hart is what the documents say the command does.

- `dm_ar_semantic.c`: the documented effect, from the RISC-V Debug Specification 0.13.2 (3.6.1.1,
  regno table 3.3) and `hw/ip/rv_dm/doc/theory_of_operation.md` ("CHERIoT Mode"). Bambu
  synthesises it to Verilog.
- `dm_ar_check.sv`: the real `dm_mem` with the EG2 parameters. The program is read word by word
  over its hart bus port. A symbolic interpreter, written from the instruction set documents,
  runs it on an EG2 hart whose registers hold symbols. It compares the result with the C:
  - every GPR
  - data0
  - the one CSR or SCR written
  - how the program ends (ebreak, or into the program buffer for `postexec`)
  - cmderr 2 for unsupported commands
- `dm_ar_equiv.v`: the miter; `equiv.ys`: the Yosys SAT proofs.

Run (in the c2rtl shell of eg2-git-proposal):

    nix develop <eg2-git-proposal>#c2rtl -c make equiv equiv-negative

`equiv` proves the documented register numbers (`restrict_i` 1) and that the Bambu result is
compared. It then reports all register numbers (`restrict_i` 0) and three named cases, where a
counterexample is a finding. `equiv-negative` makes `dm_mem` store reads as words whatever
`aarsize` says, and checks that the proof fails.

Outside the comparison:
- `transfer` = 0 (no program is built).
- Commands naming the DM's own scratch registers (dscratch0/1, or SCR 25/26 in CHERIoT mode),
  which the debug spec leaves to the DM.
- Whether the hart implements a given CSR (that is the hart's exception, cmderr 3).
