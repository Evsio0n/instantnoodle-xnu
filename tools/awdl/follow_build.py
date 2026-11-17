"""Private RAW/observed-clock profile; no additional firmware peers."""
from build_roc import read,write,once,HERE

def function(source,name,new):
    at=source.index(name+'(');start=source.rfind('static ',0,at)
    brace=source.index('{',at);depth=1;end=brace+1
    while depth:
        if source[end]=='{':depth+=1
        if source[end]=='}':depth-=1
        end+=1
    return source[:start]+new+'\n'+source[end:]

def apply(stage,raw=True):
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    write(src+'awdl_discovery.h',(HERE/'awdl_discovery.h').read_text())
    write(src+'peer_table.h',(HERE/'peer_table.h').read_text())
    write(src+'peer_projection.h',(HERE/'peer_projection.h').read_text())
    peers=read(src+'auto_peers.inc')
    peers=once(peers,'#include "peer_table.h"','#include "peer_table.h"\n#include "peer_projection.h"\n#include "awdl_discovery.h"')
    peers=function(peers,'auto_phy_chan','''static unsigned auto_phy_chan(const struct lx_awdl_peer *p,uint64_t now)
{
    (void)now;unsigned best=0,score=0;
    /* Keep a carrier on the predominant actual advertised data channel.
     * A social discovery reception alone does not select its data channel. */
    for(unsigned i=0;i<16;i++) {
        unsigned ch=p->clock.slots[i],count=0;if(!ch || !auto_channel(air.radio,ch,true))continue;
        for(unsigned j=0;j<16;j++)count+=p->clock.slots[j]==ch;
        if(count>score){score=count;best=ch;}
    }
    if(best)return best;
    unsigned heard=channel_of(p->last_rx_mhz);return auto_channel(air.radio,heard,true)?heard:0;
}''')
    peers+='''
static uint64_t follow_last_discovery;
/* Called inside the native FIFO's existing mutex. Do not lock recursively. */
static bool follow_multicast_due_locked(uint64_t now)
{
    return air.active && air.table.peers[air.active_slot].serial==air.active_serial &&
        awdl_peer_multicast_due(&air.table.peers[air.active_slot],now);
}
static bool follow_data_window(uint64_t now,unsigned channel)
{
    if(!air.active)return false;
    lx_os_lock();struct lx_awdl_peer p=air.table.peers[air.active_slot];lx_os_unlock();
    struct awdl_window window;
    return p.serial==air.active_serial && awdl_peer_project(&p,now,8,&window) &&
        window.channel==channel && window.in_guarded_interval;
}
static void follow_announcement(struct lx_awdl_discovery_window *w,unsigned channel)
{
    w->channel=channel;w->social_mask=0;
    if(auto_channel(air.radio,6,false))w->social_mask|=1;
    if(auto_channel(air.radio,44,false))w->social_mask|=2;
    if(auto_channel(air.radio,149,false))w->social_mask|=4;
    w->dualband_supported=auto_channel(air.radio,6,false) &&
        (auto_channel(air.radio,36,false) || auto_channel(air.radio,44,false) ||
         auto_channel(air.radio,48,false) || auto_channel(air.radio,149,false));
    if(!air.active)return;
    lx_os_lock();struct lx_awdl_peer p=air.table.peers[air.active_slot];lx_os_unlock();
    if(p.serial!=air.active_serial || p.distance>=9 || !p.clock.from_peer ||
       p.clock.period_tu!=16 || p.clock.presence!=4 || w->now_us<p.last_rx_us ||
       w->now_us-p.last_rx_us>=LX_AWDL_PEER_TTL_US)return;
    /* Join the actually observed cluster as a leaf. No hostname, remote
     * IP or election identity is supplied from a host-side target file. */
    w->anchor_us=p.clock.anchor_us;w->base_aw=p.clock.base_aw;w->follow_master=true;w->master_channel=p.master_channel;
    memcpy(w->master,p.master,6);memcpy(w->sync_address,p.mac,6);
    w->master_counter=p.master_counter;w->master_metric=p.master_metric;w->distance=p.distance+1;
}
'''
    write(src+'auto_peers.inc',peers)
    owner=read(src+'auto_owner.inc')
    old='''if(!air.prepeer && ch->center_freq!=2437 && (++cycles%3)==1) {
            if(!auto_window(r,v,6)) {ES(live.stop,1);break;}
            continue;
        }'''
    # Tabs in the frozen C source are significant only to the checked anchor.
    old=old.replace('            ','\t\t\t').replace('        }','\t\t}')
    owner=once(owner,old,'''if(!air.prepeer && ch->center_freq!=2437 &&
            lx_os_nanoseconds()/1000ull-follow_last_discovery>=1500000ull) {
            if(!auto_window(r,v,6)) {ES(live.stop,1);break;}
            follow_last_discovery=lx_os_nanoseconds()/1000ull;continue;
        }''')
    ready_start=owner.index('if(!EG(live.ready) && EG(live_wmi_psf) && EG(live_wmi_mif)) {')
    ready_end=owner.index('\n\t\tfor(unsigned i=0;i<4',ready_start)
    ready=owner[ready_start:ready_end]
    owner=owner[:ready_start]+'auto_mark_ready();'+owner[ready_end:]
    owner=once(owner,'static bool auto_data_service(',
        'static void auto_mark_ready(void)\n{\n\t'+ready+'\n}\n\nstatic bool auto_data_service(')
    # Home-channel service does not pass the off-channel ROC tail. It must
    # publish the SAME actual PSF/MIF-completion readiness, otherwise native
    # CREATE waits forever despite a working registered radio owner.
    owner=once(owner,'live_tick(r,v,ch,lx_awdl_home_basis(&r->hw));lx_pause_ms(2);continue;',
        'live_tick(r,v,ch,lx_awdl_home_basis(&r->hw));auto_mark_ready();lx_pause_ms(2);continue;')
    write(src+'auto_owner.inc',owner)
    live=read(src+'live_transport.inc')
    live=once(live,'live_multicast_due(live.anchor_us,now)', 'follow_multicast_due_locked(now)')
    live=once(live,'if (!basis || !EG(live.accept) || !live.peer || air.prepeer || !auto_active_fresh()) return;',
        'if (!basis || !EG(live.accept) || !live.peer || air.prepeer || !auto_active_fresh() || !follow_data_window(lx_os_nanoseconds()/1000ull,channel_of(ch->center_freq))) return;')
    live=once(live,'!lx_awdl_data_foreign(hw) || EG(live.stop)',
        '(!lx_awdl_data_foreign(hw) && !(freq==0 && lx_awdl_home_basis(hw)==3)) || EG(live.stop)')
    live=once(live,'freq!=air.listen_mhz ||','(freq!=air.listen_mhz && !(freq==0 && lx_awdl_home_basis(hw)==3)) ||')
    live=once(live,'uint64_t now=lx_os_nanoseconds()/1000ull, began=EG(r->roc.foreign_us);',
        'uint64_t now=lx_os_nanoseconds()/1000ull, began=EG(r->roc.foreign_us);\n\tbool home=lx_awdl_home_basis(&r->hw)==3;\n\tif(home)began=now;')
    live=once(live,'!lx_awdl_data_foreign(&r->hw) ||', '(!home && !lx_awdl_data_foreign(&r->hw)) ||')
    live=once(live,'.grant_end_us=began+475000,','.grant_end_us=began+(home?131072:475000),')
    live=once(live,'const struct ieee80211_sta_ht_cap *ht=&r->wiphy->bands[ch->band]->ht_cap;',
        'follow_announcement(&w,channel_of(ch->center_freq));\n\tconst struct ieee80211_sta_ht_cap *ht=&r->wiphy->bands[ch->band]->ht_cap;')
    live=once(live,'IEEE80211_TX_CTL_REQ_TX_STATUS|IEEE80211_TX_CTL_TX_OFFCHAN|IEEE80211_TX_CTL_NO_ACK;',
        'IEEE80211_TX_CTL_REQ_TX_STATUS|(home?0:IEEE80211_TX_CTL_TX_OFFCHAN)|IEEE80211_TX_CTL_NO_ACK;')
    live=once(live,'if (basis==1 && live_infra_window && ch->center_freq==2437) live_discovery(r,v,ch);',
        'if ((basis==1 && live_infra_window) || basis==3) live_discovery(r,v,ch);')
    live=once(live,'out->preferred=air.active?air.channel:6;',
        'out->preferred=air.active?air.channel:6;out->peer_master=air.active?air.table.peers[air.active_slot].master_channel:0;')
    write(src+'live_transport.inc',live)
    # The peer clock/channel/leaf profile also works with normal native-WiFi
    # descriptors. RAW is a separate firmware experiment, not a prerequisite
    # for following an observed master's availability windows.
    if raw:apply_raw(stage)

def apply_raw(stage):
    """Apply the existing per-secondary-VDEV RAW transport profile."""
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    mac_path=work+'src/ath11k/mac.c';mac=read(mac_path)
    mac+='''
int lx_awdl_raw_prepare(struct ieee80211_hw *hw,struct ieee80211_vif *v)
{
    struct ath11k *ar=hw->priv;struct ath11k_vif *av=ath11k_vif_to_arvif(v);
    if(!v->p2p || av->vdev_subtype!=WMI_VDEV_SUBTYPE_P2P_CLIENT || av->is_started || av->is_up)return -EINVAL;
    mutex_lock(&ar->conf_mutex);
    int tx=ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_TX_ENCAP_TYPE,ATH11K_HW_TXRX_RAW);
    int rx=ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_RX_DECAP_TYPE,ATH11K_HW_TXRX_RAW);
    mutex_unlock(&ar->conf_mutex);
    lx_tell("awdl-auto: raw_before_start tx_rc=%d rx_rc=%d\\n",tx,rx);
    return tx?tx:rx;
}
'''
    anchor='av->aid=vif->cfg.aid; ether_addr_copy(av->bssid,vif->bss_conf.bssid);'
    mac=once(mac,anchor,'''if(lx_awdl_raw_vdev(ar->hw,av->vdev_id)) {
        int raw_tx=ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_TX_ENCAP_TYPE,ATH11K_HW_TXRX_RAW);
        int raw_rx=ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_RX_DECAP_TYPE,ATH11K_HW_TXRX_RAW);
        lx_tell("awdl-auto: raw_vdev tx_submit_rc=%d rx_submit_rc=%d scope=secondary_only\\n",raw_tx,raw_rx);
        }
        '''+anchor)
    write(mac_path,mac)
    header=read(src+'roc_state.h')
    header=once(header,'#endif','int lx_awdl_raw_prepare(struct ieee80211_hw *,struct ieee80211_vif *);\nint lx_awdl_raw_vdev(struct ieee80211_hw *,unsigned);\n#endif')
    write(src+'roc_state.h',header);write(work+'src/ath11k/roc_state.h',header)
    data=read(src+'data.inc')+'''
int lx_awdl_raw_vdev(struct ieee80211_hw *hw,unsigned id)
{return air.radio && hw==&air.radio->hw && EG(air.running) && air.vif && lx_awdl_roc_vdev(air.vif)==id;}
'''
    write(src+'data.inc',data)
    owner=read(src+'auto_owner.inc')
    marker='lx_tell("awdl-auto: started rc=%d fixed_targets=0 vdev=%u mtu=%u\\n",rc,id,LIVE_MTU);'
    owner=once(owner,marker,marker+'\n\tint raw_setup=lx_awdl_raw_prepare(hw,v);\n\tif(raw_setup)ES(live.stop,1);')
    write(src+'auto_owner.inc',owner)
    wmi_path=work+'src/ath11k/wmi.c';wmi=read(wmi_path)
    wmi=once(wmi,'if (test_bit(ATH11K_FLAG_HW_CRYPTO_DISABLED, &ar->ab->dev_flags))\n\t\tcmd->flags |= WMI_VDEV_START_HW_ENCRYPTION_DISABLED;',
        'if (test_bit(ATH11K_FLAG_HW_CRYPTO_DISABLED, &ar->ab->dev_flags) || lx_awdl_raw_vdev(ar->hw,arg->vdev_id))\n\t\tcmd->flags |= WMI_VDEV_START_HW_ENCRYPTION_DISABLED;')
    write(wmi_path,wmi)
    dp_path=work+'src/ath11k/dp_tx.c';dp=read(dp_path)
    dp=once(dp,'ti.encap_type = ath11k_dp_tx_get_encap_type(arvif, skb);',
        'ti.encap_type = ath11k_dp_tx_get_encap_type(arvif, skb);\n\tif(lx_awdl_descriptor_role(ar->hw,arvif->vif)==2)ti.encap_type=HAL_TCL_ENCAP_TYPE_RAW;')
    dp=once(dp,'if (!test_bit(ATH11K_FLAG_RAW_MODE, &ab->dev_flags)) {',
        'if (!test_bit(ATH11K_FLAG_RAW_MODE, &ab->dev_flags) && lx_awdl_descriptor_role(ar->hw,arvif->vif)!=2) {')
    write(dp_path,dp)
