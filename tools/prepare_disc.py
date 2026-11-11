#!/usr/bin/env python3
"""Extract the Snatcher (Sega CD, US) data track into a game directory.

    python3 prepare_disc.py <disc.cue> <out dir>

Writes the data track as track01.iso, the CD audio tracks as trackNN.raw + tracks.txt, the boot
programs IP.bin (main CPU, loaded at $FF0000)
and SP.bin (sub CPU, loaded at $6000), and every file of the ISO9660 root (PCMLD_01.BIN, the
85 MB voice bank, is skipped unless --all is given). Dev tool only - not shipped with the game.
"""
import os
import re
import struct
import sys


def read_cue(cue):
    bin_name, tracks, cur = None, [], None
    for line in open(cue, encoding='latin-1'):
        line = line.strip()
        m = re.match(r'FILE "(.+)"', line)
        if m:
            bin_name = os.path.join(os.path.dirname(cue), m.group(1))
        m = re.match(r'TRACK (\d+) (\S+)', line)
        if m:
            cur = {'n': int(m.group(1)), 'mode': m.group(2)}
            tracks.append(cur)
        m = re.match(r'INDEX (\d+) (\d+):(\d+):(\d+)', line)
        if m and cur is not None:
            mm, ss, ff = int(m.group(2)), int(m.group(3)), int(m.group(4))
            cur['i%d' % int(m.group(1))] = (mm * 60 + ss) * 75 + ff
    return bin_name, tracks


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    keep_all = '--all' in sys.argv
    cue, out = args
    os.makedirs(out, exist_ok=True)
    bin_name, tracks = read_cue(cue)
    t1 = tracks[0]
    assert t1['mode'] == 'MODE1/2352', t1
    end = tracks[1].get('i0', tracks[1]['i1']) if len(tracks) > 1 else os.path.getsize(bin_name) // 2352
    iso = bytearray()
    with open(bin_name, 'rb') as f:
        f.seek(t1['i1'] * 2352)
        for _ in range(end - t1['i1']):
            iso += f.read(2352)[16:16 + 2048]
    iso = bytes(iso)
    assert iso[:14] == b'SEGADISCSYSTEM', 'not a Sega CD disc'
    open(os.path.join(out, 'track01.iso'), 'wb').write(iso)
    ip_off, ip_size, sp_off, sp_size = struct.unpack('>4I', iso[0x30:0x38] + iso[0x40:0x48])
    open(os.path.join(out, 'IP.bin'), 'wb').write(iso[0x200:ip_off + ip_size])
    open(os.path.join(out, 'SP.bin'), 'wb').write(iso[sp_off:sp_off + sp_size])
    pvd = iso[16 * 2048:17 * 2048]
    ext, ln = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    d, p, n = iso[ext * 2048:ext * 2048 + ln], 0, 0
    while p < len(d):
        rl = d[p]
        if rl == 0:
            p = (p // 2048 + 1) * 2048
            continue
        e, sz = struct.unpack_from('<I', d, p + 2)[0], struct.unpack_from('<I', d, p + 10)[0]
        name = d[p + 33:p + 33 + d[p + 32]]
        if name not in (b'\x00', b'\x01') and not d[p + 25] & 2:
            name = name.decode('ascii').split(';')[0]
            if keep_all or name != 'PCMLD_01.BIN':
                open(os.path.join(out, name), 'wb').write(iso[e * 2048:e * 2048 + sz])
                n += 1
        p += rl
    # CD audio: tracks.txt (number, start sector on disc, length in sectors) + trackNN.raw (44.1 kHz
    # 16-bit stereo little endian, from INDEX 01 to the next track)
    total = os.path.getsize(bin_name) // 2352
    with open(os.path.join(out, 'tracks.txt'), 'w') as tf, open(bin_name, 'rb') as f:
        for k, t in enumerate(tracks):
            start = t['i1']
            nxt = tracks[k + 1].get('i0', tracks[k + 1]['i1']) if k + 1 < len(tracks) else total
            tf.write(f"{t['n']} {start} {nxt - start} {'data' if t['mode'] != 'AUDIO' else 'audio'}\n")
            if t['mode'] == 'AUDIO':
                f.seek(start * 2352)
                with open(os.path.join(out, f"track{t['n']:02d}.raw"), 'wb') as af:
                    left = (nxt - start) * 2352
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        af.write(chunk)
                        left -= len(chunk)
    print(f'{out}: track01.iso ({len(iso)} bytes), IP.bin, SP.bin, {n} files, {len(tracks) - 1} audio tracks')


if __name__ == '__main__':
    main()
