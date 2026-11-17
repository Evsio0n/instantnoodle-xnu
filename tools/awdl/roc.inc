/* One passive dwell on the existing, authorized STA. The regular station owner
 * serializes the request. IRQ callbacks only update fixed counters. */
#define ROC_GET(field) __atomic_load_n(&(field), __ATOMIC_ACQUIRE)
#define ROC_ADD(field) __atomic_fetch_add(&(field), 1, __ATOMIC_RELAXED)
#define ROC_SET(field, value) __atomic_store_n(&(field), (value), __ATOMIC_RELEASE)

void
lx_awdl_roc_wmi(struct ieee80211_hw *hw, unsigned vdev, unsigned scan, unsigned request,
    enum lx_awdl_roc_event event, unsigned frequency, unsigned reason)
{
	struct lx_awdl_roc_state *r = &radio_of(hw)->roc;
	if (!ROC_GET(r->active) || vdev != r->vdev || scan != r->scan || request != r->request) return;
	switch (event) {
	case LX_ROC_START: ROC_ADD(r->started); break;
	case LX_ROC_FOREIGN:
		if (frequency == r->target_mhz) ROC_ADD(r->foreign);
		break;
	case LX_ROC_BSS: ROC_ADD(r->bss); break;
	case LX_ROC_DONE: ROC_SET(r->reason, reason); ROC_ADD(r->done); break;
	case LX_ROC_DEQUEUED: ROC_ADD(r->dequeued); break;
	}
}

void
lx_awdl_roc_notify(struct ieee80211_hw *hw, int ready)
{
	struct lx_awdl_roc_state *r = &radio_of(hw)->roc;
	if (!ROC_GET(r->active)) return;
	if (ready) ROC_ADD(r->ready);
	else ROC_ADD(r->expired);
}

static void
lx_awdl_roc_rx(struct lx_radio *radio, struct sk_buff *skb, const struct ieee80211_rx_status *status)
{
	struct lx_awdl_roc_state *r = &radio->roc;
	if (!ROC_GET(r->active) || (status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC))) return;
	const uint8_t *p = skb->data;
	size_t n = skb_headlen(skb);
	if (status->freq == r->target_mhz && ROC_GET(r->foreign) && !ROC_GET(r->done)) {
		ROC_ADD(r->target_rx);
		if (!(status->flag & RX_FLAG_8023)) {
			enum lx_awdl_action kind = lx_awdl_action_kind(p,n);
			if (kind == LX_AWDL_PSF) ROC_ADD(r->target_psf);
			if (kind == LX_AWDL_MIF) ROC_ADD(r->target_mif);
		}
	}
	if (status->freq != r->home_mhz || !ROC_GET(r->done) || !radio->vif) return;
	if (status->flag & RX_FLAG_8023) {
		if (n >= 14 && !memcmp(p,radio->vif->addr,ETH_ALEN)) ROC_ADD(r->home_data);
	} else if (n >= 36 && p[0] == 0x80 && !memcmp(p+16,radio->target.address,ETH_ALEN)) {
		ROC_ADD(r->home_beacon);
	}
}

static void
lx_awdl_listen_once(struct lx_radio *radio)
{
	struct lx_awdl_roc_state *r = &radio->roc;
	unsigned c = r->choice;
	if (r->attempted || (c!=6 && c!=44 && c!=149)) return;
	r->attempted = 1;
	struct ieee80211_hw *hw = &radio->hw;
	if (!radio->joined || !radio->vif || !radio->channel || !radio->channel->def.chan ||
	    radio->vif->bss_conf.chanctx_conf != radio->channel ||
	    !radio->ops->remain_on_channel || !radio->ops->cancel_remain_on_channel) {
		lx_tell("awdl-roc: not armed, association/channel/ops unavailable\n"); return;
	}
	unsigned target = c == 6 ? 2437 : (c == 44 ? 5220 : 5745);
	struct linuxkpi_ieee80211_channel *chan = linuxkpi_ieee80211_get_channel(hw->wiphy,target);
	unsigned home = radio->channel->def.chan->center_freq;
	if (!chan || (chan->flags & (IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_RADAR)) || home==target) {
		lx_tell("awdl-roc: not armed home=%u target=%u eligible=%u\n",home,target,
		    !!chan && !(chan->flags & (IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_RADAR))); return;
	}
	unsigned duration = 500;
	if (hw->wiphy->max_remain_on_channel_duration < duration || !lx_awdl_roc_driver_idle(hw)) {
		lx_tell("awdl-roc: not armed, busy or insufficient dwell capability\n"); return;
	}
	r->vdev=lx_awdl_roc_vdev(radio->vif);
	r->scan=0xa000; /* ATH11K_SCAN_ID: same ID the pinned driver's ROC request uses */
	r->request=1;  /* ath11k_wmi_start_scan_init() */
	r->home_mhz=home; r->target_mhz=target;
	ROC_SET(r->active,1);
	lx_tell("awdl-roc: begin home=%u target=%u duration_ms=%u vdev=%u\n",home,target,duration,r->vdev);
	mutex_lock(&hw->wiphy->mtx);
	int rc=radio->ops->remain_on_channel(hw,radio->vif,chan,duration,IEEE80211_ROC_TYPE_NORMAL);
	mutex_unlock(&hw->wiphy->mtx);
	unsigned long long limit=lx_os_nanoseconds()+1800000000ull;
	while (!rc && (!ROC_GET(r->done) || !ROC_GET(r->expired)) && lx_os_nanoseconds()<limit)
		lx_pause_ms(10);
	int cancel=0, cancelled=0;
	if (!lx_awdl_roc_driver_idle(hw)) {
		cancelled=1;
		mutex_lock(&hw->wiphy->mtx);
		cancel=radio->ops->cancel_remain_on_channel(hw,radio->vif);
		mutex_unlock(&hw->wiphy->mtx);
	}
	limit=lx_os_nanoseconds()+1500000000ull;
	while (ROC_GET(r->done) && !ROC_GET(r->home_beacon) && !ROC_GET(r->home_data) &&
	    lx_os_nanoseconds()<limit) lx_pause_ms(10);
	ROC_SET(r->active,0);
	unsigned retained=radio->vif && radio->channel && radio->vif->bss_conf.chanctx_conf == radio->channel &&
	    radio->channel->def.chan && radio->channel->def.chan->center_freq == home;
	lx_tell("awdl-roc: event rc=%d scan=%u request=%u vdev=%u started=%u ready=%u foreign=%u bss=%u completed=%u reason=%u dequeued=%u expired=%u\n",
	    rc,r->scan,r->request,r->vdev,ROC_GET(r->started),ROC_GET(r->ready),ROC_GET(r->foreign),ROC_GET(r->bss),
	    ROC_GET(r->done),ROC_GET(r->reason),ROC_GET(r->dequeued),ROC_GET(r->expired));
	lx_tell("awdl-roc: end target_rx=%u psf=%u mif=%u cancel_used=%u cancel_rc=%d idle=%u owner_home=%u home_beacon=%u home_data=%u\n",
	    ROC_GET(r->target_rx),ROC_GET(r->target_psf),ROC_GET(r->target_mif),cancelled,cancel,
	    lx_awdl_roc_driver_idle(hw),retained,ROC_GET(r->home_beacon),ROC_GET(r->home_data));
}
#undef ROC_GET
#undef ROC_ADD
#undef ROC_SET
