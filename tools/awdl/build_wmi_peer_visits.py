"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-slot-end-LCs3yL'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='94722ece59855933a573ed138f36df82a7e4331b6896c64dd2fe05d111072d16'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-peer-visits-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'roc.inc')
s=once(s,'unsigned lx_awdl_request_token(', (HERE/'discovery_slot.inc').read_text()+'\nunsigned lx_awdl_request_token(')
s=once(s,'unsigned sorted[5],n=latencies[ch].count;', 'unsigned sorted[5],n=__atomic_load_n(&latencies[ch].count,__ATOMIC_ACQUIRE);')
s=once(s,'value=latencies[ch].samples[i]', 'value=__atomic_load_n(&latencies[ch].samples[i],__ATOMIC_RELAXED)')
s=once(s,'latencies[ch].samples[pos]=us;', '__atomic_store_n(&latencies[ch].samples[pos],us,__ATOMIC_RELAXED);')
s=once(s,'if(latencies[ch].count<5)latencies[ch].count++;','if(latencies[ch].count<5)__atomic_add_fetch(&latencies[ch].count,1,__ATOMIC_RELEASE);')
s=once(s,'struct slot_target plan=slot_target_for(c,now);','bool visit=c==discovery_channel && discovery_target.serial;\n    struct slot_target plan=visit?discovery_target:slot_target_for(c,now);')
s=once(s,'        plan=slot_target_for(c,now);','        if(!visit)plan=slot_target_for(c,now);')
s=once(s,'if(plan.serial){','if(visit){\n        lx_os_lock();\n        for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++)if(air.table.peers[i].serial==plan.serial){discovery_visits[i].serial=plan.serial;discovery_visits[i].visited=receipt_begin;}\n        lx_os_unlock();\n        lx_tell("awdl-visit: serial=%llu ch=%u grant=%u\\n",plan.serial,c,!!opened);\n    }\n    if(plan.serial){')
write(src+'roc.inc',s)
s=read(src+'auto_owner.inc')
s=once(s,'unsigned social=social_visit(r,channel_of(ch->center_freq));','unsigned social=discovery_visit(r,channel_of(ch->center_freq),visit_now);\n                if(!social)social=social_visit(r,channel_of(ch->center_freq));')
s=once(s,'if(social)lx_tell(', 'discovery_channel=0;discovery_target.serial=0;\n                if(social)lx_tell(')
write(src+'auto_owner.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-peer-visits-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-peer-visits.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
