# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# MPACT-CHERIoT (google/mpact-cheriot), the CHERIoT-mode instruction-set model of the EG2 SystemC
# model (eg2sim; decision 2026-10-23: Spike in RISC-V mode, MPACT-CHERIoT in CHERIoT mode). Output:
# lib/libeg2_mpact.so and include/eg2_mpact.h, MPACT behind the C interface of mpact-cheriot/
# (eg2_mpact.h), which eg2sim links (EG2_MPACT, devShell `model`).
#
# MPACT is a Bazel module (bzlmod) that pulls abseil, protobuf, mpact-sim and mpact-riscv from the
# Bazel Central Registry. Two derivations:
#   deps   fixed-output: `bazel fetch` of //eg2:libeg2_mpact.so into a repository cache (downloads
#          and registry files, content-addressed by their sha256), against the pinned
#          MODULE.bazel.lock in mpact-cheriot/. Its hash changes only with the MPACT revision, the
#          lockfile or the wrapper's dependencies.
#   build  offline from that cache with --lockfile_mode=error.
# The compiler is pkgsCompat's Clang 11 with GCC 9.3's libstdc++ and glibc 2.31, eg2sim's own
# toolchain: GCC 9.3 cannot build MPACT (mpact-riscv compiles with -ffp-model=strict, a Clang
# option). Bazel 8 (MPACT's .bazelversion) comes from pkgsBazel, the flake has none older.
{ pkgs, pkgsCompat, pkgsBazel }:
let
  rev = "bfd53d8763fac116228af37eee12f83eff49b97a";  # main, 2026-06-15
  src = pkgs.fetchFromGitHub {
    owner = "google";
    repo = "mpact-cheriot";
    inherit rev;
    hash = "sha256-ylCQ7NL3Q2Qv1Dl8uDvEb/xqDh7/FokkE1E7l8NZ/IE=";
  };
  bazel = pkgsBazel.bazel_8;

  # The MPACT workspace with the wrapper as package //eg2 and the pinned lockfile.
  workspace = pkgs.runCommand "mpact-cheriot-eg2-workspace" { } ''
    cp -r ${src} $out
    chmod -R u+w $out
    # CAndPerm on a sealed capability keeps the tag when rs2 clears nothing or only Permit_Global
    # (CHERIoT Sail, cheri_insts.sail, CAndPerm; cheriot-sail#83). MPACT clears it for every sealed
    # capability, which the RTOS's misc test catches (sonata-prerelease runs the check).
    # The line is CheriotCAndPerm's alone in the file.
    substituteInPlace $out/cheriot/riscv_cheriot_instructions.cc \
      --replace-fail 'bool valid = !cap_src->IsSealed();' \
        'bool valid = !cap_src->IsSealed() || ((perms_to_keep | CheriotRegister::kPermitGlobal) & CheriotRegister::kPermitMask) == CheriotRegister::kPermitMask;'
    # FENCE.I: Ibex runs code written before it (Zifencei; MPACT-CHERIoT does not decode FENCE.I,
    # eg2sim emulates it) and MPACT keeps decoded instructions by address, so the wrapper must drop
    # them: an accessor for the decode cache (chip_eg2_rv_dm_code_patch_cheriot, 2026-11-07).
    substituteInPlace $out/cheriot/cheriot_top.h \
      --replace-fail '  CheriotState* state() const { return state_; }' \
        '  CheriotState* state() const { return state_; }
  generic::DecodeCache* decode_cache() const { return cheriot_decode_cache_; }'
    cp -r ${./mpact-cheriot} $out/eg2
    chmod -R u+w $out/eg2
    mv $out/eg2/MODULE.bazel.lock $out/MODULE.bazel.lock
  '';

  # Bazel's auto-configured C++ toolchain on Clang 11. The nix wrapper adds libstdc++'s include
  # directories only when called as clang++, and Bazel compiles C++ with the C compiler's name: C++
  # (and the link) goes to clang++, C and assembly to clang. ar, nm and the rest from binutils.
  cc = pkgs.runCommand "mpact-cheriot-cc" { } ''
    mkdir -p $out/bin
    cat > $out/bin/clang <<EOF
    #!${pkgs.runtimeShell}
    for a in "\$@"; do
      case "\$a" in
        *.c|*.S|*.s|-xc|-xassembler*) exec ${pkgsCompat.clang_11}/bin/clang "\$@";;
      esac
    done
    exec ${pkgsCompat.clang_11}/bin/clang++ "\$@"
    EOF
    chmod +x $out/bin/clang
    for t in ld ld.gold ld.bfd as cpp; do ln -s ${pkgsCompat.clang_11}/bin/$t $out/bin/$t; done
    for t in ar nm objcopy objdump strip c++filt dwp; do
      if [ -e ${pkgsCompat.binutils-unwrapped}/bin/$t ]; then
        ln -s ${pkgsCompat.binutils-unwrapped}/bin/$t $out/bin/$t
      fi
    done
  '';

  tools = [ cc bazel pkgs.jdk21_headless pkgs.python3 pkgs.coreutils pkgs.bash pkgs.gnugrep
            pkgs.gnused pkgs.gawk pkgs.findutils pkgs.which pkgs.diffutils pkgs.gnutar pkgs.gzip ];
  # Bazel's options for both derivations. The repo rules (cc_configure) and the actions see PATH.
  bazelSetup = ''
    export HOME=$TMPDIR
    export PATH=${pkgs.lib.makeBinPath tools}:$PATH
    bazelFlags=(
      --repo_contents_cache=
      --lockfile_mode=error
      --repo_env=CC=${cc}/bin/clang
      --repo_env=PATH=$PATH
      --action_env=PATH=$PATH
      --host_action_env=PATH=$PATH
      --noshow_progress
    )
    # The external repositories' scripts start with #!/usr/bin/env, which the sandbox has not
    # (rules_cc's generate_system_module_map.sh, run by cc_configure): a first fetch extracts them,
    # its failure is expected; patchShebangs; then the real command.
    patchedExternals() {
      bazel --batch --output_user_root=$TMPDIR/bazel fetch "$@" >/dev/null 2>&1 || true
      patchShebangs --build $TMPDIR/bazel/*/external >/dev/null
    }
  '';
  target = "//eg2:libeg2_mpact.so";

  deps = pkgs.stdenvNoCC.mkDerivation {
    name = "mpact-cheriot-bazel-deps";
    src = workspace;
    nativeBuildInputs = [ pkgs.cacert ];
    dontConfigure = true;
    dontFixup = true;
    buildPhase = ''
      ${bazelSetup}
      patchedExternals --repository_cache=$out "''${bazelFlags[@]}" ${target}
      bazel --batch --output_user_root=$TMPDIR/bazel fetch --repository_cache=$out \
        "''${bazelFlags[@]}" ${target}
    '';
    dontInstall = true;
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
    outputHash = "sha256-ck6V0Cz6DZHFEo3gloSuPRojnUVgz9/11vUVwauea38=";
  };
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "mpact-cheriot-eg2";
  version = "0-unstable-2026-06-15";
  src = workspace;
  passthru = { inherit deps; };
  nativeBuildInputs = [ pkgs.patchelf ];  # for the fixup's check of /build references
  dontConfigure = true;
  buildPhase = ''
    ${bazelSetup}
    cp -r ${deps} $TMPDIR/repository_cache
    chmod -R u+w $TMPDIR/repository_cache
    patchedExternals --repository_cache=$TMPDIR/repository_cache "''${bazelFlags[@]}" ${target}
    # As many compile jobs as nix gives the build (--cores), not one per host CPU.
    bazel --batch --output_user_root=$TMPDIR/bazel build -c opt --jobs=''${NIX_BUILD_CORES:-1} \
      --repository_cache=$TMPDIR/repository_cache "''${bazelFlags[@]}" ${target}
  '';
  installPhase = ''
    install -D -m 755 bazel-bin/eg2/libeg2_mpact.so $out/lib/libeg2_mpact.so
    install -D -m 644 eg2/eg2_mpact.h $out/include/eg2_mpact.h
  '';
  # The library's RUNPATH is glibc 2.31's, from the Clang 11 wrapper; nothing else to patch.
  dontPatchELF = true;
  dontStrip = true;
}
