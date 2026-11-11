#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Extract existing ROM checker words; never calculate or replace the expected hash.

The ROM image producer's Scrambler defines its physical VMEM format and public
configuration. This tool uses that format reader, not RTL or observed chip data.
N/S must be supplied explicitly because the IP documents only name the prefix.
The standard scramble_image.py producer uses --function-name '' --customization ROM_CTRL.
"""
import argparse
import hashlib
import io
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[5]
sys.path[:0] = [str(ROOT), str(ROOT / 'hw/ip/rom_ctrl/util')]
from mem import MemFile
from scramble_image import Scrambler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--vmem', type=Path, required=True)
    parser.add_argument('--top', type=Path, default=ROOT / 'hw/top_earlgrey/data/autogen/top_earlgrey.gen.hjson')
    parser.add_argument('--secrets', type=Path, default=ROOT / 'hw/top_earlgrey/data/autogen/top_earlgrey.secrets.testing.gen.hjson')
    parser.add_argument('--function-name', required=True)
    parser.add_argument('--customization', required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    scrambler = Scrambler.from_hjson_path(str(args.top), str(args.secrets), 'base-rom', io.StringIO())
    if scrambler.is_disabled():
        parser.error('unscrambled ROM configuration has no checker input')
    with args.vmem.open() as stream:
        memory = MemFile.load_vmem(39, stream)
    if len(memory.chunks) != 1 or memory.chunks[0].base_addr != 0 or len(memory.chunks[0].words) != scrambler.rom_size_words:
        parser.error('VMEM must contain every physical ROM word exactly once')
    physical = memory.chunks[0].words
    words = [physical[scrambler.addr_sp_enc(i)] for i in range(scrambler.rom_size_words)]
    with args.elf.open('rb') as stream:
        body = MemFile.load_elf32(stream, scrambler.rom_base).flatten(scrambler.rom_size_words - 8).chunks[0].words
    for i, expected in enumerate(body):
        if scrambler.unscramble_word(39, i, words[i]) & 0xffffffff != expected:
            parser.error(f'VMEM body does not match ELF at logical word {i:#x}')
    output = args.output or Path(str(args.elf) + '.romcheck')
    with output.open('w') as stream:
        print('EG2_ROM_CHECKER_V1', file=stream)
        print('elf_sha256', hashlib.sha256(args.elf.read_bytes()).hexdigest(), file=stream)
        print('function_name', args.function_name.encode().hex() or '-', file=stream)
        print('customization', args.customization.encode().hex() or '-', file=stream)
        print('words', len(words), file=stream)
        for word in words:
            print(f'{word:010x}', file=stream)
    print(output)


if __name__ == '__main__':
    main()
