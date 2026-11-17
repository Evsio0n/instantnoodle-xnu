"""Allow aged native mDNS copies to pass an otherwise idle data reservation.

Unicast queues and existing active-peer copies retain priority. This is a
queue/carrier scheduling change only: no DNS records, TTLs, identities or
firmware grant checks change.
"""
import json
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-home-carrier-o2c4Yj'
BASE_SHA='b49984f0a6677ea2aab2a5eb5dba4d382f7fb789a182b37d10f4ff397c701ce7'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('home-carrier control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-group-deadline-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';s=read(src+'auto_owner.inc')
    s=once(s,'static void auto_schedule_queued(void)', '#include "GroupDeadline.h"\nstatic void auto_schedule_queued(void)')
    s=once(s,'if(same || now<air.unicast_until_us) {lx_os_unlock();return;}', '''bool urgent=hk_awdl_group_yield(now,f->queued_us,f->proto==LP_MDNS,
                live_count_flights_locked(0)!=0);
            if(same || (now<air.unicast_until_us && !urgent)) {lx_os_unlock();return;}''')
    write(src+'GroupDeadline.h',(HERE/'GroupDeadline.h').read_text());write(src+'auto_owner.inc',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-group-deadline-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-group-deadline.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                aged_mdns_idle_reservation_yield_us=2000000,unicast_priority_unchanged=True,
                firmware_flights_not_preempted=True,dns_bytes_unchanged=True,hardware_verified=False)
    (OUT/'wmi-group-deadline-build.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)


if __name__=='__main__':build()
