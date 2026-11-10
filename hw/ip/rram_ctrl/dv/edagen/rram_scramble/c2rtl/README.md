# C-to-RTL check of rram_ctrl's scrambling mask

The proposal's AES c2rtl flow (`crypto-engines/aes/dv/c2rtl` in eg2-git-proposal) applied to
`rram_scramble`'s address mask, `GF_MULT(word_addr, addr_key)` in GF(2^128).

- `rram_gf_mask_hls.c`: the reference model's mask (`eg2_rram_scramble_core.h`) in plain C for
  Bambu, which has no 128-bit type: four 32-bit key words in, one 32-bit word of the mask out.
- `rram_gf_mask_test.cc`: `make differential` checks the C function against the model on 100,000
  random keys and addresses and the corner cases; `make differential-negative` flips one expected bit
  and must fail.
- `rram_gf_mask_equiv.v`, `equiv.ys`: `make equiv` is a Yosys SAT miter between Bambu's core and
  `rram_scramble` (through `rram_scramble_unit`) for every key, address and output word.
  `make equiv-negative` repeats it with one bit of the reduction polynomial changed, and must fail.

Run from the c2rtl shell of eg2-git-proposal:

    nix develop <eg2-git-proposal>#c2rtl --command make -C <this directory> all

The differential tests pass. Bambu builds the core. The SAT miter does not fit the laptop: Yosys ran
out of memory at time step 41 of 160 with `ulimit -v 8000000` (`std::bad_alloc`, 2026-11-07).
Bambu's core loops over 17 address bits and four words, so the proof needs well over 40 cycles.

The mask is proven in Jasper instead. The scrambling unit's FPV (`../formal/rram_scramble_fpv.sv`,
`MaskProduct_A`) proves that every acknowledged mask equals a reference GF(2^128) multiplier for
every key and address. That reference is the same function as the C code above.

The C-to-RTL step through Bambu stays open. Options: a Jasper miter, which needs a reset input
added to `rram_gf_mask_equiv.v`; or a run on nyx with more memory and a tighter bound taken from
Bambu's latency.
