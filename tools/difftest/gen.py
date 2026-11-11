#!/usr/bin/env python3
"""Differential test generator: random straight-line 68000 snippets, recompiled with src/recomp.py.

    python3 gen.py <cases> <out dir>
Writes cases.inc (snippet bytes) and funcs.cpp (recompiled functions), built by run.sh.
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src'))
from m68k import decode  # noqa: E402
import recomp  # noqa: E402

BASE = 0x800000
SKIP = {'bcc', 'bra', 'bsr', 'dbcc', 'jmp', 'jsr', 'rts', 'rte', 'rtr', 'trap', 'trapv', 'stop', 'reset',
        'illegal', 'linea', 'linef', 'chk'}


def ok(i):
    if i.op in SKIP:
        return False
    for o in i.ops:
        if o[0] in ('sr', 'usp'):
            return False
    if i.op in ('link', 'unlk'):
        return True
    for o in i.ops:
        if o[0] in ('a', 'post', 'pre', 'ind', 'disp', 'idx') and len(o) > 1 and o[1] == 7 and i.op != 'pea':
            return False   # keep a7 a valid stack
    return True


def main():
    n, outd = int(sys.argv[1]), sys.argv[2]
    random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 7)
    os.makedirs(outd, exist_ok=True)
    inc, cpp = [], ['#include "m68k.h"', '']
    k = 0
    while k < n:
        code, fix = b'', []
        while len(code) < 2 * random.randint(1, 4) * 4:
            if random.random() < 0.15:      # forward branch to the end of the snippet (patched below)
                if random.random() < 0.5:
                    fix.append((len(code) + 2, len(code) + 2))
                    code += struct.pack('>HH', 0x6000 | random.randint(2, 15) << 8, 0)
                else:
                    fix.append((len(code) + 2, len(code) + 2))
                    code += struct.pack('>HH', 0x50c8 | random.randint(0, 15) << 8 | random.randint(0, 7), 0)
                continue
            for _ in range(200):
                w = bytes(random.getrandbits(8) for _ in range(10))
                i = decode(lambda a: struct.unpack_from('>H', w, a - BASE)[0], BASE)
                if ok(i):
                    code += w[:i.len]
                    break
        code = bytearray(code)
        for at, base in fix:
            struct.pack_into('>h', code, at, len(code) - base)
        code = bytes(code)
        data = code + b'\x4e\x71' * 8
        im = recomp.Image('main', 't', BASE, data, 't', [BASE])
        f = recomp.Func(im, BASE)
        a = BASE
        while a < BASE + len(code):
            i = im.insn(a)
            f.insns[a] = i
            a = i.end
        end = a
        e = recomp.Emit(f, {BASE: f'case_{k}'}, {BASE: f}, None)
        cpp += e.run(f'case_{k}')
        inc.append(f'{{ {", ".join(str(b) for b in data)} }}, {len(data)}, 0x{end:06x}, case_{k} }},')
        k += 1
    cpp.append('')
    open(os.path.join(outd, 'funcs.cpp'), 'w').write('\n'.join(cpp) + '\n')
    decl = [f'void case_{j}(Cpu& c, uint32_t pc);' for j in range(n)]
    open(os.path.join(outd, 'cases.inc'), 'w').write('\n'.join(decl) + '\nstatic const Case g_cases[] = {\n' +
                                                     '\n'.join('  {' + x for x in inc) + '\n};\n')


if __name__ == '__main__':
    main()
