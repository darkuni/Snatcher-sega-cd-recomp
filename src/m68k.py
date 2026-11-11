"""Motorola 68000 instruction decoder for the static recompiler.

decode(mem, addr) -> Insn, where mem(addr) returns the big-endian 16-bit word at addr.

Operands (tuples):
    ('d', n) ('a', n)                    data / address register
    ('ind', n) ('post', n) ('pre', n)    (An) (An)+ -(An)
    ('disp', n, d16)                     d16(An)
    ('idx', n, d8, xr, xl)               d8(An,Xn) - xr = ('d'|'a', n), xl = long index
    ('absw', a) ('absl', a)              absolute, full 32 bits (absw sign-extended); the bus
                                         ignores bits 24-31, address registers keep them
    ('pcd', target)                      d16(PC), target resolved
    ('pcx', base, d8, xr, xl)            d8(PC,Xn), base = address of the extension word
    ('imm', v)
    ('ccr',) ('sr',) ('usp',)
    ('regs', mask)                       MOVEM list, bit i = d0..d7 a0..a7 (already un-reversed)
"""

SIZES = {0: 1, 1: 2, 2: 4}
CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']


class Insn:
    __slots__ = ('addr', 'op', 'size', 'ops', 'len', 'cc', 'target', 'words')

    def __init__(self, addr, op, size=0, ops=(), cc=None, target=None):
        self.addr = addr
        self.op = op            # mnemonic without size
        self.size = size        # bytes: 1/2/4, 0 = none
        self.ops = list(ops)
        self.len = 2
        self.cc = cc            # condition index for bcc/dbcc/scc
        self.target = target    # branch target

    @property
    def end(self):
        return self.addr + self.len

    def __repr__(self):
        s = {0: '', 1: '.b', 2: '.w', 4: '.l'}[self.size]
        return f'{self.addr:06x}: {self.op}{s} {", ".join(fmt(o) for o in self.ops)}'


def fmt(o):
    k = o[0]
    if k in ('d', 'a'):
        return f'{k}{o[1]}'
    if k == 'ind':
        return f'(a{o[1]})'
    if k == 'post':
        return f'(a{o[1]})+'
    if k == 'pre':
        return f'-(a{o[1]})'
    if k == 'disp':
        return f'{o[2]}(a{o[1]})'
    if k == 'idx':
        return f'{o[2]}(a{o[1]},{o[3][0]}{o[3][1]}.{"l" if o[4] else "w"})'
    if k in ('absw', 'absl'):
        return f'${o[1]:x}'
    if k == 'pcd':
        return f'${o[1]:x}(pc)'
    if k == 'pcx':
        return f'{o[2]}(pc@{o[1]:x},{o[3][0]}{o[3][1]}.{"l" if o[4] else "w"})'
    if k == 'imm':
        return f'#${o[1]:x}'
    if k == 'regs':
        return f'<{o[1]:04x}>'
    return k


class Bad(Exception):
    pass


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class _D:
    def __init__(self, mem, addr):
        self.mem = mem
        self.addr = addr
        self.pc = addr + 2

    def w(self):
        v = self.mem(self.pc)
        self.pc += 2
        return v

    def l(self):
        hi = self.w()
        return (hi << 16) | self.w()

    def ea(self, mode, reg, size, allow=None):
        """Decode an effective address. allow: set of category names that are legal."""
        if mode == 0:
            o = ('d', reg)
        elif mode == 1:
            if size == 1:
                raise Bad('byte An')
            o = ('a', reg)
        elif mode == 2:
            o = ('ind', reg)
        elif mode == 3:
            o = ('post', reg)
        elif mode == 4:
            o = ('pre', reg)
        elif mode == 5:
            o = ('disp', reg, s16(self.w()))
        elif mode == 6:
            o = ('idx', reg) + self._ext()
        elif reg == 0:
            o = ('absw', s16(self.w()) & 0xffffffff)
        elif reg == 1:
            o = ('absl', self.l())
        elif reg == 2:
            base = self.pc
            o = ('pcd', (base + s16(self.w())) & 0xffffff)
        elif reg == 3:
            base = self.pc
            o = ('pcx', base) + self._ext()
        elif reg == 4:
            if size == 1:
                o = ('imm', self.w() & 0xff)
            elif size == 2:
                o = ('imm', self.w())
            elif size == 4:
                o = ('imm', self.l())
            else:
                raise Bad('imm size')
        else:
            raise Bad('ea mode 7/%d' % reg)
        if allow is not None and cat(o) not in allow:
            raise Bad('ea not allowed')
        return o

    def _ext(self):
        e = self.w()            # the 68000 ignores bits 8-10 (68020 scale / full format)
        xr = ('a' if e & 0x8000 else 'd', (e >> 12) & 7)
        return (s8(e & 0xff), xr, bool(e & 0x800))


def cat(o):
    """Category of an operand for legality checks."""
    return o[0]


ALL = {'d', 'a', 'ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl', 'pcd', 'pcx', 'imm'}
DATA = ALL - {'a'}
MEM = ALL - {'d', 'a'}
ALT = ALL - {'pcd', 'pcx', 'imm'}
DALT = DATA & ALT
MALT = MEM & ALT
CTRL = {'ind', 'disp', 'idx', 'absw', 'absl', 'pcd', 'pcx'}
CALT = CTRL - {'pcd', 'pcx'}


def decode(mem, addr):
    d = _D(mem, addr)
    try:
        i = _decode(d, mem(addr))
    except Bad:
        i = Insn(addr, 'illegal')
        d.pc = addr + 2
    i.len = d.pc - addr
    return i


def _decode(d, w):
    a = d.addr
    hi = w >> 12
    mode, reg = (w >> 3) & 7, w & 7
    rx = (w >> 9) & 7
    sz = (w >> 6) & 3
    if hi == 0:
        return _line0(d, w, mode, reg, rx, sz)
    if hi in (1, 2, 3):
        size = {1: 1, 3: 2, 2: 4}[hi]
        src = d.ea(mode, reg, size, ALL)
        dm = (w >> 6) & 7
        if dm == 1:
            if size == 1:
                raise Bad('movea.b')
            return Insn(a, 'movea', size, [src, ('a', rx)])
        dst = d.ea(dm, rx, size, DALT)
        return Insn(a, 'move', size, [src, dst])
    if hi == 4:
        return _line4(d, w, mode, reg, rx, sz)
    if hi == 5:
        if sz == 3:
            cc = (w >> 8) & 15
            if mode == 1:
                t = (d.pc + s16(d.w())) & 0xffffff
                return Insn(a, 'dbcc', 2, [('d', reg)], cc=cc, target=t)
            return Insn(a, 'scc', 1, [d.ea(mode, reg, 1, DALT)], cc=cc)
        q = rx or 8
        size = SIZES[sz]
        dst = d.ea(mode, reg, size, ALT)
        if dst[0] == 'a' and size == 1:
            raise Bad('addq.b An')
        return Insn(a, 'subq' if w & 0x100 else 'addq', size, [('imm', q), dst])
    if hi == 6:
        cc = (w >> 8) & 15
        disp = w & 0xff
        base = d.pc
        if disp == 0:
            disp = s16(d.w())
        elif disp == 0xff:
            raise Bad('bcc.l (68020)')
        else:
            disp = s8(disp)
        t = (base + disp) & 0xffffff
        op = 'bra' if cc == 0 else 'bsr' if cc == 1 else 'bcc'
        return Insn(a, op, 0, [], cc=cc if cc > 1 else None, target=t)
    if hi == 7:
        if w & 0x100:
            raise Bad('moveq bit8')
        return Insn(a, 'moveq', 4, [('imm', s8(w & 0xff) & 0xffffffff), ('d', rx)])
    if hi in (8, 0xC):
        return _line8c(d, w, hi, mode, reg, rx, sz)
    if hi in (9, 0xD):
        op = 'sub' if hi == 9 else 'add'
        if sz == 3:
            size = 4 if w & 0x100 else 2
            return Insn(a, op + 'a', size, [d.ea(mode, reg, size, ALL), ('a', rx)])
        size = SIZES[sz]
        if w & 0x100:
            if mode in (0, 1):
                if mode == 0:
                    return Insn(a, op + 'x', size, [('d', reg), ('d', rx)])
                return Insn(a, op + 'x', size, [('pre', reg), ('pre', rx)])
            return Insn(a, op, size, [('d', rx), d.ea(mode, reg, size, MALT)])
        return Insn(a, op, size, [d.ea(mode, reg, size, ALL), ('d', rx)])
    if hi == 0xB:
        if sz == 3:
            size = 4 if w & 0x100 else 2
            return Insn(a, 'cmpa', size, [d.ea(mode, reg, size, ALL), ('a', rx)])
        size = SIZES[sz]
        if w & 0x100:
            if mode == 1:
                return Insn(a, 'cmpm', size, [('post', reg), ('post', rx)])
            return Insn(a, 'eor', size, [('d', rx), d.ea(mode, reg, size, DALT)])
        return Insn(a, 'cmp', size, [d.ea(mode, reg, size, ALL), ('d', rx)])
    if hi == 0xE:
        return _shift(d, w, mode, reg, rx, sz)
    if hi == 0xA:
        return Insn(a, 'linea', 0, [('imm', w)])
    return Insn(a, 'linef', 0, [('imm', w)])


def _line0(d, w, mode, reg, rx, sz):
    a = d.addr
    if w & 0x100:
        if mode == 1:
            size = 4 if w & 0x40 else 2
            disp = s16(d.w())
            if w & 0x80:
                return Insn(a, 'movep', size, [('d', rx), ('disp', reg, disp)])
            return Insn(a, 'movep', size, [('disp', reg, disp), ('d', rx)])
        op = ['btst', 'bchg', 'bclr', 'bset'][sz]
        size = 4 if mode == 0 else 1
        dst = d.ea(mode, reg, 1, DATA if op == 'btst' else DALT)
        return Insn(a, op, size, [('d', rx), dst])
    if rx == 4:
        op = ['btst', 'bchg', 'bclr', 'bset'][sz]
        bit = d.w() & 0xff
        size = 4 if mode == 0 else 1
        allow = (DATA - {'imm'}) if op == 'btst' else DALT
        dst = d.ea(mode, reg, 1, allow)
        return Insn(a, op, size, [('imm', bit), dst])
    ops = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
    if rx not in ops or sz == 3:
        raise Bad('line0')
    op = ops[rx]
    size = SIZES[sz]
    if mode == 7 and reg == 4 and op in ('ori', 'andi', 'eori'):
        if size == 1:
            return Insn(a, op, 1, [('imm', d.w() & 0xff), ('ccr',)])
        if size == 2:
            return Insn(a, op, 2, [('imm', d.w()), ('sr',)])
        raise Bad('long to sr')
    imm = d.ea(7, 4, size)
    dst = d.ea(mode, reg, size, DALT)
    return Insn(a, op, size, [imm, dst])


def _line4(d, w, mode, reg, rx, sz):
    a = d.addr
    if w & 0x100:
        if sz == 2:
            return Insn(a, 'chk', 2, [d.ea(mode, reg, 2, DATA), ('d', rx)])
        if sz == 3:
            return Insn(a, 'lea', 4, [d.ea(mode, reg, 4, CTRL), ('a', rx)])
        raise Bad('line4 bit8')
    sub = (w >> 8) & 15
    if sub in (0, 2, 4, 6) and sz != 3:
        op = {0: 'negx', 2: 'clr', 4: 'neg', 6: 'not'}[sub]
        size = SIZES[sz]
        return Insn(a, op, size, [d.ea(mode, reg, size, DALT)])
    if w & 0xffc0 == 0x40c0:
        return Insn(a, 'move', 2, [('sr',), d.ea(mode, reg, 2, DALT)])
    if w & 0xffc0 == 0x44c0:
        return Insn(a, 'move', 2, [d.ea(mode, reg, 2, DATA), ('ccr',)])
    if w & 0xffc0 == 0x46c0:
        return Insn(a, 'move', 2, [d.ea(mode, reg, 2, DATA), ('sr',)])
    if w & 0xffc0 == 0x4800:
        return Insn(a, 'nbcd', 1, [d.ea(mode, reg, 1, DALT)])
    if w & 0xfff8 == 0x4840:
        return Insn(a, 'swap', 4, [('d', reg)])
    if w & 0xffc0 == 0x4840:
        return Insn(a, 'pea', 4, [d.ea(mode, reg, 4, CTRL)])
    if w & 0xfff8 == 0x4880:
        return Insn(a, 'ext', 2, [('d', reg)])
    if w & 0xfff8 == 0x48c0:
        return Insn(a, 'ext', 4, [('d', reg)])
    if w & 0xfb80 == 0x4880:
        size = 4 if w & 0x40 else 2
        mask = d.w()
        if w & 0x400:
            ea = d.ea(mode, reg, size, CTRL | {'post'})
            return Insn(a, 'movem', size, [ea, ('regs', mask)])
        ea = d.ea(mode, reg, size, CALT | {'pre'})
        if ea[0] == 'pre':
            mask = int(f'{mask:016b}'[::-1], 2)   # -(An) list is reversed: bit 0 = a7
        return Insn(a, 'movem', size, [('regs', mask), ea])
    if w == 0x4afc:
        return Insn(a, 'illegal')
    if w & 0xffc0 == 0x4ac0:
        return Insn(a, 'tas', 1, [d.ea(mode, reg, 1, DALT)])
    if w & 0xff00 == 0x4a00 and sz != 3:
        size = SIZES[sz]
        return Insn(a, 'tst', size, [d.ea(mode, reg, size, DALT)])
    if w & 0xfff0 == 0x4e40:
        return Insn(a, 'trap', 0, [('imm', w & 15)])
    if w & 0xfff8 == 0x4e50:
        return Insn(a, 'link', 2, [('a', reg), ('imm', s16(d.w()))])
    if w & 0xfff8 == 0x4e58:
        return Insn(a, 'unlk', 0, [('a', reg)])
    if w & 0xfff8 == 0x4e60:
        return Insn(a, 'move', 4, [('a', reg), ('usp',)])
    if w & 0xfff8 == 0x4e68:
        return Insn(a, 'move', 4, [('usp',), ('a', reg)])
    simple = {0x4e70: 'reset', 0x4e71: 'nop', 0x4e73: 'rte', 0x4e75: 'rts', 0x4e76: 'trapv', 0x4e77: 'rtr'}
    if w in simple:
        return Insn(a, simple[w])
    if w == 0x4e72:
        return Insn(a, 'stop', 0, [('imm', d.w())])
    if w & 0xffc0 == 0x4e80:
        return Insn(a, 'jsr', 0, [d.ea(mode, reg, 4, CTRL)])
    if w & 0xffc0 == 0x4ec0:
        return Insn(a, 'jmp', 0, [d.ea(mode, reg, 4, CTRL)])
    raise Bad('line4')


def _line8c(d, w, hi, mode, reg, rx, sz):
    a = d.addr
    if sz == 3:
        op = ('divu', 'divs') if hi == 8 else ('mulu', 'muls')
        return Insn(a, op[1 if w & 0x100 else 0], 2, [d.ea(mode, reg, 2, DATA), ('d', rx)])
    if w & 0x1f0 == 0x100:
        op = 'sbcd' if hi == 8 else 'abcd'
        if mode == 0:
            return Insn(a, op, 1, [('d', reg), ('d', rx)])
        return Insn(a, op, 1, [('pre', reg), ('pre', rx)])
    if hi == 0xC and w & 0x130 == 0x100:
        opm = (w >> 3) & 0x1f
        if opm == 0x08:
            return Insn(a, 'exg', 4, [('d', rx), ('d', reg)])
        if opm == 0x09:
            return Insn(a, 'exg', 4, [('a', rx), ('a', reg)])
        if opm == 0x11:
            return Insn(a, 'exg', 4, [('d', rx), ('a', reg)])
        raise Bad('exg')
    op = 'or' if hi == 8 else 'and'
    size = SIZES[sz]
    if w & 0x100:
        return Insn(a, op, size, [('d', rx), d.ea(mode, reg, size, MALT)])
    return Insn(a, op, size, [d.ea(mode, reg, size, DATA), ('d', rx)])


def _shift(d, w, mode, reg, rx, sz):
    a = d.addr
    left = bool(w & 0x100)
    if sz == 3:
        kind = (w >> 9) & 3
        if w & 0x800:
            raise Bad('bitfield (68020)')
        op = ['as', 'ls', 'rox', 'ro'][kind] + ('l' if left else 'r')
        return Insn(a, op, 2, [('imm', 1), d.ea(mode, reg, 2, MALT)])
    kind = (w >> 3) & 3
    op = ['as', 'ls', 'rox', 'ro'][kind] + ('l' if left else 'r')
    size = SIZES[sz]
    if w & 0x20:
        cnt = ('d', rx)
    else:
        cnt = ('imm', rx or 8)
    return Insn(a, op, size, [cnt, ('d', reg)])
