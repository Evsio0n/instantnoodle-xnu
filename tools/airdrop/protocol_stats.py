"""Decode the returned native protocol counter snapshot using this XNU source.

Only the initial contiguous scalar fields are read; no guessed layouts after
conditional fields/arrays. Snapshots are system-wide, so use differences and
paired packets rather than assigning unrelated traffic to an AirDrop flow.
"""
import argparse,json,re,struct
from pathlib import Path

def prefix_fields(path,start,ctype):
 s=path.read_text();s=s[s.index(start)+len(start):]
 fields=[]
 for line in s.splitlines():
  line=line.strip()
  if not line or line.startswith('/*') or line.startswith('*'):continue
  m=re.match(rf'{ctype}\s+(\w+)\s*;',line)
  if not m:break
  fields.append(m[1])
 return fields

def decode(snapshot,kernel):
 wrapper=json.loads(snapshot.read_text());data=wrapper['result']['data']
 if data.get('exit_code')!=0:raise ValueError('native stats process did not exit successfully')
 rows=[json.loads(x) for x in data['output'].splitlines() if x.strip()]
 specs={'net.inet.tcp.stats':('bsd/netinet/tcp_var.h','struct  tcpstat {','u_int32_t','I'),
        'net.inet6.ip6.stats':('bsd/netinet6/ip6_var.h','struct  ip6stat {','u_quad_t','Q')}
 result={}
 for row in rows:
  if row['errno']:result[row['sysctl']]={'errno':row['errno']};continue
  file,start,ctype,fmt=specs[row['sysctl']];names=prefix_fields(kernel/file,start,ctype)
  raw=bytes.fromhex(row['hex']);size=struct.calcsize('<'+fmt)
  if len(raw)!=row['bytes'] or len(raw)<len(names)*size:raise ValueError('snapshot size mismatch')
  result[row['sysctl']]={n:struct.unpack_from('<'+fmt,raw,i*size)[0] for i,n in enumerate(names)}
 return result

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('snapshot',type=Path);p.add_argument('--before',type=Path);p.add_argument('--kernel',type=Path,default=Path('xnu-src/xnu-8796.121.2'));a=p.parse_args()
 r=decode(a.snapshot,a.kernel)
 if a.before:
  before=decode(a.before,a.kernel)
  for group,values in r.items():
   if 'errno' in values or 'errno' in before[group]:continue
   r[group]={k:v-before[group][k] for k,v in values.items() if v!=before[group][k]}
 print(json.dumps(r,indent=2))
if __name__=='__main__':main()
