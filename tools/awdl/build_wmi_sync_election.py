"""Elect the announcement synchronization peer independently of the data destination."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-social-visit-3q6nNg'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='63a9308047c0965c23f5b730a9ce863c76c1cf1546aa9418b09727dda5bcc42d'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-sync-election-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'auto_peers.inc')
s=once(s,'static void follow_announcement(', (HERE/'announcement_sync.inc').read_text()+'\nstatic void follow_announcement(')
a=s.index('    if(!air.active)return;',s.index('static void follow_announcement('))
b=s.index('    /* Join',a)
s=s[:a]+'''    lx_os_lock();
    int sync_slot=announcement_sync_locked(w->now_us);
    if(sync_slot<0){lx_os_unlock();return;}
    struct lx_awdl_peer p=air.table.peers[sync_slot];
    static uint64_t sync_serial;
    if(sync_serial!=p.serial){
        lx_tell("awdl-election: sync_peer=%llu data_peer=%llu distance=%u counter=%u metric=%u\\n",
            p.serial,air.active_serial,p.distance,p.master_counter,p.master_metric);
        sync_serial=p.serial;
    }
    lx_os_unlock();
'''+s[b:]
write(src+'auto_peers.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-sync-election-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-sync-election.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
