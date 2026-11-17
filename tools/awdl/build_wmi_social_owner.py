"""Give social management windows to their actual active secondary VDEV."""
import json
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-tlv-order-EtYl5W'
BASE_SHA='170efe0289c37d0fc7b22dbe6ccd8a58af58d22dafb83617cec07eefb9a406ef'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('TLV-order control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-social-owner-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/'
    owner=read(src+'auto_owner.inc')
    owner=once(owner,'static bool auto_window(', (HERE/'social_vif.inc').read_text()+'\nstatic bool auto_window(')
    owner=once(owner,'memset(&bw,0,sizeof(bw));bw.radio=r;bw.vif=r->vif;',
        'memset(&bw,0,sizeof(bw));bw.radio=r;bw.vif=auto_social_vif(r,v);')
    owner=once(owner,'air.listen_mhz=ch->center_freq;live_infra_window=1;',
        'air.listen_mhz=ch->center_freq;live_infra_window=(bw.vif==r->vif);')
    write(src+'auto_owner.inc',owner)
    live=read(src+'live_transport.inc')
    live=once(live,'unsigned mgmt_desc,mgmt_bound,mgmt_submitted,mgmt_phy,',
        'unsigned mgmt_desc,mgmt_bound,mgmt_submitted,mgmt_phy,mgmt_roc_vdev,')
    live=once(live,'ES(live.mgmt_desc,desc);ES(live.mgmt_bound,1);return 1;',
        '''/* Recheck on the issuing worker, not just at CPU enqueue. */
    if(lx_awdl_data_foreign(hw) && bw.control_id!=id)return -ECANCELED;
    ES(live.mgmt_roc_vdev,bw.control_id);
    ES(live.mgmt_desc,desc);ES(live.mgmt_bound,1);return 1;''')
    write(src+'live_transport.inc',live)
    roc=read(src+'roc.inc')
    roc=once(roc,'directed=%u status=%u desc=%u\\n",',
        'directed=%u status=%u desc=%u grant_vdev=%u latency_us=%llu\\n",')
    roc=once(roc,'EG(live.mgmt_directed),status,EG(live.mgmt_desc));',
        'EG(live.mgmt_directed),status,EG(live.mgmt_desc),EG(live.mgmt_roc_vdev),lx_os_nanoseconds()/1000ull-EG(live.last_action_us));')
    write(src+'roc.inc',roc)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-social-owner-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-social-owner.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                secondary_social_roc_when_carrier_up=True,passive_primary_fallback=True,
                deferred_management_owner_recheck=True,management_latency_receipts=True,hardware_verified=False)
    (OUT/'wmi-social-owner-build.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)


if __name__=='__main__':build()
