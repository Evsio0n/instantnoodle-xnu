"""Summarize TCP evidence for incoming native AirDrop connections by peer and port."""
import argparse
import collections
import json
from pathlib import Path

def summarize(paths):
    streams = collections.defaultdict(list)
    for path in paths:
        for line in path.read_text().splitlines():
            try:
                p = json.loads(line)
            except json.JSONDecodeError:
                continue
            if p.get('src') == 8770:
                key = (p['destination_ip'], p['dst'])
            elif p.get('dst') == 8770:
                key = (p['source_ip'], p['src'])
            else:
                continue
            streams[key].append(p)
    reports = []
    for (peer, port), packets in streams.items():
        uploads = [p for p in packets if p['dst'] == 8770 and p['payload']]
        responses = collections.Counter((p['seq'], p['payload']) for p in packets
                                       if p['src'] == 8770 and p['payload'])
        covered = 0
        high = None
        for lo, hi in sorted((p['seq'], p['seq'] + p['payload']) for p in uploads):
            covered += max(0, hi - max(lo, high if high is not None else lo))
            high = max(hi, high if high is not None else hi)
        reports.append(dict(peer=peer, port=port, inbound_unique_tcp_payload_bytes=covered,
                            most_repeated_response=responses.most_common(1),
                            final_flags=packets[-1]['flags'], packets=len(packets)))
    return reports

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    directory = parser.parse_args().directory
    print(json.dumps(summarize(sorted(directory.glob('packets-*.jsonl'))), indent=2))
