#include "psf.h"

/* Called immediately before the native WMI submission. A queued work item
 * can outlive a dwell, so the admission is rechecked on the issuing worker. */
int
lx_awdl_psf_prepare(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned vdev,
    unsigned desc,unsigned frequency)
{
	struct lx_radio *radio=radio_of(hw);
	struct lx_awdl_roc_state *r=&radio->roc;
	if (__atomic_load_n(&r->tx_frame,__ATOMIC_ACQUIRE)!=skb) return 0;
	struct linuxkpi_ieee80211_channel *ch=linuxkpi_ieee80211_get_channel(hw->wiphy,frequency);
	unsigned long long now=lx_os_nanoseconds()/1000ull;
	unsigned long long anchor=ROC_GET(r->foreign_us);
	if (!ROC_GET(r->tx_permit) || !ROC_GET(r->active) || !ROC_GET(r->foreign_open) ||
	    !anchor || now<anchor || now-anchor>65536 || frequency!=2437 ||
	    vdev!=r->p2p_id || !ch || (ch->flags&(IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_RADAR|IEEE80211_CHAN_NO_IR))) {
		ROC_ADD(r->tx_cancel_before_wmi);
		return -ECANCELED;
	}
	ROC_SET(r->tx_wmi_us,lx_os_nanoseconds()/1000ull);
	ROC_SET(r->tx_desc,desc);
	ROC_SET(r->tx_bound,1);
	return 1;
}

void
lx_awdl_psf_submit_result(struct ieee80211_hw *hw,unsigned desc,int rc)
{
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	if (!ROC_GET(r->tx_bound) || ROC_GET(r->tx_desc)!=desc) return;
	ROC_SET(r->tx_submit_rc,rc);
	ROC_SET(r->tx_submit_seen,1);
}

void
lx_awdl_psf_fw(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned desc,unsigned status)
{
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	if (__atomic_load_n(&r->tx_frame,__ATOMIC_ACQUIRE)!=skb ||
	    !ROC_GET(r->tx_bound) || ROC_GET(r->tx_desc)!=desc) return;
	ROC_SET(r->tx_fw_us,lx_os_nanoseconds()/1000ull);
	ROC_SET(r->tx_fw_status,status);
	ROC_ADD(r->tx_fw);
}

void
lx_awdl_psf_release(struct ieee80211_hw *hw,struct sk_buff *skb,int dropped)
{
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	struct sk_buff *expected=skb;
	if (!__atomic_compare_exchange_n(&r->tx_frame,&expected,NULL,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
	ROC_SET(r->tx_free_us,lx_os_nanoseconds()/1000ull);
	if (dropped) ROC_ADD(r->tx_dropped);
	ROC_ADD(r->tx_released);
}

static void
lx_awdl_psf_once(struct lx_radio *radio,struct ieee80211_vif *vif,
    struct linuxkpi_ieee80211_channel *ch)
{
	struct lx_awdl_roc_state *r=&radio->roc;
	if (r->choice!=47 || !ROC_GET(r->foreign_open) || !ROC_GET(r->foreign) ||
	    !ROC_GET(r->active) || r->tx_called || vif==radio->vif ||
	    vif->type!=NL80211_IFTYPE_P2P_DEVICE || ch->center_freq!=2437 ||
	    (ch->flags&(IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_RADAR|IEEE80211_CHAN_NO_IR))) return;
	if (!lx_awdl_psf_worker_independent(&radio->hw)) {
		lx_tell("awdl-psf: declined same or missing WMI worker\n"); return;
	}
	uint8_t *bytes;
	struct sk_buff *skb=frame_new(radio,LX_AWDL_PSF_LENGTH,&bytes);
	if (!skb) return;
	if (!lx_awdl_local_psf(bytes,LX_AWDL_PSF_LENGTH,vif->addr,lx_os_nanoseconds()/1000ull,
	    ROC_GET(r->foreign_us))) { dev_kfree_skb_any(skb); return; }
	struct ieee80211_tx_info *info=IEEE80211_SKB_CB(skb);
	info->control.vif=vif; info->band=ch->band;
	info->flags=IEEE80211_TX_CTL_TX_OFFCHAN | IEEE80211_TX_CTL_NO_ACK | IEEE80211_TX_CTL_REQ_TX_STATUS;
	struct ieee80211_tx_control control={.sta=NULL};
	ROC_SET(r->tx_begin_us,lx_os_nanoseconds()/1000ull);
	lx_tell("awdl-psf: begin clock=host_monotonic_us time=%llu phy=%u vdev=%u bytes=%u peer_sync=0\n",
	    ROC_GET(r->tx_begin_us),ch->center_freq,r->p2p_id,LX_AWDL_PSF_LENGTH);
	ROC_SET(r->tx_called,1);
	ROC_SET(r->tx_permit,1);
	__atomic_store_n(&r->tx_frame,skb,__ATOMIC_RELEASE);
	radio->ops->tx(&radio->hw,&control,skb);
	ROC_SET(r->tx_queue_return_us,lx_os_nanoseconds()/1000ull);
	lx_tell("awdl-psf: handed_to_mac time_us=%llu\n",ROC_GET(r->tx_queue_return_us));
}

static bool
lx_awdl_psf_quiesced(struct lx_radio *radio)
{
	struct lx_awdl_roc_state *r=&radio->roc;
	ROC_SET(r->tx_permit,0);
	unsigned long long deadline=lx_os_nanoseconds()+1200000000ull;
	while (ROC_GET(r->tx_called) && !ROC_GET(r->tx_released) && lx_os_nanoseconds()<deadline)
		lx_pause_ms(10);
	bool drained=!ROC_GET(r->tx_called) ||
	    (ROC_GET(r->tx_released)==1 && (__atomic_load_n(&r->tx_frame,__ATOMIC_ACQUIRE)==NULL) &&
	    (ROC_GET(r->tx_fw)==1 || ROC_GET(r->tx_dropped)==1));
	lx_tell("awdl-psf: local_submit=%u vdev=%u offchan=%u desc_bound=%u desc=%u wmi_result_seen=%u wmi_rc=%d fw_complete=%u fw_status=%u drop=%u cancel_before_wmi=%u released=%u drained=%u broadcast=1\n",
	    ROC_GET(r->tx_called),r->p2p_id,r->target_mhz,ROC_GET(r->tx_bound),ROC_GET(r->tx_desc),
	    ROC_GET(r->tx_submit_seen),ROC_GET(r->tx_submit_rc),ROC_GET(r->tx_fw),ROC_GET(r->tx_fw_status),
	    ROC_GET(r->tx_dropped),ROC_GET(r->tx_cancel_before_wmi),ROC_GET(r->tx_released),drained);
	lx_tell("awdl-psf: timing clock=host_monotonic_us begin=%llu mac_return=%llu descriptor_bound=%llu fw_complete=%llu released=%llu finished=%llu\n",
	    ROC_GET(r->tx_begin_us),ROC_GET(r->tx_queue_return_us),ROC_GET(r->tx_wmi_us),ROC_GET(r->tx_fw_us),
	    ROC_GET(r->tx_free_us),lx_os_nanoseconds()/1000ull);
	return drained;
}
