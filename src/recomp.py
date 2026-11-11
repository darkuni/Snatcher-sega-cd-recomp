#!/usr/bin/env python3
"""Static recompiler: Sega CD 68000 code (main and sub CPU) -> C++.

    python3 recomp.py <game dir> <config dir> <out dir> [functions per file]

<game dir> is made by tools/prepare_disc.py. config/images.txt lists the code images,
config/functions.txt adds entry points and names by hand.

Model (see src/runtime/m68k.h):
  * one C++ function per guest function: void <prefix>_XXXXXX(Cpu& c, uint32_t pc)
    `pc` picks where to start: the entry, a return address after a call, a jump-table target,
    or a loop head where the CPU may have been interrupted. Anything else -> c.miss(pc).
  * bsr/jsr push the return address on the guest stack and call the host function. rts pops it
    into c.ret and returns. After a call, if c.ret is not the expected return address (the
    callee dropped or replaced its return address), this function returns too, so the address
    unwinds to whoever expects it, or to the top-level loop, which dispatches it.
  * indirect jmp/jsr and branches out of the function go through c.jump(addr), which finds the
    image + function that owns addr (signature words tell overlays at the same address apart).
  * cycles are counted per instruction; at loop heads `c.cycles >= c.limit` hands control to
    the other CPU / hardware (c.sync), which is also where interrupts are taken.
"""
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m68k import decode, CC  # noqa: E402

TERM = {'rts', 'rte', 'rtr', 'jmp', 'bra', 'illegal', 'linea', 'linef', 'stop', 'reset_never'}


class Image:
    def __init__(self, cpu, name, base, data, prefix, entries):
        self.cpu, self.name, self.base, self.data, self.prefix = cpu, name, base, data, prefix
        self.end = base + len(data)
        self.entries = set(entries)
        self.cache = {}

    def has(self, a):
        return self.base <= a < self.end - 1

    def w(self, a):
        o = a - self.base
        if 0 <= o < len(self.data) - 1:
            return struct.unpack_from('>H', self.data, o)[0]
        return 0x4afc

    def l(self, a):
        return (self.w(a) << 16) | self.w(a + 2)

    def insn(self, a):
        i = self.cache.get(a)
        if i is None:
            i = decode(self.w, a)
            self.cache[a] = i
        return i

    def sig(self):
        """Signature: first 8 longwords (overlays sharing an address differ there)."""
        return [self.l(self.base + 4 * k) for k in range(8)]


def load_images(game, cfg):
    imgs = []
    for line in open(os.path.join(cfg, 'images.txt')):
        line = line.split('#')[0].split()
        if not line:
            continue
        cpu, fname, base, prefix = line[0], line[1], int(line[2], 16), line[3]
        sl = None
        if '@' in fname:            # a slice of a file: NAME@offset+length (hex)
            fname, rng = fname.split('@')
            off, ln = rng.split('+')
            sl = (int(off, 16), int(ln, 16))
        p = os.path.join(game, fname)
        if not os.path.exists(p):
            print('missing', p)
            continue
        data = open(p, 'rb').read()
        if sl:
            data = data[sl[0]:sl[0] + sl[1]]
            fname = f'{fname}@{sl[0]:x}'
        im = Image(cpu, fname, base, data, prefix, [])
        for e in line[4:]:
            if e == 'hdr':          # sub-CPU module header ("MAIN ..."): word offsets at +$18
                so = struct.unpack_from('>I', data, 0x18)[0]
                k = 0
                while True:
                    off = struct.unpack_from('>H', data, so + 2 * k)[0]
                    if off == 0:
                        break
                    im.entries.add(base + so + off)
                    k += 1
            elif e == 'jmptab':     # nop/nop then jmp abs.l ... at the start
                a = base
                while im.w(a) == 0x4e71:
                    a += 2
                while im.w(a) == 0x4ef9:
                    im.entries.add(a)
                    im.entries.add(im.l(a + 2) & 0xffffff)
                    a += 6
            elif e == 'ovl':        # $28000 overlay: header longs, last one before the name = entry
                ptrs = []
                a = base
                while im.l(a) >> 16 == base >> 16 and len(ptrs) < 4:
                    ptrs.append(im.l(a))
                    a += 4
                if len(ptrs) == 3:
                    im.entries.add(ptrs[2])
            else:
                im.entries.add(int(e, 16))
        imgs.append(im)
    names = {}
    fn = os.path.join(cfg, 'functions.txt')
    if os.path.exists(fn):
        for line in open(fn):
            line = line.split('#')[0].split()
            if len(line) >= 2:
                pre, a = line[0], int(line[1], 16)
                for im in imgs:
                    if im.prefix == pre:
                        im.entries.add(a)
                        if len(line) > 2:
                            names[(pre, a)] = line[2]
    return imgs, names


def jump_table(im, i, prev):
    """Targets of `move.w T(pc,dn.w),dn ; jmp T(pc,dn.w)` and `movea.l T(pc,dn.w),an ; jmp (an)`."""
    if not prev:
        return None
    o = i.ops[0]
    p = prev.ops[0] if prev.ops else None
    if o[0] == 'pcx' and prev.op == 'move' and prev.size == 2 and p and p[0] == 'pcx':
        t = (p[1] + p[2]) & 0xffffff
        if (o[1] + o[2]) & 0xffffff != t:
            return None
        out, lim = [], 1 << 30
        for k in range(256):
            if t + 2 * k >= lim or not im.has(t + 2 * k):
                break
            off = struct.unpack('>h', struct.pack('>H', im.w(t + 2 * k)))[0]
            tg = t + off
            if tg & 1 or not im.has(tg) or off == 0:
                break
            out.append(tg)
            if tg > t:
                lim = min(lim, tg)
        return out or None
    if o[0] == 'ind' and prev.op == 'movea' and prev.size == 4 and p and p[0] == 'pcx' and prev.ops[1] == ('a', o[1]):
        t = (p[1] + p[2]) & 0xffffff
        out, lim = [], 1 << 30
        for k in range(256):
            if t + 4 * k >= lim:
                break
            tg = im.l(t + 4 * k) & 0xffffff
            if tg & 1 or not im.has(tg):
                break
            out.append(tg)
            if tg > t:
                lim = min(lim, tg)
        return out or None
    return None


class Func:
    def __init__(self, im, entry):
        self.im, self.entry = im, entry
        self.insns = {}         # addr -> Insn
        self.labels = set()     # branch targets inside
        self.resume = {entry}   # addresses the function can be (re)entered at
        self.tables = {}        # jmp addr -> targets
        self.calls = set()
        self.loops = set()


def explore(im, entry, entries):
    f = Func(im, entry)
    work, prev_of = [entry], {}
    while work:
        a = work.pop()
        while im.has(a) and a not in f.insns:
            if a != entry and a in entries and a not in f.labels:
                break           # falls into another function: tail call there
            i = im.insn(a)
            if i.op == 'illegal':
                f.insns[a] = i
                break
            f.insns[a] = i
            nxt = i.end
            if i.op in ('bsr', 'jsr'):
                t = i.target if i.op == 'bsr' else const_target(i)
                if t is not None and im.has(t):
                    f.calls.add(t)
                f.resume.add(nxt)
            elif i.op in ('bcc', 'dbcc', 'bra'):
                t = i.target
                if im.has(t) and (t not in entries or t == entry):
                    f.labels.add(t)
                    work.append(t)
                    if t <= a:
                        f.loops.add(t)
            elif i.op == 'jmp':
                t = const_target(i)
                if t is not None and im.has(t) and (t not in entries or t == entry):
                    f.labels.add(t)
                    work.append(t)
                    if t <= a:
                        f.loops.add(t)
                elif t is None:
                    tab = jump_table(im, i, prev_of.get(a))
                    if tab:
                        f.tables[a] = tab
                        for t in tab:
                            f.labels.add(t)
                            f.resume.add(t)
                            work.append(t)
            if i.op in TERM:
                break
            prev_of[nxt] = i
            a = nxt
    f.resume |= f.loops
    return f


def const_target(i):
    o = i.ops[0]
    if o[0] in ('absw', 'absl', 'pcd'):
        return o[1] & 0xffffff      # code addresses: the bus is 24 bits wide
    return None


def discover(im, names):
    entries = {e for e in im.entries if im.has(e)}
    while True:
        new = set()
        for e in entries:
            f = explore(im, e, entries)
            new |= {t for t in f.calls if im.has(t)}
            for tab in f.tables.values():
                pass
        if new <= entries:
            break
        entries |= new
    return {e: explore(im, e, entries) for e in sorted(entries)}


# ---------------------------------------------------------------- C++ emission

SZ = {1: '8', 2: '16', 4: '32'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}


class Emit:
    def __init__(self, f, fname, funcs, img_of):
        self.f, self.fname, self.funcs, self.img_of = f, fname, funcs, img_of
        self.out = []
        self.n = 0

    def tmp(self):
        self.n += 1
        return f't{self.n}'

    # operand access ------------------------------------------------------
    def ea(self, o, size):
        """Return (setup lines, address expression) for a memory operand."""
        k = o[0]
        step = 2 if (size == 1 and o[1] == 7) else size
        if k == 'ind':
            return [], f'c.a[{o[1]}]'
        if k == 'post':
            t = self.tmp()
            return [f'uint32_t {t} = c.a[{o[1]}]; c.a[{o[1]}] += {step};'], t
        if k == 'pre':
            t = self.tmp()
            return [f'c.a[{o[1]}] -= {step}; uint32_t {t} = c.a[{o[1]}];'], t
        if k == 'disp':
            return [], f'(c.a[{o[1]}] + {o[2]})'
        if k == 'idx':
            return [], f'(c.a[{o[1]}] + {o[2]} + {self.xr(o[3], o[4])})'
        if k in ('absw', 'absl', 'pcd'):
            return [], f'0x{o[1]:06x}u'     # full 32-bit value (lea/pea keep bits 24-31)
        if k == 'pcx':
            return [], f'(0x{o[1]:06x}u + {o[2]} + {self.xr(o[3], o[4])})'
        raise ValueError(o)

    @staticmethod
    def xr(r, lng):
        reg = f'c.{r[0]}[{r[1]}]'
        return reg if lng else f'(uint32_t)(int16_t){reg}'

    def operand(self, o, size, lines, need_addr=False):
        """Prepare an operand; returns (read expr, write(value)->line, addr expr or None)."""
        k = o[0]
        if k == 'd':
            r = f'c.d[{o[1]}]'
            if size == 4:
                return r, (lambda v: f'{r} = {v};'), None
            m = MASK[size]
            return f'({r} & {m})', (lambda v: f'{r} = ({r} & ~{m}) | (({v}) & {m});'), None
        if k == 'a':
            r = f'c.a[{o[1]}]'
            if size == 4:
                return r, (lambda v: f'{r} = {v};'), None
            return f'({r} & 0xffffu)', (lambda v: f'{r} = (uint32_t)(int16_t)({v});'), None
        if k == 'imm':
            return f'0x{o[1] & 0xffffffff:x}u', None, None
        if k == 'ccr':
            return 'c.get_ccr()', (lambda v: f'c.set_ccr({v});'), None
        if k == 'sr':
            return 'c.get_sr()', (lambda v: f'c.set_sr({v});'), None
        if k == 'usp':
            return 'c.usp', (lambda v: f'c.usp = {v};'), None
        setup, addr = self.ea(o, size)
        lines += setup
        if any(ch in addr for ch in '+(') and not addr.startswith('0x'):
            t = self.tmp()
            lines.append(f'uint32_t {t} = {addr};')
            addr = t
        b = SZ[size]
        return f'c.rd{b}({addr})', (lambda v: f'c.wr{b}({addr}, {v});'), addr

    # instructions ---------------------------------------------------------
    def label(self, a):
        return f'L_{a:06x}'

    def goto(self, a):
        """Branch to address a: inside this function -> goto, else a tail call."""
        if a in self.f.insns and (a == self.f.entry or a not in self.funcs):
            return f'goto {self.label(a)};'
        return f'{{ c.jump(0x{a:06x}u); return; }}'

    def cond(self, cc):
        return {0: 'true', 1: 'false'}.get(cc, f'c.cond_{CC[cc]}()')

    def call(self, t, ret, lines):
        lines.append(f'c.push32(0x{ret:06x}u); c.enter(0x{ret:06x}u);')
        if t is not None and t in self.funcs:
            lines.append(f'{self.fname[t]}(c, 0x{t:06x}u);')
        elif t is not None:
            lines.append(f'c.jump(0x{t:06x}u);')
        else:
            return False
        lines.append(f'c.leave(); if (c.ret != 0x{ret:06x}u) return;')
        return True

    def insn(self, i):
        L = []
        op, s, o = i.op, i.size, i.ops
        L.append(f'c.cycles += {cycles(i)};')
        S = s if s else 4
        if op == 'move' and o[1][0] in ('ccr', 'sr', 'usp') or op == 'move' and o[0][0] in ('sr', 'usp'):
            rv, _, _ = self.operand(o[0], 2 if o[0][0] != 'usp' else 4, L)
            _, wr, _ = self.operand(o[1], 2 if o[1][0] != 'usp' else 4, L)
            L.append(wr(rv))
            if o[1][0] == 'sr':
                L.append(f'if (!c.check_irq(0x{i.end:06x}u)) return;')
        elif op in ('move', 'moveq'):
            rv, _, _ = self.operand(o[0], S, L)
            t = self.tmp()
            L.append(f'uint32_t {t} = {rv};')
            _, wr, _ = self.operand(o[1], S, L)
            L.append(wr(t))
            L.append(f'c.logic<{S}>({t});')
        elif op == 'movea':
            rv, _, _ = self.operand(o[0], S, L)
            L.append(f'c.a[{o[1][1]}] = {"(uint32_t)(int16_t)(" + rv + ")" if S == 2 else rv};')
        elif op in ('add', 'sub', 'and', 'or', 'eor', 'addi', 'subi', 'andi', 'ori', 'eori', 'addq', 'subq',
                    'cmp', 'cmpi', 'cmpm', 'addx', 'subx'):
            base = {'addi': 'add', 'addq': 'add', 'subi': 'sub', 'subq': 'sub', 'andi': 'and', 'ori': 'or',
                    'eori': 'eor', 'cmpi': 'cmp', 'cmpm': 'cmp'}.get(op, op)
            if o[1][0] in ('ccr', 'sr'):
                rv, _, _ = self.operand(o[0], s, L)
                cur = 'c.get_ccr()' if o[1][0] == 'ccr' else 'c.get_sr()'
                setf = 'c.set_ccr' if o[1][0] == 'ccr' else 'c.set_sr'
                sym = {'and': '&', 'or': '|', 'eor': '^'}[base]
                L.append(f'{setf}({cur} {sym} {rv});')
                if o[1][0] == 'sr':
                    L.append(f'if (!c.check_irq(0x{i.end:06x}u)) return;')
            elif o[1][0] == 'a':        # addq/subq to An: whole register, no flags
                rv, _, _ = self.operand(o[0], S, L)
                sym = '+' if base == 'add' else '-'
                L.append(f'c.a[{o[1][1]}] {sym}= {rv};')
            else:
                rv, _, _ = self.operand(o[0], S, L)
                t = self.tmp()
                L.append(f'uint32_t {t} = {rv};')
                dv, wr, _ = self.operand(o[1], S, L)
                fn = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'and': 'logic_and', 'or': 'logic_or',
                      'eor': 'logic_eor', 'addx': 'addx', 'subx': 'subx'}[base]
                if base == 'cmp':
                    L.append(f'c.cmp<{S}>({t}, {dv});')
                else:
                    L.append(wr(f'c.{fn}<{S}>({t}, {dv})'))
        elif op in ('adda', 'suba', 'cmpa'):
            rv, _, _ = self.operand(o[0], S, L)
            v = f'(uint32_t)(int16_t)({rv})' if S == 2 else rv
            n = o[1][1]
            if op == 'cmpa':
                L.append(f'c.cmp<4>({v}, c.a[{n}]);')
            else:
                L.append(f'c.a[{n}] {"+" if op == "adda" else "-"}= {v};')
        elif op in ('neg', 'negx', 'not', 'clr', 'tst', 'nbcd'):
            dv, wr, _ = self.operand(o[0], S, L)
            if op == 'tst':
                L.append(f'c.logic<{S}>({dv});')
            elif op == 'clr':
                L.append(wr('0'))
                L.append(f'c.logic<{S}>(0);')
            elif op == 'not':
                L.append(wr(f'c.logic_eor<{S}>({MASK[S]}, {dv})'))
            else:
                L.append(wr(f'c.{op}<{S}>({dv})'))
        elif op in ('abcd', 'sbcd'):
            rv, _, _ = self.operand(o[0], 1, L)
            t = self.tmp()
            L.append(f'uint32_t {t} = {rv};')
            dv, wr, _ = self.operand(o[1], 1, L)
            L.append(wr(f'c.{op}({t}, {dv})'))
        elif op in ('mulu', 'muls', 'divu', 'divs'):
            rv, _, _ = self.operand(o[0], 2, L)
            n = o[1][1]
            if op.startswith('mul'):
                L.append(f'c.d[{n}] = c.{op}({rv}, c.d[{n}]);')
            else:
                L.append(f'if (!c.{op}({rv}, c.d[{n}], 0x{i.end:06x}u)) return;')
        elif op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
            if o[1][0] == 'd':
                cnt = f'0x{o[0][1]:x}u' if o[0][0] == 'imm' else f'(c.d[{o[0][1]}] & 63)'
                dv, wr, _ = self.operand(o[1], S, L)
                L.append(wr(f'c.{op}<{S}>({cnt}, {dv})'))
            else:
                dv, wr, _ = self.operand(o[1], 2, L)
                L.append(wr(f'c.{op}<2>(1, {dv})'))
        elif op in ('btst', 'bchg', 'bclr', 'bset'):
            if o[0][0] == 'imm':
                bit = f'{o[0][1] & (31 if S == 4 else 7)}'
            else:
                bit = f'(c.d[{o[0][1]}] & {31 if S == 4 else 7})'
            dv, wr, _ = self.operand(o[1], S, L)
            t = self.tmp()
            L.append(f'uint32_t {t} = {dv}; c.z = !(({t} >> {bit}) & 1);')
            if op != 'btst':
                expr = {'bchg': f'{t} ^ (1u << {bit})', 'bclr': f'{t} & ~(1u << {bit})',
                        'bset': f'{t} | (1u << {bit})'}[op]
                L.append(wr(expr))
        elif op == 'scc':
            _, wr, _ = self.operand(o[0], 1, L)
            L.append(wr(f'({self.cond(i.cc)} ? 0xffu : 0u)'))
        elif op == 'dbcc':
            n = o[0][1]
            L.append(f'if (!({self.cond(i.cc)})) {{ uint16_t w = (uint16_t)(c.d[{n}] - 1); '
                     f'c.d[{n}] = (c.d[{n}] & 0xffff0000u) | w; if (w != 0xffff) {self.goto_sync(i.target, i.addr)} }}')
        elif op == 'bcc':
            L.append(f'if ({self.cond(i.cc)}) {self.goto_sync(i.target, i.addr)}')
        elif op == 'bra':
            L.append(self.goto_sync(i.target, i.addr))
        elif op in ('bsr', 'jsr'):
            t = i.target if op == 'bsr' else const_target(i)
            if t is None:
                _, addr = self.ea(o[0], 4)
                tv = self.tmp()
                L.append(f'uint32_t {tv} = {addr};')
                L.append(f'c.push32(0x{i.end:06x}u); c.enter(0x{i.end:06x}u); c.jump({tv}); c.leave(); '
                         f'if (c.ret != 0x{i.end:06x}u) return;')
            else:
                self.call(t, i.end, L)
        elif op == 'jmp':
            t = const_target(i)
            if t is not None:
                L.append(self.goto_sync(t, i.addr))
            else:
                setup, addr = self.ea(o[0], 4)
                L += setup
                tv = self.tmp()
                L.append(f'uint32_t {tv} = {addr};')
                tab = self.f.tables.get(i.addr)
                if tab:
                    L.append(f'switch ({tv} & 0xffffff) {{')
                    for t in sorted(set(tab)):
                        L.append(f'  case 0x{t:06x}: goto {self.label(t)};')
                    L.append('}')
                L.append(f'c.jump({tv}); return;')
        elif op == 'rts':
            L.append('c.ret = c.pop32(); return;')
        elif op == 'rtr':
            L.append('c.set_ccr(c.pop16()); c.ret = c.pop32(); return;')
        elif op == 'rte':
            L.append('c.rte(); return;')
        elif op == 'lea':
            setup, addr = self.ea(o[0], 4)
            L += setup
            L.append(f'c.a[{o[1][1]}] = {addr};')
        elif op == 'pea':
            setup, addr = self.ea(o[0], 4)
            L += setup
            t = self.tmp()
            L.append(f'uint32_t {t} = {addr}; c.push32({t});')
        elif op == 'link':
            n = o[0][1]
            if n == 7:      # link a7: the value pushed is the already decremented a7
                L.append(f'c.a[7] -= 4; c.wr32(c.a[7], c.a[7]); c.a[7] += {o[1][1]};')
            else:
                L.append(f'c.push32(c.a[{n}]); c.a[{n}] = c.a[7]; c.a[7] += {o[1][1]};')
        elif op == 'unlk':
            n = o[0][1]
            L.append(f'c.a[7] = c.a[{n}]; c.a[{n}] = c.pop32();')
        elif op == 'swap':
            n = o[0][1]
            L.append(f'c.d[{n}] = (c.d[{n}] >> 16) | (c.d[{n}] << 16); c.logic<4>(c.d[{n}]);')
        elif op == 'ext':
            n = o[0][1]
            if S == 2:
                L.append(f'c.d[{n}] = (c.d[{n}] & 0xffff0000u) | (uint16_t)(int8_t)c.d[{n}]; c.logic<2>(c.d[{n}]);')
            else:
                L.append(f'c.d[{n}] = (uint32_t)(int16_t)c.d[{n}]; c.logic<4>(c.d[{n}]);')
        elif op == 'exg':
            r1 = f'c.{o[0][0]}[{o[0][1]}]'
            r2 = f'c.{o[1][0]}[{o[1][1]}]'
            L.append(f'{{ uint32_t t = {r1}; {r1} = {r2}; {r2} = t; }}')
        elif op == 'movem':
            self.movem(i, L)
        elif op == 'movep':
            self.movep(i, L)
        elif op == 'chk':
            rv, _, _ = self.operand(o[0], 2, L)
            L.append(f'if (!c.chk({rv}, c.d[{o[1][1]}], 0x{i.end:06x}u)) return;')
        elif op == 'tas':
            dv, wr, _ = self.operand(o[0], 1, L)
            t = self.tmp()
            L.append(f'uint32_t {t} = {dv}; c.logic<1>({t});')
            L.append(wr(f'{t} | 0x80u'))
        elif op == 'trap':
            L.append(f'c.exception(32 + {o[0][1]}, 0x{i.end:06x}u); if (c.ret != 0x{i.end:06x}u) return;')
        elif op == 'trapv':
            L.append(f'if (c.v) {{ c.exception(7, 0x{i.end:06x}u); if (c.ret != 0x{i.end:06x}u) return; }}')
        elif op == 'stop':
            L.append(f'c.set_sr(0x{o[0][1]:x}u); c.stop(0x{i.end:06x}u); if (c.ret != 0x{i.end:06x}u) return;')
        elif op == 'reset':
            L.append('c.reset_devices();')
        elif op == 'nop':
            pass
        else:   # illegal, linea, linef
            vec = {'illegal': 4, 'linea': 10, 'linef': 11}[op]
            L.append(f'c.exception({vec}, 0x{i.addr:06x}u); return;')
        return L

    def goto_sync(self, t, at):
        """Branch; backward branches are loop heads where the CPU can be interrupted."""
        inside = t in self.f.insns and (t == self.f.entry or t not in self.funcs)
        if inside and t <= at:
            return f'{{ if (c.cycles >= c.limit && !c.sync(0x{t:06x}u)) return; goto {self.label(t)}; }}'
        return self.goto(t)

    def movem(self, i, L):
        S = i.size
        if i.ops[0][0] == 'regs':
            mask, dst = i.ops[0][1], i.ops[1]
            regs = [('d', k) if k < 8 else ('a', k - 8) for k in range(16) if mask >> k & 1]
            if dst[0] == 'pre':
                n = dst[1]
                keep = ('a', n) in regs
                L.append(f'{{ uint32_t ea = c.a[{n}];' + (f' uint32_t a0v = c.a[{n}];' if keep else ''))
                for r in reversed(regs):
                    v = 'a0v' if r == ('a', n) else f'c.{r[0]}[{r[1]}]'
                    L.append(f'  ea -= {S}; c.wr{SZ[S]}(ea, {v});')
                L.append(f'  c.a[{n}] = ea; }}')
            else:
                setup, addr = self.ea(dst, S)
                L += setup
                L.append(f'{{ uint32_t ea = {addr};')
                for r in regs:
                    L.append(f'  c.wr{SZ[S]}(ea, c.{r[0]}[{r[1]}]); ea += {S};')
                L.append('}')
        else:
            src, mask = i.ops
            mask = mask[1]
            regs = [('d', k) if k < 8 else ('a', k - 8) for k in range(16) if mask >> k & 1]
            if src[0] == 'post':
                n = src[1]
                L.append(f'{{ uint32_t ea = c.a[{n}];')
            else:
                setup, addr = self.ea(src, S)
                L += setup
                L.append(f'{{ uint32_t ea = {addr};')
            for r in regs:
                rd = f'c.rd16(ea)' if S == 2 else 'c.rd32(ea)'
                v = f'(uint32_t)(int16_t){rd}' if S == 2 else rd
                L.append(f'  c.{r[0]}[{r[1]}] = {v}; ea += {S};')
            if src[0] == 'post':
                L.append(f'  c.a[{n}] = ea;')
            L.append('}')

    def movep(self, i, L):
        S = i.size
        o = i.ops
        if o[0][0] == 'd':
            n, (_, an, disp) = o[0][1], o[1]
            L.append(f'{{ uint32_t ea = c.a[{an}] + {disp};')
            for k in range(S):
                sh = 8 * (S - 1 - k)
                L.append(f'  c.wr8(ea + {2 * k}, (c.d[{n}] >> {sh}) & 0xff);')
            L.append('}')
        else:
            (_, an, disp), n = o[0], o[1][1]
            L.append(f'{{ uint32_t ea = c.a[{an}] + {disp}, v = 0;')
            for k in range(S):
                L.append(f'  v = (v << 8) | c.rd8(ea + {2 * k});')
            m = '0xffff0000u' if S == 2 else '0'
            L.append(f'  c.d[{n}] = (c.d[{n}] & {m}) | v; }}')

    def run(self, name):
        f = self.f
        addrs = sorted(f.insns)
        out = self.out
        out.append(f'void {name}(Cpu& c, uint32_t pc) {{')
        out.append('  switch (pc) {')
        for a in sorted(f.resume):
            if a in f.insns:
                out.append(f'    case 0x{a:06x}: goto {self.label(a)};')
        out.append('    default: c.miss(pc); return;')
        out.append('  }')
        for k, a in enumerate(addrs):
            i = f.insns[a]
            out.append(f'{self.label(a)}:;')
            out.append(f'  {{ // {i!r}')
            for line in self.insn(i):
                out.append('    ' + line)
            out.append('  }')
            nxt = i.end
            falls = i.op not in TERM
            if falls and (k + 1 == len(addrs) or addrs[k + 1] != nxt):
                out.append('  ' + self.goto(nxt))
        out.append('}')
        return out


def cycles(i):
    """Rough 68000 cycle cost (base + 4 per extension word / memory operand)."""
    c = 4 + 2 * (i.len - 2)
    for o in i.ops:
        if o[0] in ('ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl', 'pcd', 'pcx'):
            c += 4 if i.size != 4 else 8
    if i.op in ('mulu', 'muls'):
        c += 50
    elif i.op in ('divu', 'divs'):
        c += 120
    elif i.op in ('bsr', 'jsr', 'rts', 'rte'):
        c += 12
    elif i.op == 'movem':
        c += 4 * bin(i.ops[0][1] if i.ops[0][0] == 'regs' else i.ops[1][1]).count('1')
    return c


def main():
    game, cfg, outd = sys.argv[1:4]
    per_file = int(sys.argv[4]) if len(sys.argv) > 4 else 150
    os.makedirs(outd, exist_ok=True)
    imgs, names = load_images(game, cfg)
    allf = []           # (image, func, c name)
    for im in imgs:
        funcs = discover(im, names)
        fname = {e: names.get((im.prefix, e), f'{im.prefix}_{e:06x}') for e in funcs}
        n_ins = sum(len(f.insns) for f in funcs.values())
        cover = len(set(a for f in funcs.values() for a in f.insns))
        print(f'{im.name}: {len(funcs)} functions, {cover} instructions covered ({n_ins} emitted)')
        for e, f in sorted(funcs.items()):
            allf.append((im, f, fname[e], funcs, fname))
    hdr = ['// generated by src/recomp.py - do not edit', '#pragma once', '#include "../runtime/m68k.h"', '']
    for im, f, name, _, _ in allf:
        hdr.append(f'void {name}(Cpu& c, uint32_t pc);')
    open(os.path.join(outd, 'funcs.h'), 'w').write('\n'.join(hdr) + '\n')
    chunks = [allf[k:k + per_file] for k in range(0, len(allf), per_file)]
    for k, ch in enumerate(chunks):
        lines = ['// generated by src/recomp.py - do not edit', '#include "funcs.h"', '']
        for im, f, name, funcs, fname in ch:
            lines += Emit(f, fname, funcs, None).run(name)
            lines.append('')
        open(os.path.join(outd, f'funcs_{k:03d}.cpp'), 'w').write('\n'.join(lines) + '\n')
    # image table: cpu, base, end, signature, functions with resume points
    t = ['// generated by src/recomp.py - do not edit', '#include "funcs.h"', '#include "../runtime/table.h"', '']
    for im in imgs:
        rows = {}
        for (i2, f, n, _, _) in allf:
            if i2 is im:
                for a in f.resume:
                    if a in f.insns:
                        # an address several functions can resume at: prefer the one that starts there
                        if a not in rows or a == f.entry:
                            rows[a] = n
        t.append(f'static const FuncEntry fe_{im.prefix}[] = {{   // sorted: the runtime binary-searches it')
        for a in sorted(rows):
            t.append(f'  {{0x{a:06x}, {rows[a]}}},')
        t.append('};')
    t.append('const ImageEntry g_images[] = {')
    for im in imgs:
        sig = ', '.join(f'0x{x:08x}u' for x in im.sig())
        cnt = f'sizeof(fe_{im.prefix}) / sizeof(fe_{im.prefix}[0])'
        t.append(f'  {{"{im.name}", {1 if im.cpu == "sub" else 0}, 0x{im.base:06x}, 0x{im.end:06x}, {{{sig}}}, '
                 f'fe_{im.prefix}, {cnt}}},')
    t.append('};')
    t.append(f'const int g_image_count = {len(imgs)};')
    open(os.path.join(outd, 'table.cpp'), 'w').write('\n'.join(t) + '\n')
    print(f'{len(allf)} functions -> {len(chunks)} files in {outd}')


if __name__ == '__main__':
    main()
