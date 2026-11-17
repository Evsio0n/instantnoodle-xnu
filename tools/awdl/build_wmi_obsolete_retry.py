"""Do not classify an intentionally cancelled rendezvous as channel failure."""
import json
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-visit-reserved-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-obsolete-retry-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
p=stage+'/xnu-deps/build/ath11k/src/ath11k/mac.c';s=read(p)
a=s.index('static int ath11k_mac_op_remain_on_channel(')
b=s.index('\nstatic int ath11k_mac_station_add',a)
part=s[a:b]
part=once(part,'\tint ret;', '\tint ret;\n    bool obsolete_window=false;')
part=once(part,'                ret=0;break;', '                obsolete_window=true;ret=0;break;')
part=once(part,'\t\tret = -ETIMEDOUT;', '\t\tret = obsolete_window ? -EAGAIN : -ETIMEDOUT;')
s=s[:a]+part+s[b:];write(p,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/ath11k_mac.o'],check=True)
with (OUT/'wmi-obsolete-retry-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-obsolete-retry.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-obsolete-retry-build.json').write_text(json.dumps(receipt,indent=2))
print(json.dumps(receipt,indent=2))
