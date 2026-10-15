#!/usr/bin/env python3
"""
Edit a GBADoom WAD: add maps or other lumps from the shareware IWAD, drop
lumps.

  python edit_wad.py --iwad doom1.wad --into ../wad/doom1_e1m1_sfx.wad \\
                     --maps E1M2 E1M3 --add D_E1M1 --drop 'WILV1*' \\
                     --out ../wad/out.wad

GBADoom reads the map lumps in place from flash, so they have to be in the
layout GbaWadUtil (github.com/doomhack/GbaWadUtil, wadprocessor.cpp)
writes: VERTEXES, LINEDEFS, SEGS and SIDEDEFS are converted to the
engine's own structs, the other map lumps are copied unchanged. This is a
port of that conversion.

Before writing anything, every map already present in --into is converted
from --iwad as well and compared byte for byte, so a conversion that
drifts from GbaWadUtil stops here instead of on the console.

--add copies lumps from --iwad unchanged and appends them. --drop takes
names or shell patterns; a map marker or map lump cannot be dropped this
way, and a pattern that matches nothing is an error.
"""
import argparse
import fnmatch
import os
import struct
import sys

MAP_LUMPS = ('THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS',
             'SSECTORS', 'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP')

NO_INDEX = 0xFFFF
ML_TWOSIDED = 4
ST_HORIZONTAL, ST_VERTICAL, ST_POSITIVE, ST_NEGATIVE = range(4)
INT_MAX = 0x7FFFFFFF


def read_wad(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] not in (b'IWAD', b'PWAD'):
        sys.exit('%s: not a WAD' % path)
    num, infotable = struct.unpack_from('<II', data, 4)
    lumps = []
    for i in range(num):
        off = infotable + i * 16
        pos, size = struct.unpack_from('<II', data, off)
        name = data[off + 8:off + 16].split(b'\0', 1)[0].decode('ascii')
        lumps.append((name, data[pos:pos + size]))
    return data[:4], lumps


def is_map_marker(name):
    return len(name) == 4 and name[0] == 'E' and name[2] == 'M'


def find_map(lumps, name):
    """Index of the map marker, or -1."""
    for i, (n, _) in enumerate(lumps):
        if n == name and [l[0] for l in lumps[i + 1:i + 11]] == list(MAP_LUMPS):
            return i
    return -1


def s32(x):
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x & 0x80000000 else x


def fixed_div(a, b):
    """FixedDiv as in GbaWadUtil's doomtypes.h, overflow case included."""
    if (abs(a) >> 14) >= abs(b):
        return s32(((a ^ b) >> 31) ^ INT_MAX)
    q = abs(a << 16) // abs(b)          # C division truncates toward zero
    return s32(q if (a < 0) == (b < 0) else -q)


def texture_names(lumps):
    """Texture numbers as R_TextureNumForName counts them: TEXTURE1, then
    TEXTURE2."""
    names = []
    for tex in ('TEXTURE1', 'TEXTURE2'):
        data = next((b for n, b in lumps if n == tex), None)
        if data is None:
            continue
        count = struct.unpack_from('<i', data, 0)[0]
        for i in range(count):
            off = struct.unpack_from('<i', data, 4 + 4 * i)[0]
            names.append(data[off:off + 8].split(b'\0', 1)[0])
    return names


def convert_map(group, textures):
    """group: dict lump name -> raw IWAD bytes. Returns the converted dict."""
    out = dict(group)

    raw_vtx = group['VERTEXES']
    vtx = [(x << 16, y << 16) for x, y in
           struct.iter_unpack('<hh', raw_vtx[:len(raw_vtx) // 4 * 4])]
    out['VERTEXES'] = b''.join(struct.pack('<ii', x, y) for x, y in vtx)

    raw_lines = group['LINEDEFS']
    lines = list(struct.iter_unpack('<HHHhhHH',
                                    raw_lines[:len(raw_lines) // 14 * 14]))
    new_lines = []
    for i, (v1, v2, flags, special, tag, s0, s1) in enumerate(lines):
        (x1, y1), (x2, y2) = vtx[v1], vtx[v2]
        dx, dy = s32(x2 - x1), s32(y2 - y1)
        slope = (ST_VERTICAL if not dx else ST_HORIZONTAL if not dy else
                 ST_POSITIVE if fixed_div(dy, dx) > 0 else ST_NEGATIVE)
        bbox = (max(y1, y2), min(y1, y2), min(x1, x2), max(x1, x2))
        new_lines.append(struct.pack('<iiiiIiiHHiiiiHhhH',
                                     x1, y1, x2, y2, i, dx, dy, s0, s1,
                                     *bbox, flags, special, tag, slope))
    out['LINEDEFS'] = b''.join(new_lines)

    raw_sides = group['SIDEDEFS']
    sides = [struct.unpack_from('<hh8s8s8sh', raw_sides, o)
             for o in range(0, len(raw_sides) // 30 * 30, 30)]

    raw_segs = group['SEGS']
    new_segs = []
    for v1, v2, angle, linedef, side, offset in struct.iter_unpack(
            '<HHhHhh', raw_segs[:len(raw_segs) // 12 * 12]):
        _, _, flags, _, _, s0, s1 = lines[linedef]
        sidenums = (s0, s1)
        sidenum = sidenums[side]
        front = sides[sidenum][5] & 0xFFFF if sidenum != NO_INDEX else NO_INDEX
        back = NO_INDEX
        if flags & ML_TWOSIDED and sidenums[side ^ 1] != NO_INDEX:
            back = sides[sidenums[side ^ 1]][5] & 0xFFFF
        new_segs.append(struct.pack('<iiiiiIHHHH', *vtx[v1], *vtx[v2],
                                    s32(offset << 16),
                                    (angle << 16) & 0xFFFFFFFF,
                                    sidenum, linedef, front, back))
    out['SEGS'] = b''.join(new_segs)

    def texnum(name):
        name = name.split(b'\0', 1)[0].upper()
        return textures.index(name) if name in textures else 0

    out['SIDEDEFS'] = b''.join(
        struct.pack('<hhhhhh', xoff, yoff, texnum(top), texnum(bottom),
                    texnum(mid), sector)
        for xoff, yoff, top, bottom, mid, sector in sides)
    return out


def map_group(lumps, idx):
    return {n: b for n, b in lumps[idx + 1:idx + 11]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iwad', required=True, help='shareware doom1.wad')
    ap.add_argument('--into', required=True, help='GBADoom WAD to extend')
    ap.add_argument('--maps', nargs='+', default=[], metavar='ExMy')
    ap.add_argument('--add', nargs='+', default=[], metavar='LUMP')
    ap.add_argument('--drop', nargs='+', default=[], metavar='PATTERN')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()

    _, src = read_wad(args.iwad)
    ident, dst = read_wad(args.into)
    textures = texture_names(dst)

    present = [n for n, _ in dst if is_map_marker(n) and find_map(dst, n) >= 0]
    if not present:
        sys.exit('%s: no map to check the conversion against' % args.into)
    for name in present:
        si = find_map(src, name)
        if si < 0:
            sys.exit('%s: %s not found' % (args.iwad, name))
        if convert_map(map_group(src, si), textures) != \
                map_group(dst, find_map(dst, name)):
            sys.exit('%s: conversion does not reproduce %s in %s'
                     % (name, name, args.into))
        print('%s: conversion matches %s' % (name, args.into))

    for name in sorted(args.maps):
        name = name.upper()
        if name in present:
            sys.exit('%s: already in %s' % (name, args.into))
        si = find_map(src, name)
        if si < 0:
            sys.exit('%s: %s not found' % (args.iwad, name))
        group = convert_map(map_group(src, si), textures)
        # After the last map that sorts before it, else after the first.
        before = [n for n, _ in dst if is_map_marker(n) and n < name]
        at = (find_map(dst, max(before)) if before
              else find_map(dst, present[0]) - 11) + 11
        dst[at:at] = [(name, b'')] + [(n, group[n]) for n in MAP_LUMPS]
        print('%s: %d bytes' % (name, sum(len(group[n]) for n in MAP_LUMPS)))

    for pattern in args.drop:
        in_map = set()
        for i, (n, _) in enumerate(dst):
            if is_map_marker(n) and find_map(dst, n) == i:
                in_map.update(range(i, i + 11))
        hits = [i for i, (n, _) in enumerate(dst)
                if fnmatch.fnmatchcase(n, pattern.upper()) and i not in in_map]
        if not hits:
            sys.exit('--drop %s: no such lump' % pattern)
        print('drop %s: %d bytes' % (' '.join(dst[i][0] for i in hits),
                                     sum(len(dst[i][1]) for i in hits)))
        for i in reversed(hits):
            del dst[i]

    have = {n for n, _ in dst}
    for name in args.add:
        name = name.upper()
        if name in have:
            sys.exit('%s: already in %s' % (name, args.into))
        blob = next((b for n, b in src if n == name), None)
        if blob is None:
            sys.exit('%s: %s not found' % (args.iwad, name))
        dst.append((name, blob))
        print('add %s: %d bytes' % (name, len(blob)))

    # Every lump 4-byte aligned: the engine reads the converted map lumps
    # as structs, in place.
    body = bytearray()
    directory = []
    for name, blob in dst:
        while (12 + len(body)) % 4:
            body += b'\0'
        directory.append((12 + len(body) if blob else 0, len(blob), name))
        body += blob
    while (12 + len(body)) % 4:
        body += b'\0'
    infotable = 12 + len(body)
    out = ident + struct.pack('<II', len(directory), infotable) + bytes(body)
    out += b''.join(struct.pack('<II8s', pos, size, name.encode('ascii'))
                    for pos, size, name in directory)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.', exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(out)
    print('wrote %s (%d bytes, %d lumps)' % (args.out, len(out), len(directory)))


if __name__ == '__main__':
    main()
