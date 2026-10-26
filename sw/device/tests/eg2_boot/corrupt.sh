#!/bin/sh
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# corrupt.sh <in> <out> <offset>: copies a signed binary and inverts the byte at <offset>, for the
# bad-signature images of chip_sw_cheriot_secure_boot_bad_sig_* (eg2_boot/BUILD).
set -e
cp "$1" "$2"
chmod u+w "$2"
b=$(od -An -tu1 -j "$3" -N1 "$2" | tr -d ' \n')
[ -n "$b" ] || { echo "corrupt.sh: offset $3 is past the end of $1" >&2; exit 1; }
printf "$(printf '\\%03o' $((b ^ 255)))" | dd of="$2" bs=1 seek="$3" count=1 conv=notrunc 2>/dev/null
