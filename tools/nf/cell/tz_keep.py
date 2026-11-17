#!/usr/bin/env python3
"""tz_keep.py [--root] LOG... OUT: the record a tz=7 boot printed of the key it made ("tz: keep NNNN: hex" lines and
"tz: keep end: N bytes, sum X", tools/nf/cell/hk_tz.c), made a file: the next boot's root carries it as share/tz-kept
(mk_root.sh ... OUT=share/tz-kept) and that boot's key master is asked to decrypt with it. --root: the record a tz_keys=2
boot printed of the root key it had the key master seal ("tz: root ..." lines), for the next boot's share/tz-root. Several
logs are one run's pieces. The file is a wrapped test key of this phone's: it stays in out/, mode 0600, and goes into no
repository."""
import os
import re
import struct
import sys

WHAT = {'keep': 0x314b5a54, 'root': 0x31525a54}          # the record's first word: "TZK1", "TZR1"


def main():
    args = sys.argv[1:]
    what = 'root' if args[:1] == ['--root'] else 'keep'
    *logs, out = args[1:] if what == 'root' else args
    PIECE = re.compile(rb'tz: ' + what.encode() + rb' (\d{4}): ([0-9a-f]+)')
    END = re.compile(rb'tz: ' + what.encode() + rb' end: (\d+) bytes, sum ([0-9a-f]{8})')
    pieces, end = {}, None
    for path in logs:
        data = open(path, 'rb').read()
        for m in PIECE.finditer(data):
            at, piece = int(m.group(1)), bytes.fromhex(m.group(2).decode())
            if pieces.setdefault(at, piece) != piece:
                # (a line cut short where the log was copied in the middle of it: the longer one is the whole)
                if piece.startswith(pieces[at]):
                    pieces[at] = piece
                elif not pieces[at].startswith(piece):
                    sys.exit(f'tz_keep: two different pieces at {at}')
        for m in END.finditer(data):
            end = (int(m.group(1)), int(m.group(2), 16))
    if end is None or not pieces:
        sys.exit('tz_keep: no record in the log (was it a tz=7 boot without a kept key?)')
    record = b''
    for at in sorted(pieces):
        if at != len(record):
            sys.exit(f'tz_keep: a piece is missing at {len(record)}')
        record += pieces[at]
    total = 0
    for byte in record:
        total = (total * 31 + byte) & 0xffffffff
    if len(record) != end[0] or total != end[1]:
        sys.exit(f'tz_keep: {len(record)} bytes with sum {total:08x}, the boot said {end[0]} with {end[1]:08x}')
    magic, key, nonce, sealed = struct.unpack_from('<4I', record)
    if magic != WHAT[what] or len(record) != 16 + key + nonce + sealed:
        sys.exit('tz_keep: not a record')
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(record)
    print(f'{out}: {len(record)} bytes: a wrapped key of {key}, a nonce of {nonce}, {sealed} bytes it encrypted')


if __name__ == '__main__':
    main()
