/* WMI only signals. The station owner transfers the prebuilt skb while a
 * separate ordinary worker is inside the driver's synchronous ROC call. */
static bool lx_awdl_listen_channel(struct lx_radio *,struct ieee80211_vif *,unsigned);
static struct {
	struct lx_radio *radio;
	struct ieee80211_vif *vif;
	unsigned armed,finished,roc_ok,signal,bss_raw,foreign_raw,own_mhz,started,ready;
	unsigned basis,last_kind,at_submit,after_done,at_desc,at_wbm,done_reason;
	unsigned long long bss_us;
} bw;

static bool lx_awdl_data_foreign(struct ieee80211_hw *hw)
{
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	return EG(echo.active) && echo.hw==hw && EG(r->active) && r->target_mhz==2437 &&
	    r->vdev==echo.vdev && EG(r->ready) && EG(r->foreign_open) && !EG(r->done);
}
static unsigned lx_awdl_home_basis(struct ieee80211_hw *hw)
{
	if (!EG(bw.armed) || !EG(echo.active) || echo.hw!=hw || !EG(bw.ready) || !EG(bw.started)) return 0;
	if (lx_awdl_data_foreign(hw)) return 1;
	if (EG(bw.last_kind)!=2 || EG(bw.own_mhz)!=2437) return 0;
	unsigned long long now=lx_os_nanoseconds()/1000ull, stamp=EG(bw.bss_us);
	/* A completed scan returns the carrier to its BSS. Classify a very
	 * recent return separately from a still-running scan. Aborts close it. */
	return stamp && now>=stamp && now-stamp<=32000 ? 2:0;
}
static void lx_awdl_window_signal(struct ieee80211_hw *hw,unsigned vdev,unsigned kind,
    unsigned freq,unsigned reason)
{
	if (!EG(bw.armed) || !EG(echo.active) || echo.hw!=hw || echo.vdev!=vdev) return;
	if (kind==LX_ROC_START) ES(bw.started,1);
	else if (kind==LX_ROC_FOREIGN) { ES(bw.foreign_raw,freq); ES(bw.last_kind,freq==2437?1:0); }
	else if (kind==LX_ROC_BSS) {
		ES(bw.bss_raw,freq); ES(bw.bss_us,lx_os_nanoseconds()/1000ull); ES(bw.last_kind,2);
	} else if (kind==LX_ROC_DONE) {
		ES(bw.done_reason,reason);
		if (reason || EG(bw.last_kind)!=2) ES(bw.last_kind,0);
	} else if (kind==LX_ROC_DEQUEUED) ES(bw.last_kind,0);
	EA(bw.signal); lx_os_wakeup(&bw.signal);
}
static void lx_awdl_window_worker(void *unused)
{
	(void)unused;
	bool ok=lx_awdl_listen_channel(bw.radio,bw.vif,6);
	ES(bw.roc_ok,ok); ES(bw.finished,1); EA(bw.signal); lx_os_wakeup(&bw.signal);
}
static int lx_awdl_station_window(struct lx_radio *radio,struct ieee80211_vif *vif,
    struct linuxkpi_ieee80211_channel *ch)
{
	struct ieee80211_hw *hw=&radio->hw;
	if (!vif->bss_conf.chanctx_conf || vif->bss_conf.chanctx_conf!=echo.ctx ||
	    vif->bss_conf.chanctx_conf->def.chan!=ch || !ch || ch->center_freq!=2437 ||
	    (ch->flags&(IEEE80211_CHAN_RADAR|IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_DISABLED)) ||
	    !EG(echo.start) || EG(echo.start_status) || EG(echo.stop) ||
	    vif==radio->vif || lx_awdl_roc_vdev(vif)!=echo.vdev) return 0;
	bw.radio=radio; bw.vif=vif; bw.own_mhz=ch->center_freq;
	ES(bw.armed,1);
	int rc=lx_os_thread("wifi-awdl-roc",lx_awdl_window_worker,NULL);
	if (rc) { ES(bw.armed,0); return 0; }
	unsigned long long limit=lx_os_nanoseconds()+8500000000ull;
	while (!EG(bw.finished) && lx_os_nanoseconds()<limit) {
		unsigned changed=EG(bw.signal);
		unsigned basis=lx_awdl_home_basis(hw);
		if (echo.window_frame && basis) {
			struct ieee80211_tx_control control={.sta=echo.window_peer};
			struct sk_buff *skb=echo.window_frame;
			echo.window_frame=NULL;
			ES(echo.receive,1); ES(echo.msdu,skb); ES(echo.submitted,1);
			ES(echo.foreign_submit,basis==1); ES(bw.at_submit,basis);
			ES(bw.after_done,EG(radio->roc.done)!=0);
			ES(echo.submit_us,lx_os_nanoseconds()/1000ull);
			radio->ops->tx(hw,&control,skb);
		}
		if (EG(echo.submitted)) lx_awdl_data_poll(hw);
		lx_os_lock();
		if (!EG(bw.finished) && changed==EG(bw.signal)) lx_os_sleep(&bw.signal,1000000ull);
		lx_os_unlock();
	}
	if (!EG(bw.finished)) return -1;
	ES(bw.armed,0);
	lx_tell("awdl-echo: chanctx_mhz=%u matched_start=%u matched_ready=%u bss_raw_freq=%u bss_freq_valid=0 foreign_raw_freq=%u submit_basis=%u desc_basis=%u wbm_basis=%u after_scan_done_at_tx=%u rf_freq_measured_at_tx=%u roc_rc_success=%u\n",
	    bw.own_mhz,EG(bw.started),EG(bw.ready),EG(bw.bss_raw),EG(bw.foreign_raw),
	    EG(bw.at_submit),EG(bw.at_desc),EG(bw.at_wbm),EG(bw.after_done),EG(bw.at_submit)==1,EG(bw.roc_ok));
	return EG(bw.roc_ok) ? 1:0;
}
