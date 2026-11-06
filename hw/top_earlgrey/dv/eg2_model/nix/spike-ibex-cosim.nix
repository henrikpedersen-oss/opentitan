# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Spike for Ibex co-simulation (lowRISC/riscv-isa-sim, the ibex-cosim fork), the ISS the EG2 chip
# scoreboard steps with RVFI in RISC-V mode. A copy of the derivation in ibex_cheriot_verification's
# flake.nix (ibex_spike), byte for byte, with the same pkgsCompat (nixos-20.09, GCC 9.3 / glibc
# 2.32, for Xcelium's bundled glibc): both flakes then build the same store path. Change both
# together.
{ pkgsCompat }:
pkgsCompat.spike.overrideAttrs (prev: {
          pname = "spike-ibex-cosim";
          version = "0.5-dev";
          src = pkgsCompat.fetchFromGitHub {
            owner = "lowRISC";
            repo = "riscv-isa-sim";
            rev = "4b97396656485a129119deaec2ba35e5bf354841";
            sha256 = "sha256-oF2poKMYoYXytqo/t6eqngJgrr4WFHvKj/cKsGQ88DQ=";
          };
          configureFlags = (prev.configureFlags or []) ++ ["--enable-commitlog" "--enable-misaligned"];
          LDFLAGS = (prev.LDFLAGS or "") + " -static-libstdc++ -static-libgcc";

          # Make `_Zihpm` in the ISA string actually enable the extension.
          #
          # riscv/isa_parser.cc in this lowRISC fork does two things that defeat it:
          #
          #   extension_table[EXT_ZIHPM] = false; // IBEX does not implement this extension
          #   ...
          #   } else if (ext_str == "zihpm") {      <-- empty body, string silently ignored
          #
          # So hpmcounter3-hpmcounter31 do not exist for the ISS under any ISA
          # string, and a U-mode `csrr t0, hpmcounter3` raises
          # trap_illegal_instruction while Ibex -- which implements
          # MHPMCounterNum of them -- retires it. That is the mcounteren_test
          # cosim mismatch; see TODO.md.
          #
          # The comment is out of date rather than wrong in principle: Ibex does
          # implement these counters when MHPMCounterNum > 0. Fixed opt-in
          # (honour the ISA string) rather than by flipping the unconditional
          # default, so a build that does not ask for Zihpm is unchanged. The
          # testbench asks for it only when MHPMCounterNum > 0 -- see
          # get_isa_string() in core_ibex_base_test.sv.
          # NOTE: this appends to patchPhase, not postPatch. The nixos-20.09 spike
          # derivation sets its own patchPhase:
          #
          #     patchPhase = "patchShebangs scripts/*.sh\npatchShebangs tests/ebreak.py";
          #
          # A package-supplied patchPhase REPLACES the generic one, and this one
          # never calls `runHook postPatch`. So anything in postPatch is dropped
          # silently -- including the grep guard below, which is why an earlier
          # attempt at this fix produced a green build whose Spike still reported
          # extension_enabled(EXT_ZIHPM) == false.
          patchPhase = (prev.patchPhase or "") + ''

            substituteInPlace riscv/isa_parser.cc \
              --replace '} else if (ext_str == "zihpm") {' \
                        '} else if (ext_str == "zihpm") { extension_table[EXT_ZIHPM] = true;'
            # Fail the build rather than silently produce a Spike without the fix
            # if a source bump ever moves this line.
            grep -q 'ext_str == "zihpm") { extension_table\[EXT_ZIHPM\] = true;' \
              riscv/isa_parser.cc \
              || { echo "ZIHPM patch did not apply: riscv/isa_parser.cc changed upstream"; exit 1; }

            # cm.popret/cm.popretz return like `jalr x0, 0(ra)`, which clears bit 0.
            # The fork does set_pc(RA), so an odd popped ra becomes an odd Spike PC;
            # odd PCs are Spike's internal PC_SERIALIZE_* markers, and advance_pc()
            # calls abort() on an unknown one (riscv_debug_triggers_test seed 23322,
            # ra=0x6d0ef581). cm_popretz.h includes cm_popret.h, so this covers both.
            substituteInPlace riscv/insns/cm_popret.h \
              --replace 'set_pc(RA);' 'set_pc(RA & ~reg_t(1));'
            grep -q 'set_pc(RA & ~reg_t(1));' riscv/insns/cm_popret.h \
              || { echo "cm.popret patch did not apply: riscv/insns/cm_popret.h changed upstream"; exit 1; }

            # Zbs shift-immediates with shamt[5] = 1 are reserved on RV32 (Zbs: "shamt[5] = 1 is
            # reserved when XLEN = 32"). The fork masks the shift to 5 bits and executes them, where
            # Ibex raises an illegal instruction (cov_expr_rv_misc 4408, 2026-10-09: "DUT didn't write
            # to register x12" on bexti a2, a0, 33). rori already has the check; upstream
            # riscv-isa-sim added it to these four as well.
            for i in bexti bclri bseti binvi; do
              substituteInPlace riscv/insns/$i.h \
                --replace 'require_extension(EXT_ZBS);' 'require_extension(EXT_ZBS);
            require(SHAMT < xlen);'
              grep -q 'require(SHAMT < xlen);' riscv/insns/$i.h \
                || { echo "Zbs shamt patch did not apply: riscv/insns/$i.h changed upstream"; exit 1; }
            done
          '';
        })
