/* Diagnostic second VDEV ownership. A firmware-supported P2P device has
 * a distinct MAC from the existing STA. No peer is created from a MAC
 * observed in the environment and no address is included in these logs. */
void
lx_awdl_p2p_deleted(struct ieee80211_hw *hw, unsigned vdev)
{
	struct lx_awdl_roc_state *r = &radio_of(hw)->roc;
	if (__atomic_load_n(&r->deleting,__ATOMIC_ACQUIRE) && vdev == r->p2p_id)
		__atomic_fetch_add(&r->delete_events,1,__ATOMIC_RELAXED);
}

static void
lx_awdl_listen_once(struct lx_radio *radio)
{
	struct lx_awdl_roc_state *r = &radio->roc;
	unsigned mode = r->choice;
	if (r->attempted || (mode!=6 && mode!=44 && mode!=149 && mode!=46)) return;
	r->attempted=1;
	if (mode!=46) {
		(void)lx_awdl_listen_channel(radio,radio->vif,mode);
		return;
	}
	struct ieee80211_hw *hw=&radio->hw;
	struct wiphy *w=hw->wiphy;
	if (!radio->joined || !radio->vif || !radio->channel ||
	    !(w->interface_modes & BIT(NL80211_IFTYPE_P2P_DEVICE)) ||
	    !w->addresses || w->n_addresses<2 || !hw->vif_data_size ||
	    !radio->ops->add_interface || !radio->ops->remove_interface) {
		lx_tell("awdl-p2p: unavailable station=%u p2p_mode=%u advertised_addresses=%u\n",
		    radio->joined,!!(w->interface_modes&BIT(NL80211_IFTYPE_P2P_DEVICE)),w->n_addresses);
		return;
	}
	const uint8_t *address=NULL;
	for (unsigned i=1;i<w->n_addresses;i++) {
		const uint8_t *candidate=w->addresses[i].addr;
		if (!is_zero_ether_addr(candidate) && !(candidate[0]&1) &&
		    memcmp(candidate,radio->vif->addr,ETH_ALEN)) {address=candidate;break;}
	}
	if (!address) {lx_tell("awdl-p2p: no distinct driver address\n");return;}
	unsigned sta_id=lx_awdl_roc_vdev(radio->vif);
	uint64_t before=lx_awdl_allocated_vdevs(hw);
	if (sta_id>=64 || !(before&(1ull<<sta_id)) || !lx_awdl_roc_driver_idle(hw)) {
		lx_tell("awdl-p2p: original station or scan ownership not confirmed\n");return;
	}
	void *storage=kzalloc(64+sizeof(struct ieee80211_vif)+hw->vif_data_size,GFP_KERNEL);
	if (!storage) return;
	struct ieee80211_vif *peer_vif=(void *)roundup2((uintptr_t)storage,64);
	peer_vif->type=NL80211_IFTYPE_P2P_DEVICE;
	peer_vif->p2p=true;
	memcpy(peer_vif->addr,address,ETH_ALEN);
	peer_vif->bss_conf.vif=peer_vif;
	peer_vif->bss_conf.bssid=no_network;
	peer_vif->link_conf[0]=&peer_vif->bss_conf;

	mutex_lock(&w->mtx);
	int rc=radio->ops->add_interface(hw,peer_vif);
	mutex_unlock(&w->mtx);
	uint64_t added=lx_awdl_allocated_vdevs(hw);
	unsigned id=lx_awdl_roc_vdev(peer_vif);
	bool distinct=!rc && id<64 && id!=sta_id && !(before&(1ull<<id)) &&
	    added==(before|(1ull<<id));
	lx_tell("awdl-p2p: add rc=%d station=%u requested_type=%u new_vdev=%u distinct_map=%u\n",
	    rc,sta_id,NL80211_IFTYPE_P2P_DEVICE,id,distinct);
	if (rc) {
		if (added==before) kfree(storage);
		else lx_tell("awdl-p2p: failed add did not restore VDEV map; retaining allocation\n");
		return;
	}
	r->p2p_id=id;
	__atomic_store_n(&r->visible,peer_vif,__ATOMIC_SEQ_CST);
	if (distinct) {
		static const unsigned social[]={6,44,149};
		for (unsigned i=0;i<sizeof(social)/sizeof(social[0]);i++) {
			unsigned c=social[i];
			unsigned mhz=c==6?2437:c==44?5220:5745;
			struct linuxkpi_ieee80211_channel *ch=linuxkpi_ieee80211_get_channel(w,mhz);
			if (!ch || (ch->flags&(IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_RADAR)) ||
			    radio->channel->def.chan->center_freq==mhz) {
				lx_tell("awdl-p2p: skip ineligible/same-home channel=%u\n",c);continue;
			}
			if (!lx_awdl_listen_channel(radio,peer_vif,c)) {
				lx_tell("awdl-p2p: stopped dwell series after channel=%u\n",c);break;
			}
		}
	}
	__atomic_store_n(&r->visible,NULL,__ATOMIC_SEQ_CST);
	unsigned long long limit=lx_os_nanoseconds()+1500000000ull;
	while (__atomic_load_n(&r->iterating,__ATOMIC_SEQ_CST) && lx_os_nanoseconds()<limit)
		lx_pause_ms(10);
	if (__atomic_load_n(&r->iterating,__ATOMIC_SEQ_CST) || !lx_awdl_roc_driver_idle(hw)) {
		lx_tell("awdl-p2p: unresolved iteration/ROC, retained owned VDEV and allocation\n");return;
	}
	__atomic_store_n(&r->deleting,1,__ATOMIC_RELEASE);
	mutex_lock(&w->mtx);
	radio->ops->remove_interface(hw,peer_vif);
	mutex_unlock(&w->mtx);
	__atomic_store_n(&r->deleting,0,__ATOMIC_RELEASE);
	uint64_t after=lx_awdl_allocated_vdevs(hw);
	unsigned ack=__atomic_load_n(&r->delete_events,__ATOMIC_ACQUIRE);
	bool clean=ack==1 && after==before && lx_awdl_roc_driver_idle(hw);
	lx_tell("awdl-p2p: delete vdev=%u fw_responses=%u original_map=%u station_owned=%u clean=%u\n",
	    id,ack,after==before,!!(after&(1ull<<sta_id)),clean);
	if (clean) kfree(storage);
	else lx_tell("awdl-p2p: unresolved delete; retaining allocation until reboot\n");
}
