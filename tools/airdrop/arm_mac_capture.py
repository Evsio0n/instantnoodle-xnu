"""Publish only a fresh, live RAM phone's actual AWDL address to the bounded Mac capture.

Run after the boot controller reports native Everyone ready. Never hardcode a
previous RAM boot's identity; this writes data only, no privileged commands.
"""
import argparse,ipaddress,json,re,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('controller',type=Path);a=p.parse_args()
s=a.controller.read_text();d=re.search(r'^RESULT_DIR (\S+)',s,re.M);log=re.search(r'^LOG (\S+)',s,re.M)
if 'MULTI_RECEIVER_READY' not in s or not d or not log:raise SystemExit('controller has not verified native Everyone ready')
try:started=int(Path(d[1]).name.rsplit('-',1)[1])
except ValueError:raise SystemExit('missing controller boot timestamp')
if not 0<=time.time()-started<300:raise SystemExit('controller is stale; use a fresh RAM boot before arming')
latest=max(Path('logs').glob('*-run.log.live'),key=lambda x:x.stat().st_mtime)
if latest!=Path(log[1]):raise SystemExit('controller does not match latest live kernel')
def call(*args):
 r=subprocess.run(['./iphont',*args],capture_output=True,text=True,timeout=25,check=True);v=json.loads(r.stdout)
 if not v.get('ok'):raise RuntimeError('device query failed')
 return v['result']
state=call('state')
if not state.get('running'):raise SystemExit('phone kernel is not running')
rows=call('network')['data']['interfaces'];targets=[]
for row in rows:
 if row['name']=='awdl0' and row['family']==30:
  ip=ipaddress.IPv6Address(row['address'].split('%',1)[0])
  if ip.is_link_local:targets.append(str(ip))
if len(set(targets))!=1:raise SystemExit('no unique real AWDL link-local address')
path=Path('out/airdrop-re-20f66/current-mac-capture.ready')
if path.exists():raise SystemExit('a capture readiness file already exists; inspect it first')
tmp=path.with_suffix('.tmp');tmp.write_text(targets[0]+'\n');tmp.chmod(0o600);tmp.replace(path)
print(json.dumps({'armed':True,'controller':str(a.controller),'target_source':'native live awdl0 interface','readiness':str(path)}))
