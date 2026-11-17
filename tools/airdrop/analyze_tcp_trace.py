"""Summarize captured TCP retransmission gaps without treating rapid BPF
repeat taps or ordinary pure ACKs as TCP data retransmissions."""
import json,sys
from collections import defaultdict
from pathlib import Path
flows=defaultdict(lambda:dict(events=0,observed_tcp_payload_bytes=0,data_retransmissions=[],control_retries=[],bad_checksums=0,zero_windows=0))
seen={};status=[]
for name in sys.argv[1:]:
 for line in Path(name).read_text(errors='replace').splitlines():
  try:r=json.loads(line)
  except ValueError:continue
  if 'src' not in r:status.append(r);continue
  key=tuple(sorted(((r.get('source_ip','?'),r['src']),(r.get('destination_ip','?'),r['dst']))));f=flows[key];f['events']+=1
  f['bad_checksums']+=r.get('checksum')==0;f['zero_windows']+=r.get('window')==0
  n=r.get('payload',0);f['observed_tcp_payload_bytes']+=n
  if not n and not r['flags']&7:continue
  packet=(key,r['src'],r['seq'],n,r['flags']&7);old=seen.get(packet)
  if old is not None and r['time']-old>.05:
   item=dict(gap_seconds=round(r['time']-old,3),seq=r['seq'],payload=n,flags=r['flags'])
   f['data_retransmissions' if n else 'control_retries'].append(item)
  seen[packet]=r['time']
print(json.dumps(dict(flows=[dict(endpoints=k,**v) for k,v in flows.items()],capture_status=status),indent=2))
