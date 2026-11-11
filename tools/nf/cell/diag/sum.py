#!/usr/bin/env python3
"""sum.py MERGED: what a cellular run did, in a page: bring-up, SIM, registration, data, EFS, the files asked over TFTP, the DIAG messages by
source file (counts) and the ones that look like faults. Identities (IMSI, IMEI, ICCID, numbers) are not printed: digit runs of 10+ are masked."""
import re, sys, collections
lines = open(sys.argv[1], errors='replace').read().split('\n')
mask = lambda s: re.sub(r"(?i)(lat|lon|alt)[:=] ?-?[0-9.]+", r"\1:#", re.sub(r"\d{10,}", lambda m: "#" * len(m.group(0)), s))
def t(l):
    m = re.search(r'\[ *(\d+) ms\]', l); return int(m.group(1)) if m else -1
body = lambda l: l.split('] ', 1)[1] if '] ' in l else l
keep = re.compile(r'\] (bringup|dms|nas|uim|sim|efs|monitor|service|data|wds|sms|voice|pdc|mission|mhi|net|ims|call|after|clock):')
print('== state lines (repeats folded)')
prev = None; n = 0
for l in lines:
    if not keep.search(l) or 'loopback' in l.lower(): continue
    b = body(l)
    if b == prev: n += 1; continue
    if n: print(f'      (x{n + 1})')
    print(mask(l[7:])[:300]); prev, n = b, 0
if n: print(f'      (x{n + 1})')
print('== TFTP files')
files = collections.OrderedDict()
for l in lines:
    m = re.search(r'tftp: (read|WRITE) "([^"]+)"', l)
    if m: files.setdefault((m.group(1), m.group(2)), []).append(t(l))
    m = re.search(r'tftp: "([^"]+)": (.*)', l)
    if m: files.setdefault(('note', m.group(1) + ': ' + m.group(2)[:80]), []).append(t(l))
for (k, f), ts in files.items(): print(f'  {k:5} x{len(ts):<3} first {ts[0] / 1000:6.1f}s  {f}')
print('== DIAG messages by source file')
c = collections.Counter(); bad = []
for l in lines:
    m = re.search(r' f3: (ss\d+) (\S+?):(\d+) \|(.*)', l)
    if not m: continue
    c[m.group(2)] += 1
    if re.search(r'(?i)fatal|assert|err(or)?[ :=]|fail|invalid|denied|reject|not allowed|no service|limited|barred|illegal|unavail', m.group(4)): bad.append(l)
print('  ' + ', '.join(f'{k} {v}' for k, v in c.most_common(60)))
print(f'== DIAG lines that read like faults ({len(bad)}; unique texts, first 80)')
seen = set(); k = 0
for l in bad:
    b = re.sub(r'\d+', 'N', body(l))
    if b in seen: continue
    seen.add(b); k += 1
    if k <= 80: print('  ' + mask(l[7:])[:260])
