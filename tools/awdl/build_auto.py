"""Build address-free AWDL discovery and radio-driven peer replacement privately."""
import hashlib,json,subprocess
from pathlib import Path
from build_roc import BUS,HERE,ROOT,OUT,read,write,once,sha
import direct_build

def replace_function(s,name,new):
    at=s.index(name+'('); start=s.rfind('static ',0,at)
    brace=s.index('{',at); depth=1;end=brace+1
    while depth:
        if s[end]=='{':depth+=1
        if s[end]=='}':depth-=1
        end+=1
    return s[:start]+new+'\n'+s[end:]

def build():
    pin=json.loads((OUT/'live-flights-build.json').read_text())
    assert pin['sha256']=='23aa58b59b5b6c70099553d6012a8771f5fc7b27e8f8a3e5f13054ea5c5881c0'
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-auto-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    for name in ('peer_table.h','peer_windows.h','awdl_rx_action.h','awdl_discovery.h','wire_checksum.h','auto_peers.inc','auto_owner.inc','auto_maintenance.inc','auto_direct.inc','ifnet_transport.h','lx_awdl_transport.h'):
        source=(HERE/name).read_text()
        if name=='awdl_rx_action.h':
            source=source.replace('../../wifi/lx/src/lx_awdl_rx.h','lx_awdl_rx.h')
        write(src+name,source)
    peers=read(src+'auto_peers.inc').replace('static uint8_t active_peer_mac[6],active_peer_ip[16];','')
    write(src+'auto_peers.inc',peers)
    data=read(src+'data.inc')
    data=once(data,'#include "echo_target_private.h"','''static uint8_t active_peer_mac[6],active_peer_ip[16];
static const uint8_t awdl_target_mac[6]={0,0x25,0,0xff,0x94,0x73};
static const uint8_t awdl_target_ip[16]={0};''')
    marker='static void lx_awdl_data_poll(struct ieee80211_hw *);'
    data=once(data,marker,'''#include "auto_peers.inc"
static void auto_maintain(void);
static bool auto_data_service(struct lx_radio *,struct ieee80211_vif *,struct ieee80211_sta *,struct linuxkpi_ieee80211_channel *);
'''+marker)
    # The single worker changes channels by posting a new job, not by spawning.
    data=once(data,'unsigned submitted,retired;','unsigned submitted,retired,channel;')
    data=once(data,'bool ok=lx_awdl_listen_channel(r,v,6);','bool ok=lx_awdl_listen_channel(r,v,window_owner.channel);')
    data=once(data,'window_owner.radio=r;window_owner.vif=bw.vif;','window_owner.radio=r;window_owner.vif=bw.vif;window_owner.channel=channel_of(bw.own_mhz);')
    data=data.replace('r->target_mhz==2437','r->target_mhz==bw.own_mhz').replace('EG(bw.own_mhz)!=2437','EG(bw.own_mhz)!=air.mhz')
    data=data.replace('freq==2437?1:0','freq==bw.own_mhz?1:0')
    data=data.replace('ch->center_freq!=2437','ch->center_freq!=air.mhz')
    data=data.replace('linuxkpi_ieee80211_get_channel(w,2437)','linuxkpi_ieee80211_get_channel(w,air.mhz)')
    data=data.replace('if (radio->persistent_awdl) live_withdraw();','/* The outer automatic generation owns the interface registration. */')
    data=data.replace('memcmp(p+6,awdl_target_mac,6)','!auto_route(p+6,NULL)').replace('memcmp(p+10,awdl_target_mac,6)','!auto_route(p+10,NULL)')
    data=once(data,'static bool lx_awdl_nd_input(const uint8_t *da,','static bool lx_awdl_nd_input(const uint8_t *source,const uint8_t *da,')
    data=once(data,'if (EG(live.armed)) return live_rx_ipv6(da,ip,bytes);','if (EG(live.armed)) return live_rx_ipv6(source,da,ip,bytes);')
    data=once(data,'return lx_awdl_nd_input(da,ip,bytes,format);','return lx_awdl_nd_input((s->flag&RX_FLAG_8023)?p+6:p+10,da,ip,bytes,format);')
    # An old carrier is completely retired before this owner selects another.
    data=data.replace('if (!live_service(radio,vif,peer,ch)) return false;','if (!auto_data_service(radio,vif,peer,ch)) return false;')
    data=once(data,'static struct ieee80211_sta *lookup_peer;','static struct ieee80211_sta *lookup_peer,*lookup_alt_peer;')
    data=once(data,'return lookup_peer && !memcmp(lookup_peer->addr,addr,6)?lookup_peer:NULL;',
        'if(lookup_peer && !memcmp(lookup_peer->addr,addr,6)) return lookup_peer;\n\treturn lookup_alt_peer && !memcmp(lookup_alt_peer->addr,addr,6)?lookup_alt_peer:NULL;')
    data=once(data,'void lx_awdl_echo_config_event(struct ieee80211_hw *hw,unsigned vdev,const unsigned char *peer)\n{',
        'void lx_awdl_echo_config_event(struct ieee80211_hw *hw,unsigned vdev,const unsigned char *peer)\n{\n\tauto_direct_config_event(hw,vdev,peer);')
    # Use only capabilities actually advertised by this live MIF, bounded
    # by the real local wiphy and this carrier's single-stream HT20 mode.
    data=once(data,'peer->deflink.sta=peer; peer->link[0]=&peer->deflink;', '''peer->deflink.sta=peer; peer->link[0]=&peer->deflink;
    if(air.active) {
        lx_os_lock();struct lx_awdl_peer observed=air.table.peers[air.active_slot];lx_os_unlock();
        const struct ieee80211_sta_ht_cap *local=&w->bands[ch->band]->ht_cap;
        if(observed.serial==air.active_serial && observed.has_ht && local->ht_supported &&
           (observed.ht_mcs0 & local->mcs.rx_mask[0])) {
            struct ieee80211_sta_ht_cap *cap=&peer->deflink.ht_cap;
            cap->ht_supported=true;cap->cap=observed.ht_capabilities & local->cap;
            cap->cap &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
            cap->ampdu_factor=min_t(unsigned,observed.ht_ampdu&3,local->ampdu_factor);
            cap->ampdu_density=(observed.ht_ampdu>>2)&7;
            cap->mcs.rx_mask[0]=observed.ht_mcs0 & local->mcs.rx_mask[0];
            cap->mcs.tx_params=IEEE80211_HT_MCS_TX_DEFINED;
            peer->deflink.rx_nss=1;peer->deflink.bandwidth=IEEE80211_STA_RX_BW_20;peer->wme=true;
        }
    }
    lx_tell("awdl-auto: rate_context ht=%u mcs0_nonzero=%u bandwidth_mhz=20 streams=1 source=live_mif\\n",
        peer->deflink.ht_cap.ht_supported,!!peer->deflink.ht_cap.mcs.rx_mask[0]);''')
    # Same-channel traffic needs no fictional ROC. A recent ordinary home
    # frame plus an idle scan and matching started chanctx are its own basis.
    anchor='if (!EG(bw.armed) || !EG(echo.active) || echo.hw!=hw || !EG(bw.ready) || !EG(bw.started)) return 0;'
    data=once(data,anchor,'''struct lx_radio *radio=radio_of(hw);
    uint64_t home_now=lx_os_nanoseconds()/1000ull,home_stamp=EG(air.home_us);
    if(EG(echo.active) && echo.hw==hw && EG(echo.start) && !EG(echo.stop) && !EG(echo.start_status) &&
       radio->joined && radio->channel && radio->channel->def.chan &&
       echo.ctx && echo.ctx->def.chan && radio->channel->def.chan->center_freq==echo.ctx->def.chan->center_freq &&
       lx_awdl_scan_state(hw,EG(radio->roc.request_seq),echo.vdev)==LX_AWDL_SCAN_IDLE &&
       home_stamp && home_now>=home_stamp && home_now-home_stamp<250000) return 3;
    '''+anchor)
    write(src+'data.inc',data)

    live=read(src+'live_transport.inc')
    live=once(live,'now-live_first_us>=165000000ull','now-live_first_us>=420000000ull')
    live=once(live,'#define LIVE_DEPTH 8u','#define LIVE_DEPTH 32u')
    live=once(live,'uint8_t bytes[14+LIVE_MTU];','uint8_t link_target[6],bytes[14+LIVE_MTU];')
    live=once(live,'p->lease=lease;p->n=n;memcpy(p->bytes,frame,n);queue->count++;',
        'memset(p,0,sizeof(*p));p->lease=lease;p->n=n;memcpy(p->bytes,frame,n);queue->count++;')
    live=once(live,'return !memcmp(a,awdl_target_mac,6);','return auto_route(a,NULL);')
    live=once(live,'(!live_mac_peer(frame)||memcmp(frame+38,awdl_target_ip,16))','!auto_route(frame,frame+38)')
    live=replace_function(live,'live_output',(HERE/'auto_output.inc').read_text())
    live=once(live,'static bool live_rx_ipv6(const uint8_t *da,','static bool live_rx_ipv6(const uint8_t *source,const uint8_t *da,')
    live=once(live,'if (!memcmp(ip+8,awdl_target_ip,16)) ES(live_once_rx_ip,1);','')
    live=once(live,'memcpy(pkt->bytes+6,awdl_target_mac,6);','memcpy(pkt->bytes+6,source,6);')
    # Update the observed IP outside the FIFO mutex.
    live=once(live,'/* Caller has already validated the provisioned radio peer and BSSID. */','if (!auto_note_ip(source,ip,n)) return false;')
    live=replace_function(live,'live_presence','static void live_presence(void) { auto_maintain(); }')
    # The automatic owner performs registration and final withdrawal once.
    live=replace_function(live,'live_service','static bool live_service(struct lx_radio *r,struct ieee80211_vif *v,struct ieee80211_sta *p,struct linuxkpi_ieee80211_channel *ch) {return auto_data_service(r,v,p,ch);}')
    live=once(live,'if (!basis || !EG(live.accept)) return;','if (!basis || !EG(live.accept) || !live.peer || air.prepeer || !auto_active_fresh()) return;')
    live=once(live,'q=lane?&live.groups:&live.tx;', 'q=lane?&live.groups:&live.tx;') if False else live
    marker='if (!q->count || (lane&&!live_multicast_due(live.anchor_us,now))) continue;'
    live=once(live,marker,marker+'''
        unsigned offset=0;
        if(!lane && !auto_lane_target(q->slot[q->first].bytes)) continue;
        if(lane) {
            for(;offset<q->count;offset++)
                if(!memcmp(q->slot[(q->first+offset)%LIVE_DEPTH].link_target,active_peer_mac,6)) break;
            if(offset==q->count) continue;
        }''')
    live=once(live,'live.scratch=q->slot[q->first];live_pop(q);', '''live.scratch=q->slot[(q->first+offset)%LIVE_DEPTH];
            if(!offset)live_pop(q);
            else {
                for(unsigned move=offset;move+1<q->count;move++)
                    q->slot[(q->first+move)%LIVE_DEPTH]=q->slot[(q->first+move+1)%LIVE_DEPTH];
                q->count--;
            }
            if(!lane)air.unicast_until_us=now+10000000ull;''')
    # Native IP multicast may be carried as an acknowledged 802.11 unicast
    # to a currently learned peer, as on normal multicast-to-unicast WLANs.
    # The IPv6 multicast destination and the native packet remain unchanged.
    live=once(live,'memcpy(frame+10,v->addr,6);memcpy(frame+16,awdl_network,6);',
        'if(f->group) memcpy(frame+4,live.scratch.link_target,6);\n\tmemcpy(frame+10,v->addr,6);memcpy(frame+16,awdl_network,6);')
    live=once(live,'(f->group?IEEE80211_TX_CTL_NO_ACK:0)','((frame[4]&1)?IEEE80211_TX_CTL_NO_ACK:0)')
    live=once(live,'else live.tqm_ack+=!!f->acked;','live.tqm_ack+=!!f->acked;')
    live=once(live,'!!(snapshot.acked&&!snapshot.group)','!!snapshot.acked')
    live=once(live,'if (!f->wbm || (!f->group && !f->acked)) live.dropped++;',
        'if (!f->wbm || f->source!=0 || f->reason!=0) live.dropped++;')
    live=once(live,'basis=%u bound=%u ring=%u desc=%u queue_us=%llu\\n",',
        'basis=%u bound=%u ring=%u desc=%u queue_us=%llu wire_unicast=%u peer_context=%u\\n",')
    live=once(live,'f->tag,live_proto_name(f->proto),f->group,basis,EG(f->bound),EG(f->ring),EG(f->desc),now-f->queued_us);',
        'f->tag,live_proto_name(f->proto),f->group,basis,EG(f->bound),EG(f->ring),EG(f->desc),now-f->queued_us,!(frame[4]&1),!air.prepeer);')
    live=live.replace('freq!=2437','freq!=air.listen_mhz')
    live=once(live,'if (basis==1 && live_infra_window) live_discovery(r,v,ch);','if (basis==1 && live_infra_window && ch->center_freq==2437) live_discovery(r,v,ch);')
    # Return actual known preference; an unknown remote master stays zero.
    live=once(live,'out->preferred=6;','out->preferred=air.active?air.channel:6;')
    write(src+'live_transport.inc',live)

    roc=read(src+'roc.inc')
    roc=once(roc,'struct lx_awdl_roc_state *r=&radio_of(hw)->roc;\n\tif (!ROC_GET(r->active) || !ROC_GET(r->foreign_open)) return;',
        'auto_observe(hw,frequency,frame,length);\n\tstruct lx_awdl_roc_state *r=&radio_of(hw)->roc;\n\tif (!ROC_GET(r->active) || !ROC_GET(r->foreign_open)) return;')
    roc=once(roc,'unsigned target = c == 6 ? 2437 : (c == 44 ? 5220 : 5745);','unsigned target = auto_freq(c);')
    roc=once(roc,'static void live_thread(void *context)','''#include "auto_owner.inc"
static void live_thread(void *context)''')
    roc=once(roc,'\tlx_awdl_listen_owner(r);\n\tlive_withdraw();','\tauto_owner(r);\n\tlive_withdraw();')
    write(src+'roc.inc',roc)
    # A data context may use a real heard/scheduled peer channel, not a baked6.
    macpath=work+'src/ath11k/mac.c';mac=read(macpath)
    mac=once(mac,'#include "wmi.h"','#include "wmi.h"\nextern void lx_tell(const char *,...);')
    mac=mac.replace('ch.chan->center_freq!=2437','ch.chan->center_freq!=lx_awdl_auto_frequency(hw,vif)')
    mac=once(mac,'ath11k_peer_assoc_prepare(ar,vif,sta,&arg,false);',
        'ath11k_peer_assoc_prepare(ar,vif,sta,&arg,false);\n\tlx_tell("awdl-auto: fw_peer_phy ht=%u rates=%u phymode=%u nss=%u\\n",arg.ht_flag,arg.peer_ht_rates.num_rates,arg.peer_phymode,arg.peer_nss);')
    mac=once(mac,'if (enable) { av->aid=vif->cfg.aid;', '''if (enable) {
        av->ps=false;
        int ps_rc=ath11k_mac_vif_setup_ps(av);
        unsigned rx_supported=test_bit(WMI_TLV_SERVICE_VDEV_RX_FILTER,ar->ab->wmi_ab.svc_map);
        int rx_rc=rx_supported?ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_RX_FILTER,0):-EOPNOTSUPP;
        lx_tell("awdl-auto: ps_disable_rc=%d rx_filter_supported=%u rx_allow_rc=%d promisc_service=%u\\n",
            ps_rc,rx_supported,rx_rc,!!test_bit(WMI_TLV_SERVICE_RX_PROMISC_ENABLE_SUPPORT,ar->ab->wmi_ab.svc_map));
        av->aid=vif->cfg.aid;''')
    # ROC is a normal firmware scan. Its default filter discards foreign
    # BSSID action traffic; request the documented scan receive mode only
    # for this owner's current ROC, without enabling monitor hardware.
    roc_at=mac.index('static int ath11k_mac_op_remain_on_channel(')
    front,tail=mac[:roc_at],mac[roc_at:]
    tail=once(tail,'arg->scan_f_passive = 1;',
        'arg->scan_f_passive = 1;\n\tif (awdl_claim_ar==ar && awdl_claim_seq) { arg->scan_f_promisc_mode = 1; arg->scan_f_offchan_mgmt_tx = 1; arg->scan_f_offchan_data_tx = 1; }')
    mac=front+tail
    mac=direct_build.apply(mac,work)
    write(macpath,mac)
    header=read(src+'roc_state.h');header=once(header,'#endif','unsigned lx_awdl_auto_frequency(struct ieee80211_hw *,struct ieee80211_vif *);\nint lx_awdl_receiver_mode(struct ieee80211_hw *,int);\n#endif')
    header=once(header,'#endif',direct_build.DECLARATIONS+'\n#endif')
    write(src+'roc_state.h',header);write(work+'src/ath11k/roc_state.h',header)
    write(work+'src/ath11k/wmi.c',read(work+'src/ath11k/wmi.c')+'\n'+(HERE/'receiver_mode.inc').read_text())
    lx=read(src+'lx_80211.c')
    # Ingress learns from all real management deliveries, not a selected host.
    marker='radio->frames++;'
    lx=once(lx,marker,marker+'\n\tif (radio->persistent_awdl && !(status->flag&(RX_FLAG_8023|RX_FLAG_FAILED_FCS_CRC|RX_FLAG_FAILED_PLCP_CRC))) auto_observe(hw,status->freq,skb->data,skb_headlen(skb));')
    lx=once(lx,marker,marker+'\n\tif (radio->persistent_awdl && radio->channel && radio->channel->def.chan && status->freq==radio->channel->def.chan->center_freq && !(status->flag&(RX_FLAG_FAILED_FCS_CRC|RX_FLAG_FAILED_PLCP_CRC))) ES(air.home_us,lx_os_nanoseconds()/1000ull);')
    write(src+'lx_80211.c',lx)
    # New cross-TU accessor is read only and scoped to this actual secondary VIF.
    withcode=read(src+'data.inc')+'\nunsigned lx_awdl_auto_frequency(struct ieee80211_hw *hw,struct ieee80211_vif *v) {return air.radio && hw==&air.radio->hw && v==air.vif?air.mhz:0;}\n'
    write(src+'data.inc',withcode)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o',work+'obj/ath11k_mac.o',work+'obj/ath11k_wmi.o',work+'obj/ath11k_dp_tx.o'],check=True)
    with (OUT/'auto-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-auto.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True)
    dest.chmod(0o600)
    (OUT/'auto-build.json').write_text(json.dumps({'object':str(dest),'sha256':sha(dest),'stage':stage,'fixed_endpoint':False},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--experimental-extra-peer',action='store_true',
        help='build the unverified extra-default-peer path (it asserted IN2010 WLAN firmware in run447)')
    args=parser.parse_args()
    if args.experimental_extra_peer:build()
    else:
        from build_single_peer import build as single_peer
        single_peer()
