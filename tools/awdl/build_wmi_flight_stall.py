"""Execute actual native SET120 browse keys through software IPv6 mDNS."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-query-pending-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-flight-stall-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
write(src+'flight_stall.inc',(HERE/'flight_stall.inc').read_text())
p=src+'auto_owner.inc';s=read(p)
s=once(s,'static bool auto_data_service(', '#include "flight_stall.inc"\nstatic bool auto_data_service(')
s=once(s,'\t\tauto_schedule_queued();\n\t\tif(auto_follow_channel()) break;',
    '\t\tif(live_carrier_stalled(lx_os_nanoseconds()/1000ull))break;\n\t\tauto_schedule_queued();\n\t\tif(auto_follow_channel()) break;')
write(p,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-flight-stall-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-flight-stall.o';subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-flight-stall-build.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))
