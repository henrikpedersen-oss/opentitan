# C-to-RTL equivalence: the revocation decision

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

The method is the AES one (`eg2-git-proposal`, `make c2rtl`). The tools (Bambu, Yosys with
yosys-slang) come from that repository's `c2rtl` shell: from eg2_top_verification,
`make c2rtl-cms` (`C2RTL_FLAKE` points at it). Output in `scratch/c2rtl/cms-trvk/`.

Requirement: REQ_CMS_08 (the revocation decision of the subsystem's filter). Not covered: the
core's own filter in Ibex (a separate copy, ticket #1011), and the response side (the tag cleared
on a set bit, an error or an integrity fault), which the UVM bench checks.
