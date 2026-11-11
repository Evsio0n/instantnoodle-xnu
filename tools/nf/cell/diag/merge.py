#!/usr/bin/env python3
"""merge.py STAMP [OUT]: one log of a followed run. xl keeps snapshots of the kernel's log ring (logs/STAMP-run.log.live.N, then .live, then the
final STAMP-run.log); the ring wraps, so each holds a window. The windows are joined at their longest overlap. Lines of the modem only."""
import re, sys
from pathlib import Path
K = Path(__file__).resolve().parents[4] / 'logs'
stamp = sys.argv[1]
out = Path(sys.argv[2]) if len(sys.argv) > 2 else K / f'cell-{stamp}.modem.log'
pat = re.compile(rb'modem: \[ *(\d+) ms\][^\r\n]*')
files = sorted(K.glob(f'{stamp}-run.log.live.*'), key=lambda p: int(p.suffix[1:]))
files += [p for p in (K / f'{stamp}-run.log.live', K / f'{stamp}-run.log') if p.exists()]
files.sort(key=lambda p: p.stat().st_mtime)
acc = []
for f in files:
    cur = [m.group(0).decode('utf-8', 'replace') for m in pat.finditer(f.read_bytes())]
    if not cur:
        continue
    if not acc:
        acc = cur; continue
    # the longest suffix of acc that is a prefix of cur
    best = 0
    first = cur[0]
    for i in range(max(0, len(acc) - len(cur)), len(acc)):
        if acc[i] == first and acc[i:] == cur[:len(acc) - i]:
            best = len(acc) - i; break
    if best == 0:
        # no overlap: keep what is newer than the last line by its time
        last = int(re.search(r'\[ *(\d+) ms\]', acc[-1]).group(1))
        cur = [l for l in cur if int(re.search(r'\[ *(\d+) ms\]', l).group(1)) > last]
        acc += ['modem: [%6d ms] (merge: a gap in the snapshots here)' % last] + cur if cur else []
    else:
        acc += cur[best:]
out.parent.mkdir(exist_ok=True)
out.write_text('\n'.join(acc) + '\n')
print(out, len(acc), 'lines from', len(files), 'files')
