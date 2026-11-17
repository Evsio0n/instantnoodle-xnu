"""Passive canonical-peer clock transition audit; carrier behavior unchanged."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-mdns-refresh-ePmWXk'
BASE_SHA='638658a3850cdb6b37d0f9ddc8c2b988d4c21d7ce48556271576f568ab932ccf'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:raise ValueError('refresh source changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-clock-audit-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';s=read(src+'auto_peers.inc')
    s=once(s,'int i=lx_awdl_peer_observe(&air.table,p,n,air.local,mhz,now,&air.scratch);',"""struct lx_awdl_peer before={0};
        if(air.active)before=air.table.peers[air.active_slot];
        int i=lx_awdl_peer_observe(&air.table,p,n,air.local,mhz,now,&air.scratch);
        if(i>=0 && air.active && air.table.peers[i].serial==air.active_serial && before.serial==air.active_serial) {
            const struct lx_awdl_peer *after=&air.table.peers[i];
            static unsigned clock_audit_count;
            if((memcmp(before.clock.slots,after->clock.slots,16) || memcmp(before.master,after->master,6)) && clock_audit_count++<96) {
                lx_tell("awdl-clock-change: peer=%llu kind=%u seq=%u->%u age_us=%llu heard=%u old_master=%u/%u new_master=%u/%u same_master=%u old_aw=%u new_aw=%u old_slots=",after->serial,p[h+6],before.ieee_sequence,after->ieee_sequence,now-before.last_rx_us,mhz,before.master_counter,before.master_metric,after->master_counter,after->master_metric,!memcmp(before.master,after->master,6),before.clock.base_aw,after->clock.base_aw);
                for(unsigned z=0;z<16;z++)lx_tell("%u,",before.clock.slots[z]);
                lx_tell(" new_slots=");for(unsigned z=0;z<16;z++)lx_tell("%u,",after->clock.slots[z]);lx_tell("\\n");
            }
        }""")
    write(src+'auto_peers.inc',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-clock-audit-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-clock-audit.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,clock_transition_audit=True,carrier_behavior_unchanged=True)
    (OUT/'wmi-clock-audit-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
