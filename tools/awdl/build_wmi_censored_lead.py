"""Include ungranted waits as latency lower bounds instead of survivor bias."""
import json
import subprocess
from build_roc import BUS,OUT,HERE,read,write,once,sha
parent=json.loads((OUT/'wmi-obsolete-retry-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-censored-lead-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
p=src+'roc.inc';s=read(p)
s=once(s,'return sorted[n/2];', '''/* Five recent observations, including censored lower bounds. The
     * upper sample protects the rendezvous from the firmware's slow mode;
     * five subsequent fast grants naturally age that protection out. */
    return sorted[n-1];''')
s=once(s,'__atomic_store_n(&latencies[ch].samples[pos],us,__ATOMIC_RELAXED);', '__atomic_store_n(&latencies[ch].samples[pos],us>250000?250000:us,__ATOMIC_RELAXED);')
anchor='    if(visit){\n        lx_os_lock();'
s=once(s,anchor,'''    if(c<178 && !opened && rc==-EAGAIN && ROC_GET(r->done)){
        /* Cancellation is right-censored evidence: no grant had arrived
         * by this deadline. It is not a measured latency or a bad channel.
         * Give the next attempt that observed lower bound plus a guard. */
        uint64_t latest=ROC_GET(r->latest_grant_us);
        if(latest>receipt_begin){
            unsigned lower=(unsigned)(latest-receipt_begin);
            slot_sample(c,lower+8192);
            lx_tell("awdl-slot-lag: ch=%u censored=1 lower_us=%u next_lead_us=%u\\n",
                c,lower,slot_lead(c));
        }
    }
'''+anchor)
write(p,s)
policy=(HERE/'roc_deadline.h').read_text()
policy=once(policy,'uint64_t cap=UINT64_MAX-now<200000?UINT64_MAX:now+200000;', '''uint64_t budget=planned_end>now?500000:200000;
    uint64_t cap=UINT64_MAX-now<budget?UINT64_MAX:now+budget;''')
write(src+'roc_deadline.h',policy)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-censored-lead-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-censored-lead.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-censored-lead-build.json').write_text(json.dumps(receipt,indent=2))
print(json.dumps(receipt,indent=2))
