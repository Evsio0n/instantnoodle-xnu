"""Private ath11k extra-peer integration; the common BSS remains started."""
from build_roc import read,write,once

DECLARATIONS='''int lx_awdl_direct_configured(struct ieee80211_hw *,unsigned,const unsigned char *);
int lx_awdl_direct_peer(struct ieee80211_hw *,struct ieee80211_vif *,struct ieee80211_sta *,int *,int *);
int lx_awdl_direct_claim(struct ieee80211_hw *,struct ieee80211_vif *,struct ieee80211_sta *,int);
int lx_awdl_direct_match(struct ieee80211_hw *,struct ieee80211_vif *,struct ieee80211_sta *);'''

def apply(mac,work):
    mac=once(mac,'#include "wmi.h"','#include "wmi.h"\nstatic struct ieee80211_sta *awdl_extra_sta;\nstatic struct ieee80211_vif *awdl_extra_vif;')
    anchor='if (ab->hw_params.vdev_start_delay &&\n\t    arvif->is_started &&'
    mac=once(mac,anchor,'if (sta!=awdl_extra_sta && ab->hw_params.vdev_start_delay &&\n\t    arvif->is_started &&')
    at=mac.index('int lx_awdl_data_peer(');brace=mac.index('{',at);depth=1;end=brace+1
    while depth:
        if mac[end]=='{':depth+=1
        if mac[end]=='}':depth-=1
        end+=1
    extra=mac[at:end].replace('int lx_awdl_data_peer(', 'int lx_awdl_direct_peer(')
    extra=once(extra,'!av->is_started || av->is_up ||','!av->is_started || !av->is_up ||')
    extra=once(extra,'ath11k_peer_find(ar->ab,av->vdev_id,vif->bss_conf.bssid)','ath11k_peer_find(ar->ab,av->vdev_id,sta->addr)')
    extra=once(extra,'ieee80211_find_sta(vif,vif->bss_conf.bssid)==sta','ieee80211_find_sta(vif,sta->addr)==sta')
    extra=extra.replace('lx_awdl_echo_configured(hw,av->vdev_id,sta->addr)','lx_awdl_direct_configured(hw,av->vdev_id,sta->addr)')
    extra=once(extra,'*submit=ath11k_wmi_send_peer_assoc_cmd(ar,&arg);',
        'reinit_completion(&ar->peer_assoc_done);\n\t*submit=ath11k_wmi_send_peer_assoc_cmd(ar,&arg);')
    extra+='''
int lx_awdl_direct_claim(struct ieee80211_hw *hw,struct ieee80211_vif *v,struct ieee80211_sta *s,int enable)
{
    if(!v || !s || !lx_awdl_auto_frequency(hw,v))return -EINVAL;
    if(enable) {
        if(awdl_extra_sta && awdl_extra_sta!=s)return -EBUSY;
        awdl_extra_sta=s;awdl_extra_vif=v;
    } else {
        if(awdl_extra_sta!=s || awdl_extra_vif!=v)return -EINVAL;
        awdl_extra_sta=NULL;awdl_extra_vif=NULL;
    }
    return 0;
}
int lx_awdl_direct_match(struct ieee80211_hw *hw,struct ieee80211_vif *v,struct ieee80211_sta *s)
{return s && s==awdl_extra_sta && v==awdl_extra_vif && lx_awdl_auto_frequency(hw,v);}
'''
    dp_path=work+'src/ath11k/dp_tx.c';dp=read(dp_path)
    dp=once(dp,'lx_awdl_data_profile(ar,arvif,arsta,&ti);','''if(arsta) {
        struct ieee80211_sta *ms=container_of((void *)arsta,struct ieee80211_sta,drv_priv);
        if(lx_awdl_direct_match(ar->hw,arvif->vif,ms)) {
            /* Use the ordinary ath11k explicit peer-id/TO_FW route. */
            ti.meta_data_flags=arsta->tcl_metadata;
            ti.flags0|=FIELD_PREP(HAL_TCL_DATA_CMD_INFO1_TO_FW,1);
            spin_lock_bh(&ab->base_lock);
            struct ath11k_peer *peer=ath11k_peer_find(ab,arvif->vdev_id,ms->addr);
            if(peer && peer->sta==ms) {ti.bss_ast_hash=peer->ast_hash;ti.bss_ast_idx=peer->hw_peer_id;}
            spin_unlock_bh(&ab->base_lock);
        }
    }
    lx_awdl_data_profile(ar,arvif,arsta,&ti);''')
    write(dp_path,dp)
    return mac+'\n'+extra
