# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# The model's tools, so it builds without another repository's environment:
#   nix develop        SystemC, Spike and MPACT-CHERIoT built with one toolchain (EG2_SYSTEMC,
#                      EG2_SPIKE, EG2_MPACT), and GCC to build eg2sim, the chip bench's library and
#                      the NIST checker (make; README.md)
# The pins are eg2_top_verification's, so the store paths are the same as there.
{
  description = "ot-model: the SystemC model of OpenTitan chips (eg2sim), Earl Grey 2 first";

  inputs = {
    # nixos-25.05 (ac62194c): SystemC's sources, MPACT's packages.
    nixpkgs.url = "github:NixOS/nixpkgs/ac62194c3917d5f474c1a844b6fd6da2db95077d";
    # nixos-20.09 (GCC 9.3 / glibc 2.32): Spike and SystemC, so their libraries load in Xcelium.
    nixpkgs-gcc93 = {
      url = "github:NixOS/nixpkgs/1c1f5649bb9c1b0d98637c8c365228f57126f361";
      flake = false;
    };
    # nixos-unstable (26.05) for Bazel 8 alone, which MPACT-CHERIoT needs (nix/mpact-cheriot.nix).
    nixpkgs-bazel8 = {
      url = "github:NixOS/nixpkgs/da6dc0e7b26060f2f71ce3ef7088fb56ee44fa36";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, nixpkgs-gcc93, nixpkgs-bazel8 }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      pkgsCompat = import nixpkgs-gcc93 { inherit system; };
      spike-ibex-cosim = import ./nix/spike-ibex-cosim.nix { inherit pkgsCompat; };
      systemc-compat = import ./nix/systemc-compat.nix {
        inherit pkgsCompat;
        inherit (pkgs.systemc) src version;
      };
      mpact-cheriot = import ./nix/mpact-cheriot.nix {
        inherit pkgs pkgsCompat;
        pkgsBazel = import nixpkgs-bazel8 { inherit system; };
      };
      model = pkgsCompat.mkShell {
        packages = [ pkgsCompat.gcc pkgsCompat.gnumake ];
        EG2_SPIKE = "${spike-ibex-cosim}";
        EG2_SYSTEMC = "${systemc-compat}";
        EG2_MPACT = "${mpact-cheriot}";
      };
    in {
      packages.${system} = { inherit spike-ibex-cosim systemc-compat mpact-cheriot; };
      devShells.${system} = { default = model; inherit model; };
    };
}
