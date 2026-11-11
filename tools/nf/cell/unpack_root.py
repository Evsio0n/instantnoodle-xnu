#!/usr/bin/env python3
"""unpack_root.py IMAGE OUTDIR: the files of a root image (an APFS container made by tools/apfs/mkapfs.py) into a directory, to add to it and
pack it again. The Wi-Fi network file (fw/network: the owner's, name and passphrase) is not taken out: a root that needs it gets it from
fw-wifi/network, as out/mk-current-root.sh does. Read only on the image."""
import os, sys
from pathlib import Path
K = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(K / 'tools/apfs'))
import apfs
image, out = sys.argv[1], Path(sys.argv[2])
assert not out.exists(), out
c = apfs.Container(image); v = apfs.pick_volume(c, None)
n = {'dir': 0, 'file': 0, 'link': 0, 'skipped': 0}
out.mkdir(parents=True)
for p, name, fid, dt in apfs.walk(v, apfs.ROOT_DIR_INO_NUM):
    rel = p.lstrip('/')          # (walk's path is the entry's own)
    dst = out / rel
    if rel == 'fw/network':
        n['skipped'] += 1; continue
    ino = v.inode(fid)
    if dt == 4:
        dst.mkdir(parents=True, exist_ok=True); os.chmod(dst, (ino.mode & 0o7777) | 0o700 if ino else 0o755); n['dir'] += 1
    elif ino is not None and ino.is_link:
        dst.parent.mkdir(parents=True, exist_ok=True); os.symlink(ino.symlink_target_bytes().decode(), dst); n['link'] += 1
    elif ino is not None and ino.is_reg:
        dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(ino.contents()); os.chmod(dst, (ino.mode & 0o7777) | 0o600); n['file'] += 1
    else:
        n['skipped'] += 1
c.close()
print(n)
