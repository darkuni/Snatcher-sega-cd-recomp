#!/usr/bin/env python3
"""Linear 68000 disassembly (dev tool, needs `pip install capstone`).

    python3 dis68k.py <file> <base hex> <out.s> [start offset hex]
"""
import sys

import capstone


def main():
    f, base, out = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
    start = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
    b = open(f, 'rb').read()
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
    md.skipdata = True
    with open(out, 'w') as o:
        for i in md.disasm(b[start:], base + start):
            o.write(f'{i.address:06x}: {i.mnemonic} {i.op_str}\n')


if __name__ == '__main__':
    main()
