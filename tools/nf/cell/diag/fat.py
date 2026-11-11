#!/usr/bin/env python3
"""fat.py IMAGE ls [DIR] | get PATH OUT: a FAT16 volume read (8.3 and long names), nothing more: for the files of the modem firmware partition."""
import struct, sys
img = open(sys.argv[1], 'rb')
b = img.read(512)
bps, spc, rsv, nfat, nroot = struct.unpack_from('<HBHBH', b, 11)
spf = struct.unpack_from('<H', b, 22)[0]
tot = struct.unpack_from('<H', b, 19)[0] or struct.unpack_from('<I', b, 32)[0]
fat_at = rsv * bps; root_at = fat_at + nfat * spf * bps; data_at = root_at + nroot * 32; csz = spc * bps
img.seek(fat_at); fat = img.read(spf * bps)
def chain(c):
    out = []
    while 2 <= c < 0xfff8:
        out.append(c); c = struct.unpack_from('<H', fat, c * 2)[0]
    return out
def read(c, size=None):
    d = b''.join((img.seek(data_at + (x - 2) * csz), img.read(csz))[1] for x in chain(c))
    return d if size is None else d[:size]
def entries(raw):
    lfn = []
    for i in range(0, len(raw), 32):
        e = raw[i:i + 32]
        if e[0] == 0: break
        if e[0] == 0xe5: lfn = []; continue
        if e[11] == 0x0f:
            lfn.insert(0, (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'replace')); continue
        name = ''.join(lfn).split('\0')[0] if lfn else (e[0:8].decode('ascii', 'replace').rstrip() + ('.' + e[8:11].decode('ascii', 'replace').rstrip() if e[8:11].strip() else ''))
        lfn = []
        if e[11] & 8 or name in ('.', '..'): continue
        yield name, bool(e[11] & 0x10), struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]
def root():
    img.seek(root_at); return img.read(nroot * 32)
def walk(path):
    raw = root()
    for part in [p for p in path.split('/') if p]:
        for name, isdir, c, size in entries(raw):
            if name.lower() == part.lower():
                if isdir: raw = read(c)
                else: return ('file', c, size)
                break
        else: raise SystemExit('not found: ' + part)
    return ('dir', raw, 0)
if sys.argv[2] == 'ls':
    kind, raw, _ = walk(sys.argv[3] if len(sys.argv) > 3 else '')
    for name, isdir, c, size in entries(raw): print('%10s %s%s' % ('' if isdir else size, name, '/' if isdir else ''))
else:
    kind, c, size = walk(sys.argv[3]); assert kind == 'file'
    open(sys.argv[4], 'wb').write(read(c, size)); print(size, 'bytes')
