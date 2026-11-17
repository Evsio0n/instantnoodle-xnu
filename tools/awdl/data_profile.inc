/* One actual descriptor per role/port state. The first primary sample may
 * be EAPOL while the normal station state machine is still opening its
 * port. Hashes, peer IDs, MAC addresses and payloads never leave this hook. */
static void lx_awdl_data_profile(struct ath11k *ar,struct ath11k_vif *vif,
    struct ath11k_sta *sta,struct hal_tx_info *ti)
{
	static unsigned seen;
	int role=lx_awdl_descriptor_role(ar->hw,vif->vif);
	if (!role || !sta) return;
	struct ieee80211_sta *ms=container_of((void *)sta,struct ieee80211_sta,drv_priv);
	unsigned rolebits=3u<<((role-1)*2);
	if ((__atomic_load_n(&seen,__ATOMIC_RELAXED)&rolebits)==rolebits) return;
	spin_lock_bh(&ar->ab->base_lock);
	struct ath11k_peer *peer=ath11k_peer_find(ar->ab,vif->vdev_id,ms->addr);
	if (!peer || peer->sta!=ms) { spin_unlock_bh(&ar->ab->base_lock); return; }
	unsigned authorized=peer->is_authorized;
	unsigned ast=(peer->ast_hash==ti->bss_ast_hash && peer->hw_peer_id==ti->bss_ast_idx);
	unsigned peermeta=FIELD_GET(HTT_TCL_META_DATA_PEER_ID,sta->tcl_metadata)==peer->peer_id;
	spin_unlock_bh(&ar->ab->base_lock);
	unsigned bit=1u<<((role-1)*2+authorized);
	if (__atomic_fetch_or(&seen,bit,__ATOMIC_RELAXED)&bit) return;
	unsigned vmeta=FIELD_GET(HTT_TCL_META_DATA_TYPE,ti->meta_data_flags)==1 &&
	    FIELD_GET(HTT_TCL_META_DATA_VDEV_ID,ti->meta_data_flags)==vif->vdev_id;
	unsigned cache=vif->vif->bss_conf.bssid && ether_addr_equal(vif->bssid,vif->vif->bss_conf.bssid);
	printk("awdl-tcl: role=%s local_port=%u vdev_meta_matches=%u peer_meta_matches=%u map_ast_matches=%u legacy_search_y=%u bssid_cache_matches=%u native=%u tid=%u\n",
	    role==1?"infra":"direct",authorized,vmeta,peermeta,ast,
	    ti->search_type==HAL_TX_ADDR_SEARCH_DEFAULT && ti->addr_search_flags==HAL_TX_ADDRY_EN,
	    cache,ti->encap_type==HAL_TCL_ENCAP_TYPE_NATIVE_WIFI,ti->tid);
}
