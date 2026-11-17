"""Announce on every actual AWDL grant, including followed data ROC windows.

Clone the receive-fixed single-peer radio. The old channel-6-only discovery
condition survived the transition to dynamically followed data channels.
Nothing changes the channel scheduler, peer list, frame format or TX mode.
"""
import json, subprocess
from build_roc import BUS, HERE, OUT, read, write, once, sha

BASE='/tmp/awdl-single-JYi0iu'
BASE_SHA='4588a33d0eafe4d8ee78412c077ae4f3e007068b7e09b97c2f5f5214ccc70f81'

def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('receive-fixed radio input changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-announce-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    live=read(src+'live_transport.inc')
    live=once(live,'if ((basis==1 && live_infra_window) || basis==3) live_discovery(r,v,ch);',
        '''/* live_discovery rechecks the actual ROC/home grant and deadline.
         * A followed data-channel ROC needs its announcements too. */
        if (basis==1 || basis==3) live_discovery(r,v,ch);''')
    live=once(live,'unsigned mgmt_desc,mgmt_bound,mgmt_submitted,mgmt_phy;',
        'unsigned mgmt_desc,mgmt_bound,mgmt_submitted,mgmt_phy,mgmt_channel,mgmt_home,mgmt_follow;')
    live=once(live,'live.last_action_us=now;ES(live.mgmt_phy,subtype);',
        '''ES(live.mgmt_channel,channel_of(ch->center_freq));ES(live.mgmt_home,home);ES(live.mgmt_follow,w.follow_master);
        live.last_action_us=now;ES(live.mgmt_phy,subtype);''')
    write(src+'live_transport.inc',live)
    roc=read(src+'roc.inc')
    roc=once(roc,'ES(live.mgmt_status,status);ES(live.mgmt_fw,1);',
        '''ES(live.mgmt_status,status);ES(live.mgmt_fw,1);
        static unsigned announcements;
        unsigned count=++announcements;
        if(count<=16 || !(count%32))
            lx_tell("awdl-announce: completion=%u subtype=%u channel=%u home=%u follows=%u status=%u desc=%u\\n",
                count,EG(live.mgmt_phy),EG(live.mgmt_channel),EG(live.mgmt_home),EG(live.mgmt_follow),status,EG(live.mgmt_desc));''')
    write(src+'roc.inc',roc)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-announcement-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-data-announcement.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True)
    dest.chmod(0o600)
    receipt=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
        additional_peer=False,fixed_endpoint=False,data_channel_announcements=True,
        native_receive_fix=True,lifetime_seconds=420)
    (OUT/'data-announcement-build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))
if __name__=='__main__':build()
