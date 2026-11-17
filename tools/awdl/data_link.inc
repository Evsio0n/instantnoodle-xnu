/* SDL/AWDL is a direct link. Configure only our own already-started VDEV
 * with the AWDL network BSSID. No 802.11 AP association state or AID is
 * invented for the remote peer. WMI command return is local submission. */
int lx_awdl_data_link(struct ieee80211_hw *hw,struct ieee80211_vif *vif,
    struct ieee80211_sta *sta,int enable)
{
	struct ath11k *ar=hw->priv;
	struct ath11k_vif *av=ath11k_vif_to_arvif(vif);
	int rc=-EINVAL;
	if (!vif->p2p || av->vdev_subtype!=WMI_VDEV_SUBTYPE_P2P_CLIENT ||
	    !lx_awdl_echo_ctx(hw,vif->bss_conf.chanctx_conf)) return rc;
	mutex_lock(&ar->conf_mutex);
	if (av->ar!=ar || !av->is_started || (enable != !av->is_up)) goto out;
	spin_lock_bh(&ar->ab->base_lock);
	struct ath11k_peer *peer=ath11k_peer_find(ar->ab,av->vdev_id,sta->addr);
	bool mapped=peer && peer->sta==sta;
	spin_unlock_bh(&ar->ab->base_lock);
	if (!mapped) goto out;
	if (enable) rc=ath11k_wmi_vdev_up(ar,av->vdev_id,0,vif->bss_conf.bssid,NULL,0,0);
	else rc=ath11k_wmi_vdev_down(ar,av->vdev_id);
	if (!rc) av->is_up=enable;
out:
	mutex_unlock(&ar->conf_mutex);
	return rc;
}
