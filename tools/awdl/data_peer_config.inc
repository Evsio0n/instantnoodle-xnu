/* Configure the local chipset's rate context for this one open direct-link
 * peer. The caller never reports an infrastructure association response.
 * No key or external network security state is installed. */
int lx_awdl_data_peer(struct ieee80211_hw *hw,struct ieee80211_vif *vif,
    struct ieee80211_sta *sta,int *submit,int *confirm)
{
	struct ath11k *ar=hw->priv;
	struct ath11k_vif *av=ath11k_vif_to_arvif(vif);
	struct peer_assoc_params arg;
	struct cfg80211_chan_def ch;
	int rc=-EINVAL;
	if (!vif->p2p || av->vdev_subtype!=WMI_VDEV_SUBTYPE_P2P_CLIENT ||
	    !lx_awdl_echo_ctx(hw,vif->bss_conf.chanctx_conf)) return rc;
	mutex_lock(&ar->conf_mutex);
	if (av->ar!=ar || !av->is_started || av->is_up || ath11k_mac_vif_chan(vif,&ch) ||
	    !ch.chan || ch.chan->center_freq!=2437) goto out;
	spin_lock_bh(&ar->ab->base_lock);
	struct ath11k_peer *peer=ath11k_peer_find(ar->ab,av->vdev_id,sta->addr);
	bool mapped=peer && peer->sta==sta;
	spin_unlock_bh(&ar->ab->base_lock);
	if (!mapped) goto out;
	struct ieee80211_supported_band *band=hw->wiphy->bands[ch.chan->band];
	if (!band) goto out;
	u32 basic=0;
	for (int i=0;i<band->n_bitrates && i<32;i++) {
		unsigned rate=band->bitrates[i].bitrate;
		if (rate==60 || rate==120 || rate==240) basic |= BIT(i);
	}
	if (!basic) goto out;
	sta->deflink.supp_rates[ch.chan->band]=basic;
	sta->deflink.rx_nss=1;
	vif->bss_conf.basic_rates=basic;
	ath11k_peer_assoc_prepare(ar,vif,sta,&arg,false);
	if (!arg.peer_legacy_rates.num_rates || arg.need_ptk_4_way || arg.need_gtk_2_way ||
	    arg.is_pmf_enabled) goto out;
	arg.is_assoc=true;
	/* The peer's identity was provisioned locally, not authenticated by an
	 * infrastructure association. Do not mark its controlled port authorized. */
	arg.auth_flag=false;
	*submit=ath11k_wmi_send_peer_assoc_cmd(ar,&arg);
	rc=*submit;
	if (!rc) {
		(void)wait_for_completion_timeout(&ar->peer_assoc_done,HZ);
		*confirm=lx_awdl_echo_configured(hw,av->vdev_id,sta->addr);
		rc=*confirm?0:-ETIMEDOUT;
	}
out:
	mutex_unlock(&ar->conf_mutex);
	return rc;
}
