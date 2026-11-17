"""Consume a valid handoff before reconsidering the queue on an idle carrier."""
import json
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-discovery-fairness-cdlswW'
BASE_SHA='ae7981c43ba201ff85e1b8e0b15f6ff155a18dbc581595fd5a5e13b67f5c73e6'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('discovery fairness control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-handoff-commit-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/'
    owner=read(src+'auto_owner.inc')
    helper=(HERE/'handoff_pending.inc').read_text()
    owner=once(owner,'static bool auto_window(',helper+'\nstatic bool auto_window(')
    owner=once(owner,'\t\tauto_schedule_queued();\n\t\tif(auto_pick()) {',
                     '\t\tif(!auto_pending_handoff())auto_schedule_queued();\n\t\tif(auto_pick()) {')
    write(src+'auto_owner.inc',owner)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-handoff-commit-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-handoff-commit.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                aged_mdns_us=2000000,minimum_carrier_slice_us=1048576,
                preserve_valid_handoff=True,searches_behind_non_mdns_head=True,idle_active_copy_does_not_pin_aged_discovery=True,
                unicast_priority_unchanged=True,packet_bytes_unchanged=True,hardware_verified=False)
    (OUT/'wmi-handoff-commit-build.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__=='__main__':build()
