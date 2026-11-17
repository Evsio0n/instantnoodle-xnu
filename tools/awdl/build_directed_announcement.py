"""Bounded directed-MIF observation on the existing single live peer.

Alternate ordinary broadcast MIFs with unicast MIFs to the currently followed
management identity, only within that peer's estimated data window AND the
existing actual firmware grant. No peer is allocated and no address is fixed.
The firmware completion is distinct from remote IP/route admission.
"""
import json, subprocess
from build_roc import BUS, OUT, read, write, once, sha

BASE='/tmp/awdl-announce-Gcfk7T'
BASE_SHA='fe7297f03b0de12c23e5f808dd778042130b906be61f6a72175948c8d927634a'

def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('data-announcement radio input changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-directed-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    live=read(src+'live_transport.inc')
    live=once(live,'mgmt_channel,mgmt_home,mgmt_follow;', 'mgmt_channel,mgmt_home,mgmt_follow,mgmt_directed;')
    marker='uint8_t *p;struct sk_buff *skb=frame_new(r,n,&p);if (!skb) return;'
    replacement='''bool directed=false;
        static unsigned mif_count;
        if(subtype==3 && (++mif_count&1) && follow_data_window(now,channel_of(ch->center_freq))) {
            lx_os_lock();
            if(air.active) {
                const struct lx_awdl_peer *peer=&air.table.peers[air.active_slot];
                if(peer->serial==air.active_serial && peer->seen_mif && now>=peer->last_rx_us &&
                   now-peer->last_rx_us<LX_AWDL_PEER_TTL_US) {
                    memcpy(data+4,peer->mac,6);directed=true;
                }
            }
            lx_os_unlock();
        }
        ES(live.mgmt_directed,directed);
        '''+marker
    live=once(live,marker,replacement)
    live=once(live,'ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|(home?0:IEEE80211_TX_CTL_TX_OFFCHAN)|IEEE80211_TX_CTL_NO_ACK;',
        'ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|(home?0:IEEE80211_TX_CTL_TX_OFFCHAN)|(directed?0:IEEE80211_TX_CTL_NO_ACK);')
    write(src+'live_transport.inc',live)
    roc=read(src+'roc.inc')
    roc=once(roc,'if(count<=16 || !(count%32))', 'if(count<=16 || EG(live.mgmt_directed) || !(count%32))')
    roc=once(roc,'home=%u follows=%u status=%u desc=%u', 'home=%u follows=%u directed=%u status=%u desc=%u')
    roc=once(roc,'EG(live.mgmt_follow),status,EG(live.mgmt_desc)', 'EG(live.mgmt_follow),EG(live.mgmt_directed),status,EG(live.mgmt_desc)')
    write(src+'roc.inc',roc)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'directed-announcement-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-directed-announcement.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True)
    dest.chmod(0o600)
    receipt=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
        additional_peer=False,fixed_endpoint=False,directed_mif_observation=True,
        data_channel_announcements=True,native_receive_fix=True,lifetime_seconds=420)
    (OUT/'directed-announcement-build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))
if __name__=='__main__':build()
