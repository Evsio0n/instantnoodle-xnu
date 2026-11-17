"""Prevent an active peer's fresh copies from starving aged native discovery.

Preserve unicast priority, complete firmware flights and minimum carrier
residency. Select an existing queued mDNS copy for a fresh canonical peer;
do not rewrite packet bytes, manufacture peers or extend DNS lifetimes.
"""
import json
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-social-owner-wczfAl'
BASE_SHA='ef423c6b9a166861e3c25476a8866d8abef0fc3e356297a32770fc0bacbce348'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('social-owner control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-discovery-fairness-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/'
    write(src+'discovery_fairness.inc',(HERE/'discovery_fairness.inc').read_text())
    owner=read(src+'auto_owner.inc')
    owner=once(owner,'#include "GroupDeadline.h"','#include "discovery_fairness.inc"')
    owner=once(owner,'unsigned choice=q==&live.tx?live_peer_choice(q,now):0;', '''unsigned choice=q==&live.tx?live_peer_choice(q,now):0;
        bool discovery_urgent=false;
        if(q==&live.groups) {
            unsigned pending=live_group_urgent_choice(q,now,live_count_flights_locked(0)!=0);
            if(pending<q->count) {choice=pending;discovery_urgent=true;}
        }''')
    owner=once(owner,'''bool urgent=hk_awdl_group_yield(now,f->queued_us,f->proto==LP_MDNS,
                live_count_flights_locked(0)!=0);
            if(same || (now<air.unicast_until_us && !urgent))''',
        '''bool urgent=discovery_urgent;
            if((same && !urgent) || (now<air.unicast_until_us && !urgent))''')
    write(src+'auto_owner.inc',owner)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-discovery-fairness-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-discovery-fairness.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                aged_mdns_us=2000000,minimum_carrier_slice_us=1048576,
                searches_behind_non_mdns_head=True,idle_active_copy_does_not_pin_aged_discovery=True,
                unicast_priority_unchanged=True,packet_bytes_unchanged=True,hardware_verified=False)
    (OUT/'wmi-discovery-fairness-build.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__=='__main__':build()
