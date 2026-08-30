#!/usr/bin/env python3
"""
Merge DS* sound lumps from the shareware IWAD into the trimmed E1M1 WAD.

  python merge_sfx.py --iwad doom1.wad --into ../wad/doom1_e1m1.wad \\
                      --out ../wad/doom1_e1m1_sfx.wad
"""
import argparse
import os
import struct
import sys


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
        name = data[off + 8:off + 16]
        lumps.append((name, pos, size))
    return data[:4], lumps, data


def lump_name(n):
    return n.split(b'\0', 1)[0].decode('ascii', 'replace').upper()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iwad', required=True, help='shareware doom1.wad')
    ap.add_argument('--into', required=True, help='trimmed E1M1 wad')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()

    _, src_lumps, src_data = read_wad(args.iwad)
    ident, dst_lumps, dst_data = read_wad(args.into)

    have = {lump_name(n) for n, _, _ in dst_lumps}
    extra = []
    for name, pos, size in src_lumps:
        ln = lump_name(name)
        if ln.startswith('DS') and ln not in have and size > 0:
            extra.append((name, src_data[pos:pos + size]))

    if not extra:
        sys.exit('no DS* lumps found in %s' % args.iwad)

    infotable_old = struct.unpack_from('<I', dst_data, 8)[0]
    body = bytearray(dst_data[12:infotable_old])
    shift = 12  # filepos in dst_lumps is absolute in the original file

    new_dir = []
    for name, pos, size in dst_lumps:
        new_dir.append((name, pos, size))

    payload = bytearray(dst_data[:infotable_old])
    for name, blob in extra:
        pos = len(payload)
        payload += blob
        new_dir.append((name, pos, len(blob)))

    while len(payload) % 4:
        payload += b'\0'
    infotable = len(payload)
    for name, pos, size in new_dir:
        payload += struct.pack('<II8s', pos, size, name)

    out = ident + struct.pack('<II', len(new_dir), infotable) + bytes(payload[12:])

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.', exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(out)
    print('wrote %s (%d bytes, %d lumps, +%d DS*)'
          % (args.out, len(out), len(new_dir), len(extra)))


if __name__ == '__main__':
    main()
