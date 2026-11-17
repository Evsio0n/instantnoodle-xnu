"""Rebind the two private data symbols of a tested AWDL radio object to one snapshot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from peer_match import K, frozen_peer, symbol_bytes

BASE = K / 'out/awdl-kernel/ath11k-cell-live-flights.o'
TESTED = {
    '23aa58b59b5b6c70099553d6012a8771f5fc7b27e8f8a3e5f13054ea5c5881c0',
    'f242a5f387e4861dcbe69979019a1b383b9fc42bd9efd34c0b4a555c7d4dd1c3',
}
SIZES = {'_awdl_target_mac': 6, '_awdl_target_ip': 16}


def private_data_offsets(source, raw):
    magic, cpu, _, kind, count, command_bytes = struct.unpack_from('<6I', raw)
    if (magic, cpu, kind) != (0xfeedfacf, 0x0100000c, 1):
        raise ValueError('expected a thin arm64 Mach-O object')
    end = 32 + command_bytes
    if end > len(raw):
        raise ValueError('truncated load commands')
    cursor = 32
    ranges = []
    for _ in range(count):
        if end - cursor < 8:
            raise ValueError('truncated load command')
        cmd, length = struct.unpack_from('<II', raw, cursor)
        if length < 8 or length > end - cursor:
            raise ValueError('invalid load command length')
        if cmd == 0x19:
            if length < 72:
                raise ValueError('truncated segment')
            sections = struct.unpack_from('<I', raw, cursor + 64)[0]
            if sections > (length - 72) // 80:
                raise ValueError('truncated section table')
            for i in range(sections):
                pos = cursor + 72 + i * 80
                name, segment, address, size, offset = struct.unpack_from('<16s16sQQI', raw, pos)
                if (segment.rstrip(b'\0'), name.rstrip(b'\0')) == (b'__DATA', b'__data'):
                    if offset < end or offset + size > len(raw):
                        raise ValueError('invalid file-backed data section')
                    ranges.append((address, size, offset))
        cursor += length
    if cursor != end:
        raise ValueError('load command size mismatch')
    offsets = {}
    for line in subprocess.check_output(['nm', '-an', str(source)], text=True).splitlines():
        fields = line.split()
        if len(fields) != 3 or fields[2] not in SIZES:
            continue
        name = fields[2]
        addr = int(fields[0], 16)
        matches = [off + addr - base for base, n, off in ranges
                   if base <= addr and addr + SIZES[name] <= base + n]
        if name in offsets or len(matches) != 1:
            raise ValueError('missing or ambiguous endpoint in __DATA,__data')
        offsets[name] = matches[0]
    if set(offsets) != set(SIZES):
        raise ValueError('missing endpoint symbols')
    return offsets


def retarget(source, snapshot, out_dir, expected_snapshot):
    raw = source.read_bytes()
    source_sha = hashlib.sha256(raw).hexdigest()
    if source_sha not in TESTED:
        raise ValueError('source does not match a tested flights object')
    peer = frozen_peer(snapshot)
    snapshot_sha = hashlib.sha256(b''.join(peer)).hexdigest()
    if expected_snapshot and expected_snapshot.lower() != snapshot_sha:
        raise ValueError('snapshot digest mismatch')
    positions = private_data_offsets(source, raw)
    patches = sorted((positions[name], value) for name, value in zip(SIZES, peer))
    first, second = patches
    if first[0] + len(first[1]) > second[0]:
        raise ValueError('endpoint fields overlap')
    result = bytearray(raw)
    for pos, value in patches:
        result[pos:pos+len(value)] = value
    cursor = 0
    for pos, value in patches:
        if result[cursor:pos] != raw[cursor:pos]:
            raise ValueError('bytes changed outside the private fields')
        cursor = pos + len(value)
    if result[cursor:] != raw[cursor:] or len(result) != len(raw):
        raise ValueError('unexpected object modification')
    digest = hashlib.sha256(result).hexdigest()
    out_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    dest = out_dir / f'ath11k-flights-{digest}.o'
    fd, name = tempfile.mkstemp(dir=out_dir, prefix='.retarget-', suffix='.o')
    tmp = Path(name)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(result)
        if symbol_bytes(tmp) != peer:
            raise ValueError('symbol readback differs from snapshot')
        try:
            os.link(tmp, dest)
        except FileExistsError:
            pass
        if dest.is_symlink() or not dest.is_file() or dest.stat().st_mode & 0o777 != 0o600 or dest.read_bytes() != result:
            raise ValueError('existing output does not match private object')
    finally:
        tmp.unlink(missing_ok=True)
    return {'object': str(dest.resolve()), 'sha256': digest, 'source_sha256': source_sha,
            'snapshot_sha256': snapshot_sha}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--snapshot', required=True, type=Path)
    p.add_argument('--snapshot-sha', help='expected SHA-256 of snapshot MAC followed by IPv6')
    p.add_argument('--source', type=Path, default=BASE)
    p.add_argument('--out-dir', type=Path, default=K/'out/awdl-kernel/peer-objects')
    args = p.parse_args()
    print(json.dumps(retarget(args.source, args.snapshot, args.out_dir, args.snapshot_sha)))
