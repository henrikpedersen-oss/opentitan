# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# SystemC (Accellera, with TLM-2.0) for the EG2 SystemC model, built with the same toolchain as
# Spike (pkgsCompat: nixos-20.09, GCC 9.3 / glibc 2.32), so eg2sim and the bench's DPI library link
# Spike and SystemC without mixing glibc versions. The source is nixpkgs' own systemc's.
{ pkgsCompat, src, version }:
pkgsCompat.stdenv.mkDerivation {
  pname = "systemc";
  inherit src version;
  nativeBuildInputs = [ pkgsCompat.cmake ];
  cmakeFlags = [
    "-DCMAKE_CXX_STANDARD=17"
    "-DBUILD_SHARED_LIBS=ON"
    "-DENABLE_PHASE_CALLBACKS_TRACING=OFF"
  ];
}
