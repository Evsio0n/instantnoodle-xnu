#!/usr/bin/env python3
"""ev.py LOG [AFTER_MS]: the modem's DIAG events in a run's log ("modem: [ms] ev: N at OFF: HEX"): each event's number and its payload in hex.
An event: 16 bits (number in the low 12, bits 13-14 the payload's size: none, 1, 2 or a length byte, bit 15 a short time stamp), the time, the payload."""
import re, sys, collections
after = int(sys.argv[2]) if len(sys.argv) > 2 else 0
cur = None; packets = []
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[ *(\d+) ms\] ev: (\d+) at (\d+): ([0-9a-f]+)', l)
    if not m: continue
    if int(m.group(3)) == 0: cur = [int(m.group(1)), bytearray()]; packets.append(cur)
    if cur: cur[1] += bytes.fromhex(m.group(4))
count = collections.Counter(); rows = []
for ms, b in packets:
    if ms < after or len(b) < 3: continue
    n = b[1] | b[2] << 8; i = 3; end = min(len(b), 3 + n)
    while i + 2 <= end:
        w = b[i] | b[i + 1] << 8; i += 2
        eid = w & 0xfff; i += 2 if w & 0x8000 else 8
        k = (w >> 13) & 3
        if k == 3:
            l = b[i] if i < end else 0; i += 1
        else: l = k
        payload = bytes(b[i:i + l]); i += l
        count[eid] += 1; rows.append((ms, eid, payload))
if '--rows' in sys.argv:
    for ms, eid, p in rows: print('[%7d ms] event %4d %s' % (ms, eid, p.hex()))
else:
    for eid, c in count.most_common(80): print('%5d x event %d' % (c, eid))
