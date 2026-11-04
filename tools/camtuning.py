#!/usr/bin/env python3
"""Make a camera's colour tuning, as libhackios-media's processor reads it (userland/src/camera_isp.c,
isp_load_tuning), out of the camera's tuning file of Android's camera stack
(/vendor/lib64/camera/com.qti.tuned.<name>.bin of the phone: the vendor's, kept in fw-camera/ and never in
the repository; so is what this makes of it).

usage: camtuning.py TUNED.bin OUT.isp     (camtuning.py TUNED.bin -   lists what is there only)

The tuning file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file, laid out as the sensors'
module files are (see camsensor.py): a table of symbols { id, name[32], kind, ?, parent, offset, size }
and the data they point into. Two of its modules are taken, each as Chromatix keeps it -- a tree whose
first level is the flash (led_idx_data: 0 without one, the only one taken), the next the scene's light
(aec_data: from a lux index to another, Qualcomm's measure of how dark the scene is: higher is darker),
the last the light's colour temperature (cct_data: from a CCT to another, in kelvin):

  cc13 (the colour matrix, IPE's): per light and temperature a 3x3 matrix from the sensor's colours,
       white balanced, to linear sRGB, by rows R, G, B (each row sums to 1)
  lsc34 (lens shading): per light and temperature a gain for each of R, Gr, Gb and B at each point of
       a 17 x 13 grid laid over the sensor (corners on its corners, rows from the top)

and, of its white balance (Qualcomm's AWB), the reference lights (refPtV1): ten of them for every camera
of this phone, each as the sensor sees a grey under it, red over green and blue over green -- from the
bluest daylight to a candle, in the order 10000 K, D75, D65, D50 (whose gains are the AWB's first ones,
initDataV1), cool white and TL84 fluorescent tubes, U30, A (a filament lamp), horizon, candle. Each is
given the colour temperature that the tables above are picked by for it: its own, but 3800 K for U30, a
3000 K light without the infrared a filament gives -- as a white LED is -- whose tables are TL84's (the
wide camera under a warm white LED: its colour shading measured on a white sheet is TL84's table's to
within 3 %, the 2550-2900 K one's is 12 % off in red).

The OnePlus 8's (instantnoodle; /odm/lib64/camera/com.qti.tuned.<name>.bin) are of the older "Parameter Parser
V2.0.0", as its module files are (camsensor.py): every field 64 bits wide -- a symbol { id u64, name[32], kind, parent,
offset, size }, the flash's entries { index, regions, symbol } and the lights' { lux from, lux to, two more, ranges,
symbol } as u64s (a float in the low half of its own), a temperature range { CCT from, CCT to (each a float in 8
bytes), then its arrays of floats, each padded to 8 bytes }. Its lens shading is lsc40's (the IFE's of SM8250, the
same 17 x 13 grids; lsc34's there is all 1s) and its reference lights' floats are 8 bytes apart.

OUT.isp: "RWCISP02"; the number of the matrices' light ranges and per range { lux index from, to (two
floats), the number of temperature ranges, per range { CCT from, to, the matrix (9 floats) } }; the
same for the lens shading, with per temperature range { CCT from, to, R, Gr, Gb, B grids (17 x 13 floats
each) }; the number of reference lights (0 where there are not the ten) and each { R/G, B/G, CCT }.
Everything little-endian, counts as 32-bit words.
"""
import struct
import sys

GRID = 17 * 13


V3, V2 = 'V3.0.1', 'V2.0.0'


def symbols_v2(data):
    at, count = struct.unpack_from('<II', data, 0x98)
    entries = [struct.unpack_from('<3Q', data, at + 24 * i) for i in range(count)]
    (_, table, table_size), (_, pool, _) = entries[0], entries[1]
    syms, at = {}, table
    while at + 72 <= table + table_size:
        sid = struct.unpack_from('<Q', data, at)[0]
        name = data[at + 8:at + 40].split(b'\0')[0].decode('latin1')
        _, _, ref, size = struct.unpack_from('<4q', data, at + 40)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
        at += 72
    return syms


def symbols(data):
    count = struct.unpack_from('<I', data, 0xa4)[0]
    entries = [struct.unpack_from('<3I', data, 0xa8 + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, at = {}, table
    while at + 0x38 <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        _, _, _, ref, size = struct.unpack_from('<5i', data, at + 0x24)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
        at += 0x38
    return syms


def module(syms, name, floats, version=V3, arrays=1):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the module's first tree, without the flash
    (V2: the values `arrays` arrays of floats // arrays each, every one padded to 8 bytes; and the first tree
    that does something -- the OnePlus 8 front IMX471's first two, and 22 more, are identity matrices)"""
    trees = [v for n, v in syms.values() if n == 'mod_%s_led_idx_data' % name]
    if version != V2:
        return tree(syms, trees[0], floats, version, arrays)
    for led in trees:
        out = tree(syms, led, floats, version, arrays)
        if any(not identity(v, floats, arrays) for _, _, ranges in out for _, _, v in ranges):
            return out
    return tree(syms, trees[0], floats, version, arrays)


def identity(v, floats, arrays):
    """an identity matrix (one array of 9) or all 1s (a grid's gains)"""
    if arrays == 1 and floats == 9:
        return all(abs(x - (1 if i % 4 == 0 else 0)) < 1e-6 for i, x in enumerate(v))
    return all(abs(x - 1) < 1e-6 for x in v)


def tree(syms, led, floats, version, arrays):
    if version == V2:
        _, regions, sid = next(e for e in struct.iter_unpack('<3Q', led) if e[0] == 0)
    else:
        _, regions, sid = next(e for e in struct.iter_unpack('<3I', led) if e[0] == 0)
    out = []
    for i in range(regions):
        if version == V2:
            e = syms[sid][1][48 * i:48 * (i + 1)]
            lux_from, = struct.unpack_from('<f', e, 0)
            lux_to, = struct.unpack_from('<f', e, 8)
            n, cct_sid = struct.unpack_from('<2Q', e, 32)
        else:
            lux_from, lux_to, _, _, n, cct_sid = struct.unpack_from('<4f2I', syms[sid][1], 24 * i)
        cct = syms[cct_sid][1]
        size = len(cct) // n
        ranges = []
        for j in range(n):
            if version == V2:
                e = cct[size * j:size * (j + 1)]
                per = floats // arrays
                padded = (4 * per + 7) // 8 * 8
                values = []
                for k in range(arrays):
                    values += struct.unpack_from('<%df' % per, e, 16 + padded * k)
                ranges.append((struct.unpack_from('<f', e, 0)[0], struct.unpack_from('<f', e, 8)[0], tuple(values)))
            else:
                v = struct.unpack_from('<%df' % (2 + floats), cct, size * j)
                ranges.append((v[0], v[1], v[2:]))
        out.append((lux_from, lux_to, ranges))
    return out


# the colour temperature each of the ten reference lights picks the tables by (see above)
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


def references(syms, version=V3):
    """[(R/G, B/G, CCT)] of the AWB's reference lights, or [] where they are not the ten known"""
    v = next((v for n, v in syms.values() if n == 'refPtV1'), None)
    if v is None:
        return []
    if version == V2:
        f = [struct.unpack_from('<f', v, 8 * i)[0] for i in range(len(v) // 8)]
    else:
        f = struct.unpack('<%df' % (len(v) // 4), v)
    points = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    version = next((v for v in (V3, V2) if b'Parameter Parser ' + v.encode() in data[:0x60]), None)
    if not data.startswith(b'QTI Chromatix Header') or version is None:
        sys.exit('camtuning.py: not a tuning file of the kind known here')
    if version == V2:
        syms = symbols_v2(data)
        ccm = module(syms, 'cc13', 9, V2)
        lsc = module(syms, 'lsc40', 4 * GRID, V2, 4)
    else:
        syms = symbols(data)
        ccm = module(syms, 'cc13', 9)
        lsc = module(syms, 'lsc34', 4 * GRID)
    refs = references(syms, version)

    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                if what == 'colour matrix':
                    shown = ' '.join('%.3f' % x for x in v)
                else:
                    corners = [v[c * GRID] for c in range(4)]
                    shown = 'corner gains R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(corners)
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to, shown))
    for rg, bg, cct in refs:
        print('reference light R/G %.3f B/G %.3f, tables of %.0f K' % (rg, bg, cct))
    if sys.argv[2] == '-':
        return

    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    open(sys.argv[2], 'wb').write(out)


main()
