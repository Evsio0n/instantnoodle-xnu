"""Execute actual native SET120 browse keys through software IPv6 mDNS."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-flight-stall-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-queue-age-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
write(src+'queue_age.inc',(HERE/'queue_age.inc').read_text())
p=src+'auto_maintenance.inc';s=read(p)
s=once(s,'static void auto_maintain(void)', '#include "queue_age.inc"\nstatic void auto_maintain(void)')
s=once(s,'\tair.last_maint_us=now;', '\tair.last_maint_us=now;\n    live_queue_age_tick(now);')
write(p,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-queue-age-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-queue-age.o';subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-queue-age-build.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))
