/* The shared protocol BSSID and a discovered data station are separate
 * firmware peers. Keep the former up while mapping the actual link endpoint.
 * These operations configure our own open link; they send no association
 * response or claim that the remote device joined an infrastructure BSS. */
static bool auto_direct_close(struct lx_radio *r,struct ieee80211_vif *v)
{
	if(!direct.storage) return true;
	uint64_t until=lx_os_nanoseconds()+6500000000ull;
	while(live_count_flights(0) && lx_os_nanoseconds()<until) {
		live_retire();live_deliver();auto_maintain();lx_pause_ms(2);
	}
	if(live_count_flights(0))return false;
	mutex_lock(&r->wiphy->mtx);
	int port=direct.authorized?r->ops->sta_state(&r->hw,v,direct.sta,IEEE80211_STA_AUTHORIZED,IEEE80211_STA_ASSOC):0;
	int del=direct.created?r->ops->sta_state(&r->hw,v,direct.sta,IEEE80211_STA_NONE,IEEE80211_STA_NOTEXIST):0;
	mutex_unlock(&r->wiphy->mtx);
	lx_tell("awdl-auto: direct_retired port_rc=%d peer_delete_rc=%d common_vdev_stopped=%u\n",port,del,EG(echo.stop));
	if(port || del || EG(echo.stop)) return false;
	(void)lx_awdl_direct_claim(&r->hw,v,direct.sta,0);
	ES(direct.pending,0);kfree(direct.storage);memset(&direct,0,sizeof(direct));
	return true;
}
static int auto_direct_open(struct lx_radio *r,struct ieee80211_vif *v,struct ieee80211_sta *bss,
    struct linuxkpi_ieee80211_channel *ch)
{
	struct ieee80211_hw *hw=&r->hw;
	void *storage=kzalloc(sizeof(struct ieee80211_sta)+hw->sta_data_size+64,GFP_KERNEL);
	if(!storage)return -ENOMEM;
	struct ieee80211_sta *sta=(void *)roundup2((uintptr_t)storage,64);
	memcpy(sta->addr,active_peer_mac,6);memcpy(sta->deflink.addr,active_peer_mac,6);
	sta->deflink.sta=sta;sta->link[0]=&sta->deflink;
	sta->deflink.ht_cap=bss->deflink.ht_cap;sta->deflink.rx_nss=1;
	sta->deflink.bandwidth=IEEE80211_STA_RX_BW_20;sta->wme=bss->wme;
	sta->deflink.supp_rates[ch->band]=bss->deflink.supp_rates[ch->band];
	memset(&direct,0,sizeof(direct));direct.hw=hw;direct.vif=v;direct.sta=sta;direct.storage=storage;
	memcpy(direct.mac,active_peer_mac,6);
	int create=lx_awdl_direct_claim(hw,v,sta,1),cfg=-ECANCELED,submit=-ECANCELED,confirm=0,port=-ECANCELED;
	if(!create) {
		mutex_lock(&r->wiphy->mtx);
		create=r->ops->sta_state(hw,v,sta,IEEE80211_STA_NOTEXIST,IEEE80211_STA_NONE);
		if(!create) {
			direct.created=1;ES(direct.pending,1);
			lookup_vif=v;lookup_peer=sta;lookup_alt_peer=bss;ES(lookup_thread,lx_os_thread_self());
			cfg=lx_awdl_direct_peer(hw,v,sta,&submit,&confirm);
			ES(lookup_thread,NULL);lookup_vif=NULL;lookup_peer=lookup_alt_peer=NULL;ES(direct.pending,0);
			if(!cfg) {
				port=r->ops->sta_state(hw,v,sta,IEEE80211_STA_ASSOC,IEEE80211_STA_AUTHORIZED);
				if(!port)direct.authorized=1;
			}
		}
		mutex_unlock(&r->wiphy->mtx);
	}
	lx_tell("awdl-auto: direct_peer create_rc=%d cfg_rc=%d fw_submit_rc=%d confirmed=%u authorize_rc=%d common_bssid_retained=1\n",
	    create,cfg,submit,confirm,port);
	if(create || cfg || port) {
		if(!auto_direct_close(r,v))return -EBUSY;
		return create?create:cfg?cfg:port;
	}
	live.peer=sta;
	return 0;
}
