#!/usr/bin/env python3
"""q4.py LOG...: the modem's terse messages in a run's log ("modem: [ms] q4: HASH NN ARGS", printed by the kernel after a DIAG request by hand)
with their texts from the firmware's database (qdb.py). NN: high nibble the size of an argument in bytes, low nibble their number (the other way round from what scat reads). Identity-like digit runs are masked."""
import re, sys, qdb
db = qdb.load()
def fmt(s, args):
    it = iter(args)
    def one(m):
        try: v = next(it)
        except StopIteration: return m.group(0)
        c = m.group(0)[-1]
        if c in 'di': return str(v - (1 << 32) if v >= 1 << 31 else v)
        if c in 'xXp': return '%x' % v
        if c == 'c': return chr(v & 0x7f)
        if c == 's': return '<%x>' % v
        return str(v)
    return re.sub(r'%[-+ #0]*\d*(?:\.\d+)?(?:l|ll|h|z)?[diuxXpcs]', one, s)
seen = set()
for path in sys.argv[1:]:
    for l in open(path, errors='replace'):
        m = re.search(r'\[ *(\d+) ms\] q4: ([0-9a-f]{8}) ([0-9a-f]{2}) ?([0-9a-f]*)', l)
        if not m: continue
        ms, h, ns, hexargs = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        size, n = ns >> 4, ns & 15       # (measured on this firmware: "24" with 8 bytes for a text of four %d, "12" with 2 bytes for two: the high nibble is the size)
        raw = bytes.fromhex(hexargs[:len(hexargs) // 2 * 2])
        args = [int.from_bytes(raw[i * size:(i + 1) * size], 'little') for i in range(n)] if size else []
        e = db.get(h)
        text = ('ssid %d %s:%d | %s' % (e[1], e[3], e[2], fmt(e[4], args))) if e else ('unknown hash %08x args %s' % (h, args))
        text = re.sub(r'\d{10,}', '#', text)
        text = re.sub(r'(\d{1,3}\.){3}\d{1,3}', 'A.B.C.D', text)       # (addresses the carrier assigned are not written down)
        key = (ms, text)
        if key in seen: continue
        seen.add(key); print('[%7d ms] %s' % (ms, text[:260]))
