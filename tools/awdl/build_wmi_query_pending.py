"""Execute actual native SET120 browse keys through software IPv6 mDNS."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-query-social-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-query-pending-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
write(src+'mdns_pending.inc',(HERE/'mdns_pending.inc').read_text())
p=src+'live_transport.inc';s=read(p)
s=once(s,'static int live_output(', '#include "mdns_pending.inc"\nstatic int live_output(')
s=once(s,'if(!count)count=1;', '''if(!count)count=1;
    if(group) {
        unsigned pending=0;
        for(unsigned i=0;i<count;i++)
            if(!live_pending_question(q,lease,targets[i],frame,n)) {
                if(pending!=i)memcpy(targets[pending],targets[i],6);
                pending++;
            }
        count=pending;
    }''')
write(p,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-query-pending-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-query-pending.o';subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-query-pending-build.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))
