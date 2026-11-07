# NanGate45 cell libraries

The NanGate 45 nm Open Cell Library, release PDKv1.3_v2010_12.Apache.CCL (Si2), under the Apache
License 2.0 (LICENSE here). A generic, non-manufacturable library for research and EDA flows:
results with it compare designs and configurations with each other, they do not predict a
production node.

| File | Corner | From |
|---|---|---|
| `NangateOpenCellLibrary_typical.lib` | typical: 1.10 V, 25 C | OpenROAD-flow-scripts v3.0, `flow/platforms/nangate45/lib/` |
| `NangateOpenCellLibrary_slow.lib` | slow: 0.95 V, 125 C | OpenROAD 26Q2, `test/Nangate45/Nangate45_slow.lib` |

Both come from the same characterisation run (NGLibraryCharacterizer, 10 Feb 2011) and hold the
same cells with the same areas, apart from the physical-only TAPCELL_X1 in the typical one. Files
copied unchanged.

Used by `syn_setup.sh` / `syn_setup_cheriot.sh` (`LR_SYNTH_CORNER=typical|slow`, the Yosys flow)
and `genus/run_genus.sh` (`make genus-synth SYN_CORNER=...`).
