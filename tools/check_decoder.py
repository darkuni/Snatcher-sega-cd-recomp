#!/usr/bin/env python3
"""Cross-check src/m68k.py against capstone (dev tool).

    python3 check_decoder.py <file> <base hex> [...]      # real code images
    python3 check_decoder.py --fuzz N                      # N random instruction words
"""
import os
import random
import struct
import sys

import capstone

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src'))
from m68k import decode  # noqa: E402

MD = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
MD.skipdata = True

ALIAS = {'bhi': 'bcc', 'bls': 'bcc', 'bcc': 'bcc', 'bcs': 'bcc', 'bne': 'bcc', 'beq': 'bcc', 'bvc': 'bcc',
         'bvs': 'bcc', 'bpl': 'bcc', 'bmi': 'bcc', 'bge': 'bcc', 'blt': 'bcc', 'bgt': 'bcc', 'ble': 'bcc'}


def root(m):
    m = m.split('.')[0]
    if m in ALIAS:
        return 'bcc'
    if m.startswith('db'):
        return 'dbcc'
    if m[0] == 's' and m[1:] in ('t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le'):
        return 'scc'
    return m


def check(buf, base, show=20):
    mem = lambda a: struct.unpack_from('>H', buf, a - base)[0] if 0 <= a - base < len(buf) - 1 else 0
    bad = n = 0
    for i in MD.disasm(buf, base):
        if i.mnemonic in ('.byte', 'dc.w'):
            continue
        n += 1
        j = decode(mem, i.address)
        r1, r2 = root(i.mnemonic), j.op
        same = (r1 == r2 or (r1, r2) in {('andi', 'andi'), ('move', 'movea')}) and j.len == i.size
        if r1 == 'movea' and r2 == 'movea':
            same = j.len == i.size
        if not same:
            bad += 1
            if bad <= show:
                print(f'{i.address:06x} cs: {i.mnemonic} {i.op_str} ({i.size})   me: {j!r} ({j.len})')
    return n, bad


def main():
    if sys.argv[1] == '--fuzz':
        random.seed(1)
        tot = bad = 0
        for _ in range(int(sys.argv[2])):
            buf = bytes(random.getrandbits(8) for _ in range(16))
            ins = [i for i in MD.disasm(buf, 0x1000, count=1) if i.mnemonic not in ('.byte', 'dc.w')]
            if not ins:
                continue
            i = ins[0]
            j = decode(lambda a: struct.unpack_from('>H', buf, a - 0x1000)[0], 0x1000)
            tot += 1
            if not (root(i.mnemonic) == j.op and j.len == i.size):
                bad += 1
                if '[' not in i.op_str and '*' not in i.op_str and 'invalid' not in i.op_str:
                    print(f'cs: {i.mnemonic} {i.op_str} ({i.size})   me: {j!r} ({j.len})  {buf[:i.size].hex()}')
        print('fuzz', tot, 'mismatches', bad)
        return
    args = sys.argv[1:]
    for f, b in zip(args[::2], args[1::2]):
        n, bad = check(open(f, 'rb').read(), int(b, 16))
        print(f, n, 'insns,', bad, 'mismatches')


if __name__ == '__main__':
    main()
