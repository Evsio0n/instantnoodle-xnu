#!/usr/bin/env python3
"""qdb.py: the modem's message database (out/cell-fw/qdsp6m.qdb.txt, taken from the phone's own modem partition by pull-modem.sh and fat.py: vendor firmware, not in git; Qshrink 4: hash:mask:ssid:line:file:format) loaded as {hash: (mask, ssid, line, file, format)}.
Run as a program: counts by subsystem id for the files whose names match the words given (default: IMS's)."""
import collections, re, sys
from pathlib import Path
FW = Path(__file__).resolve().parents[4] / 'out/cell-fw'

def load():
    db = {}; mode = None
    for raw in open(FW / 'qdsp6m.qdb.txt', 'rb'):
        l = raw.decode('latin-1').rstrip('\r\n')
        if not l or l[0] == '#': continue
        m = re.fullmatch(r'<(\\?)(\w+)>', l.strip())
        if m:
            mode = None if m.group(1) else m.group(2); continue
        if mode == 'Content':
            p = l.split(':', 5)
            if len(p) == 6 and p[0].isdigit():
                db[int(p[0])] = (int(p[1]), int(p[2]), int(p[3]), p[4], p[5])
    return db

if __name__ == '__main__':
    db = load(); print(len(db), 'messages')
    words = sys.argv[1:] or ['ims', 'qipcall', 'sip', 'regm', 'qp', 'dpl', 'volte', 'rtp']
    by = collections.defaultdict(collections.Counter)
    for h, (mask, ssid, line, f, s) in db.items():
        if any(w in f.lower() for w in words): by[ssid][f] += 1
    for ssid, c in sorted(by.items(), key=lambda kv: -sum(kv[1].values()))[:40]:
        print('ssid %5d: %6d messages in %4d files, e.g. %s' % (ssid, sum(c.values()), len(c), ', '.join(f for f, _ in c.most_common(5))))
