/* The submitted data remains ordinary native Wi-Fi on its configured VDEV.
 * Use the actual confirmed firmware ROC as a bounded admission window. */
static bool lx_awdl_listen_channel(struct lx_radio *,struct ieee80211_vif *,unsigned);
static bool lx_awdl_data_foreign(struct ieee80211_hw *hw)
{
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	return EG(echo.active) && echo.hw==hw && EG(r->active) && r->target_mhz==2437 &&
	    r->vdev==echo.vdev && EG(r->ready) && EG(r->foreign_open) && !EG(r->done);
}
static void lx_awdl_data_foreign_send(struct lx_radio *radio,struct ieee80211_vif *vif,
    struct linuxkpi_ieee80211_channel *ch)
{
	struct sk_buff *skb=echo.window_frame;
	if (!skb || !echo.window_peer || !ch || ch->center_freq!=2437 ||
	    (ch->flags&(IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_RADAR)) ||
	    vif!=IEEE80211_SKB_CB(skb)->control.vif || !lx_awdl_data_foreign(&radio->hw)) return;
	struct ieee80211_tx_control control={.sta=echo.window_peer};
	echo.window_frame=NULL;
	ES(echo.receive,1); ES(echo.msdu,skb); ES(echo.submitted,1);
	ES(echo.foreign_submit,1); ES(echo.submit_us,lx_os_nanoseconds()/1000ull);
	radio->ops->tx(&radio->hw,&control,skb);
}
