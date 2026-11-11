#!/usr/bin/env python3
"""Guess the load address of a raw 68000 code blob (dev tool, needs capstone).

Scores every base by how many absolute jsr/jmp targets land on an instruction that follows
an rts/rte/bra/jmp (a likely function start). SUBCODE.BIN: $D400 scores 183 of 193.

    python3 findbase.py <file>
"""
import collections
import re
import sys

import capstone


def main():
    sc = open(sys.argv[1], 'rb').read()
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
    md.skipdata = True
    ins = list(md.disasm(sc, 0))
    after, prev = set(), None
    for i in ins:
        if prev in ('rts', 'bra.w', 'jmp', 'rte'):
            after.add(i.address)
        prev = i.mnemonic
    tg = []
    for i in ins:
        m = re.fullmatch(r'\$([0-9a-f]+)\.l', i.op_str) if i.mnemonic in ('jsr', 'jmp') else None
        if m:
            tg.append(int(m.group(1), 16))
    c = collections.Counter()
    for t in tg:
        for a in after:
            if 0 <= t - a < 0x1000000:
                c[t - a] += 1
    print(sys.argv[1], 'absolute jsr/jmp:', len(tg), [(hex(b), n) for b, n in c.most_common(5)])


if __name__ == '__main__':
    main()
