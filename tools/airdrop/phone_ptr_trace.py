"""Decode AirDrop PTR TTLs from native mDNS packets captured on the phone."""
import argparse
import json
import struct

def name(data, pos):
    labels = []
    end = None
    visited = set()
    while pos < len(data) and pos not in visited and len(visited) < 128:
        visited.add(pos)
        size = data[pos]
        if size == 0:
            return b'.'.join(labels).lower(), end or pos+1
        if size & 0xc0 == 0xc0 and pos+1 < len(data):
            end = end or pos+2
            pos = ((size & 63)<<8) | data[pos+1]
        elif size <= 63 and pos+1+size <= len(data):
            labels.append(data[pos+1:pos+1+size])
            pos += 1+size
        else:
            break
    raise ValueError('invalid DNS name')

def records(data):
    _, flags, qd, an, ns, ar = struct.unpack_from('!6H', data)
    if not flags & 0x8000:
        return
    pos = 12
    for _ in range(qd):
        _, pos = name(data,pos)
        pos += 4
    for _ in range(an+ns+ar):
        owner, pos = name(data,pos)
        kind, _, ttl, size = struct.unpack_from('!HHIH',data,pos)
        pos += 10
        if pos+size > len(data):
            raise ValueError('invalid resource length')
        if kind == 12 and owner == b'_airdrop._tcp.local':
            target, end = name(data,pos)
            if end == pos+size:
                yield target.decode('ascii','replace'),ttl
        pos += size

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', type=argparse.FileType('r'), nargs='+')
    args = parser.parse_args()
    for capture in args.captures:
        for line in capture:
            try:
                packet = json.loads(line)
                if 'dns_hex' not in packet:
                    continue
                for target, ttl in records(bytes.fromhex(packet['dns_hex'])):
                    print(json.dumps(dict(time=packet['time'], source=packet['mdns_source'],
                                          target=target, ttl=ttl)))
            except (ValueError, struct.error):
                continue

if __name__=='__main__':
    main()
