/* Counts ingress callbacks, including deliveries lacking a channel label.
 * A label describes the driver report. It does not timestamp an air frame. */
static void
lx_awdl_window_rx(struct lx_radio *radio, struct sk_buff *skb,
    const struct ieee80211_rx_status *status)
{
	struct lx_awdl_roc_state *r = &radio->roc;
	if (!ROC_GET(r->active) || !ROC_GET(r->foreign_open)) return;
	ROC_ADD(r->rx_all);
	if (!status->freq) ROC_ADD(r->freq_unknown);
	else if (status->freq == r->home_mhz) ROC_ADD(r->freq_home);
	else if (status->freq == r->target_mhz) ROC_ADD(r->freq_target);
	else ROC_ADD(r->freq_other);
	if (status->flag & RX_FLAG_8023) ROC_ADD(r->rx_ethernet);
	else ROC_ADD(r->rx_native);
	if (status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC | RX_FLAG_MMIC_ERROR)) {
		ROC_ADD(r->rx_failed);
		return;
	}
	if (status->flag & RX_FLAG_8023) return;
	const uint8_t *p=skb->data;
	size_t n=skb_headlen(skb);
	if (n<24 || p[0]!=0xd0 || (p[1]&0x47u) || (p[22]&15u)) return;
	size_t h=(p[1]&0x80u)?28:24;
	if (n<h+16 || p[h]!=127 || p[h+1] || p[h+2]!=0x17 ||
	    p[h+3]!=0xf2 || p[h+4]!=8) return;
	enum lx_awdl_action kind=lx_awdl_action_kind(p,n);
	if (kind==LX_AWDL_PSF) ROC_ADD(r->anyfreq_psf);
	else if (kind==LX_AWDL_MIF) ROC_ADD(r->anyfreq_mif);
	else ROC_ADD(r->other_vendor_header);
}

void
lx_awdl_wmi_clock(struct ieee80211_hw *hw, unsigned frequency, const void *frame,
    size_t length, unsigned long long tick)
{
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	if (!ROC_GET(r->active) || !ROC_GET(r->foreign_open)) return;
	ROC_ADD(r->mgmt_wmi);
	if (!tick) return;
	ROC_ADD(r->mgmt_nonzero_clock);
	if (frequency != r->target_mhz || lx_awdl_action_kind(frame,length)==LX_AWDL_NONE) return;
	ROC_ADD(r->awdl_clock);
	/* RX TSF microseconds are meaningful only for a particular pdev. The
	 * nested owner restricts this comparison to a single radio and dwell. */
	unsigned long long prev=__atomic_exchange_n(&r->last_rx_tsf,tick,__ATOMIC_RELAXED);
	if (prev && tick>prev) ROC_ADD(r->awdl_clock_advances);
}
