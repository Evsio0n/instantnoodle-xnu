"""Reserve the first management frame for the selected peer's live window."""
import json
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-grant-deadline-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-visit-reserved-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
p=src+'live_transport.inc';s=read(p)
s=once(s,'static uint64_t visit_serial, last_visit_sent;', 'static uint64_t visit_serial, last_visit_sent, visit_until;')
s=once(s,' || (live.last_action_us && now-live.last_action_us<110*1024)', '')
anchor='\tunsigned n=(unsigned)lx_awdl_discovery_action('
s=once(s,anchor,'''    /* A grant can open before the selected peer's guarded interval.
     * Do not spend this visit on a broadcast and then throttle its MIF
     * for longer than the peer's entire 49 ms rendezvous. Once the actual
     * peer window is confirmed, its one directed MIF owns the first slot.
     * Stale/missed visits revert to the regular broadcast pacing. */
    bool awaiting_visit=visit_serial && visit_epoch!=last_visit_sent &&
        channel==visit_channel && now<visit_until;
    if(awaiting_visit && !visit_mif)return;
    if(!visit_mif && live.last_action_us && now-live.last_action_us<110*1024)return;
'''+anchor)
write(p,s)
p=src+'roc.inc';s=read(p)
s=once(s,'visit_serial=best.serial;visit_channel=winner;++visit_epoch;',
       'visit_serial=best.serial;visit_channel=winner;visit_until=best.end;++visit_epoch;')
write(p,s)
p=src+'auto_owner.inc';s=read(p)
s=once(s,'visit_serial=0;visit_channel=0;', 'visit_serial=0;visit_channel=0;visit_until=0;')
write(p,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-visit-reserved-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-visit-reserved.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-visit-reserved-build.json').write_text(json.dumps(receipt,indent=2))
print(json.dumps(receipt,indent=2))
